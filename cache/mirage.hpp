#ifndef CM_CACHE_MIRAGE_MSI_HPP
#define CM_CACHE_MIRAGE_MSI_HPP

#include <stack>
#include "cache/coherence.hpp"
#include "cache/msi.hpp"
#include "rsa/monitor.hpp"
class MirageDataMeta : public CMMetadataCommon
{
protected:
  bool state = false; // false: invalid, true: valid
  uint32_t mai = 0, ms = 0, mw = 0; // data meta pointer to meta

public:
  __always_inline void bind(uint32_t ai, uint32_t s, uint32_t w) { mai = ai; ms = s; mw = w; state = true; }
  __always_inline std::tuple<uint32_t, uint32_t, uint32_t> pointer() { return std::make_tuple(mai, ms, mw);} // return the pointer to data
  virtual void to_invalid() override { state = false; }
  virtual void to_extend() override {} // dummy to meet the virtual class requirement
  virtual bool is_valid() const override { return state; }
  virtual bool match(uint64_t addr) const override { return false; } // dummy to meet the virtual class requirement
  virtual void copy(const MirageDataMeta *meta) { std::tie(state, mai, ms, mw) = std::make_tuple(meta->state, meta->mai, meta->ms, meta->mw); }
};

// metadata supporting MSI coherency
class MirageMetadataSupport
{
protected:
  uint32_t ds = 0, dw = 0;
public:
  // special methods needed for Mirage Cache
  __always_inline void bind(uint32_t s, uint32_t w) { ds = s; dw = w; }                     // initialize meta pointer
  __always_inline std::pair<uint32_t, uint32_t> pointer() { return std::make_pair(ds, dw);} // return meta pointer to data meta
  virtual std::string to_string() const { return (boost::format("(%04d,%02d)") % ds % dw).str(); }
};

// Metadata with match function
// AW    : address width
// IW    : index width
// TOfst : tag offset
template <int AW, int IW, int TOfst>
class MirageMetadataMSIBroadcast : public MetadataBroadcast<AW, IW, TOfst, MetadataMSIBase<MetadataBroadcastBase> >, public MirageMetadataSupport
{
  typedef MetadataBroadcast<AW, IW, TOfst, MetadataMSIBase<MetadataBroadcastBase> > MetadataT;
public:
  virtual std::string to_string() const override { return CMMetadataBase::to_string() + MirageMetadataSupport::to_string(); }

  virtual void copy(const CMMetadataBase *m_meta) override {
    MetadataT::copy(m_meta);
    auto meta = static_cast<const MirageMetadataMSIBroadcast<AW, IW, TOfst> *>(m_meta);
    std::tie(ds, dw) = std::make_tuple(meta->ds, meta->dw);
  }
};

// MirageMSI protocol
template<typename MT, typename CT, typename Outer>
  requires C_DERIVE<MT, MetadataBroadcastBase, MirageMetadataSupport> && C_DERIVE<CT, CacheBase>
struct MirageMSIPolicy : public MSIPolicy<false, true, Outer> // always LLC, always not L1
{
  static __always_inline void meta_after_flush(coh_cmd_t cmd, CMMetadataBase *meta, CacheBase *cache) {
    assert(coh::is_flush(cmd));
    if(coh::is_evict(cmd)) {
      static_cast<CT *>(cache)->get_data_meta(static_cast<MT *>(meta))->to_invalid();
      meta->to_invalid();
    }
  }
};

struct MirageHelper {
  static const unsigned int replace_for_relocate = 202410140ul;
};

// Mirage 
// IW: index width, NW: number of ways, EW: extra tag ways, P: number of partitions, MaxRelocN: max number of relocations
// MT: metadata type, DT: data type (void if not in use), DTMT: data meta type 
// MIDX: indexer type, MRPC: replacer type
// DIDX: data indexer type, DRPC: data replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, int EW, int P, typename MT, typename DT,
         typename DTMT, typename MIDX, typename DIDX, typename MRPC, typename DRPC, typename DLY, bool EnMon,
         bool DTEF = false, bool EnMT = false, int MSHR = 4>
  requires C_DERIVE<MT, MetadataBroadcastBase, MirageMetadataSupport> && C_DERIVE_OR_VOID<DT, CMDataBase> &&
           C_DERIVE<DTMT, MirageDataMeta>  && C_DERIVE<MIDX, IndexFuncBase>   && C_DERIVE<DIDX, IndexFuncBase> &&
           C_DERIVE_OR_VOID<DLY, DelayBase>
class MirageCache : public CacheSkewed<IW, NW+EW, P, MT, void, MIDX, MRPC, DLY, EnMon, EnMT, MSHR>, protected MirageHelper
{
// see: https://www.usenix.org/system/files/sec21fall-saileshwar.pdf

  typedef CacheSkewed<IW, NW+EW, P, MT, void, MIDX, MRPC, DLY, EnMon, EnMT, MSHR> CacheT;
protected:
  using CacheBase::arrays;
  using CacheT::indexer;
  using CacheT::loc_random;
  using CacheT::replacer;
  DIDX d_indexer;   // data index resolver
  DRPC d_replacer;  // data replacer

  uint32_t next_ai(uint32_t ai) {return (ai+1)%P;} // Do we need total random selection of partition here, does not matter for Mirage as P=2

  virtual void replace_choose_set(uint64_t addr, uint32_t *ai, uint32_t *s, unsigned int genre) override {
    if(replace_for_relocate == genre){
      *ai = next_ai(*ai);
      *s = indexer.index(addr, *ai);
      return;
    }
    int max_free = -1, p = 0;
    std::vector<std::pair<uint32_t, uint32_t> > candidates(P);
    uint32_t m_s;
    for(int i=0; i<P; i++) {
      m_s = indexer.index(addr, i);
      int free_num = replacer[i].get_free_num(m_s);
      if(free_num > max_free) { p = 0; max_free = free_num; }
      if(free_num >= max_free)
        candidates[p++] = std::make_pair(i, m_s);
    }
    std::tie(*ai, *s) = candidates[(*loc_random)() % p];
  }

public:
  MirageCache(std::string name = "") : CacheT(name, 1)
  { 
    // CacheMirage has P+1 CacheArray
    arrays[P] = new CacheArrayNorm<IW,P*NW,DTMT,DT,EnMT>(); // the separated data array

    // allocate data buffer pool as the DT given to CacheDkewed is void
    if constexpr (!C_VOID<DT>) {
      CacheT::data_buffer_pool.resize(MSHR, nullptr);
      for(auto &b : CacheT::data_buffer_pool) { b = new DT(); CacheT::data_buffer_pool_set.insert(b); }
    }
  }

  __always_inline CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w) {
    auto pointer = static_cast<MT *>(this->access(ai, s, w))->pointer();
    return arrays[P]->get_data(pointer.first, pointer.second);
  }

  virtual std::pair<CMMetadataBase *, CMDataBase *> access_line(uint32_t ai, uint32_t s, uint32_t w) override {
    auto meta = static_cast<CMMetadataBase *>(arrays[ai]->get_meta(s, w));
    if constexpr (!C_VOID<DT>)
      return std::make_pair(meta, get_data_data(static_cast<MT *>(meta)));
    else
      return std::make_pair(meta, nullptr);
  }

  __always_inline MirageDataMeta *get_data_meta(const std::pair<uint32_t, uint32_t>& pointer) {
    return static_cast<MirageDataMeta *>(arrays[P]->get_meta(pointer.first, pointer.second));
  }

  __always_inline CMDataBase *get_data_data(const std::pair<uint32_t, uint32_t>& pointer) {
    return arrays[P]->get_data(pointer.first, pointer.second);
  }

  __always_inline CMMetadataBase *get_meta_meta(const std::tuple<uint32_t, uint32_t, uint32_t>& pointer) {
    return static_cast<CMMetadataBase *>(this->access(std::get<0>(pointer), std::get<1>(pointer), std::get<2>(pointer)));
  }

  // grammer sugar
  __always_inline MirageDataMeta *get_data_meta(MT *meta)   { return get_data_meta(meta->pointer()); }
  __always_inline CMDataBase     *get_data_data(MT *meta)   { return get_data_data(meta->pointer()); }

  __always_inline std::pair<uint32_t, uint32_t> replace_data(uint64_t addr) {
    uint32_t d_s, d_w;
    if constexpr (DTEF) {
      std::vector<uint32_t> free_set;
      for(uint64_t i = 0; i < 1ul<<IW; i++){
        if(d_replacer.get_free_num(i))
          free_set.push_back(i);
      }
      d_s = free_set.empty() ? 
            (*loc_random)() % (1ul << IW) : 
            free_set[(*loc_random)() % free_set.size()];
      d_replacer.replace(d_s, &d_w);
    } else{
      d_s =  (*loc_random)() % (1ul << IW);
      d_replacer.replace(d_s, &d_w, false);
    }
    return std::make_pair(d_s, d_w);
  }

  virtual void replace_read(uint32_t ai, uint32_t s, uint32_t w, bool prefetch, bool genre = false) override {
    if(!genre && ai < P) {
      auto [ds, dw] = static_cast<MT *>(this->access(ai, s, w))->pointer();
      d_replacer.access(ds, dw, true, prefetch);
    }
    CacheT::replace_read(ai, s, w, prefetch);
  }

  virtual void replace_write(uint32_t ai, uint32_t s, uint32_t w, bool demand_acc, bool genre = false) override {
    if(!genre && ai < P) {
      auto [ds, dw] = static_cast<MT *>(this->access(ai, s, w))->pointer();
      d_replacer.access(ds, dw, demand_acc, false);
    }
    CacheT::replace_write(ai, s, w, demand_acc);
  }

  virtual void replace_manage(uint32_t ai, uint32_t s, uint32_t w, bool hit, uint32_t evict, bool genre = false) override {
    if(!genre && ai < P && hit && evict) {
      auto [ds, dw] = static_cast<MT *>(this->access(ai, s, w))->pointer();
      d_replacer.invalid(ds, dw);
    }
    CacheT::replace_manage(ai, s, w, hit, evict);
  }

  bool pre_finish_reloc(uint64_t addr, uint32_t s_ai, uint32_t s_s, uint32_t ai){
    return (s_ai == next_ai(ai)) && (s_s == indexer.index(addr, next_ai(ai)));
  }

};

// uncached MSI inner port:
//   no support for reverse probe as if there is no internal cache
//   or the interl cache does not participate in the coherence communication
template<typename Policy, bool EnMT, typename MT, typename CT, bool EnableRelocation, int MaxRelocN>
  requires C_DERIVE<MT, MetadataBroadcastBase, MirageMetadataSupport> && C_DERIVE<CT, CacheBase> && C_DERIVE<Policy, CohPolicyBase>
class MirageInnerPortUncached : public InnerCohPortUncached<Policy, EnMT>, protected MirageHelper
{
  typedef InnerCohPortUncached<Policy, EnMT> Inner;

protected:
  using InnerCohPortBase::outer;

  void bind_data(uint64_t addr, CMMetadataBase *meta, CMDataBase *&data, int32_t ai, uint32_t s, uint32_t w, uint64_t *delay){
    auto cache = static_cast<CT *>(InnerCohPortBase::cache);
    auto data_pointer = cache->replace_data(addr);
    auto data_meta = cache->get_data_meta(data_pointer);
    data = cache->get_data_data(data_pointer);
    if(data_meta->is_valid()) {
      auto meta_pointer = data_meta->pointer();
      auto replace_meta = cache->get_meta_meta(meta_pointer);
      global_evict(replace_meta, data, std::get<0>(meta_pointer), std::get<1>(meta_pointer), std::get<2>(meta_pointer), delay);
      replace_meta->to_invalid();
    }
    static_cast<MT *>(meta)->bind(data_pointer.first, data_pointer.second);
    data_meta->bind(ai, s, w);
  }

  virtual void evict(CMMetadataBase *meta, CMDataBase *data, int32_t ai, uint32_t s, uint32_t w, uint64_t *delay) {
    // evict a block due to conflict
    // do the cuckoo relocation
    if constexpr (EnableRelocation) {
      auto cache = static_cast<CT *>(InnerCohPortBase::cache);
      uint32_t relocation = 0;
      uint32_t m_ai = ai; uint32_t m_s, m_w;
      auto buf_meta = cache->meta_copy_buffer();
      auto addr = meta->addr(s);
      cache->relocate(addr, meta, buf_meta);
      cache->replace_manage(ai, s, w, true, 1, true);
      while(buf_meta->is_valid()){
        relocation++;
        cache->replace(addr, &m_ai, &m_s, &m_w, XactPrio::acquire, replace_for_relocate);
        auto m_meta = static_cast<MT *>(cache->access(m_ai, m_s, m_w));
        auto m_addr = m_meta->addr(m_s);
        if (m_meta->is_valid()) {
          if (relocation >= MaxRelocN || cache->pre_finish_reloc(m_addr, ai, s, m_ai))
            global_evict(m_meta, cache->get_data_data(static_cast<MT *>(m_meta)), m_ai, m_s, m_w, delay); // associative eviction!
          else
            cache->replace_manage(m_ai, m_s, m_w, true, 1, true);
        }
        cache->swap(m_addr, addr, m_meta, buf_meta, nullptr, nullptr);
        cache->get_data_meta(static_cast<MT *>(m_meta))->bind(m_ai, m_s, m_w);
        cache->replace_read(m_ai, m_s, m_w, false, true);
        cache->monitor_magic_func(addr, EvictDistRelocEvent, &m_s);
        addr = m_addr;
      }
      cache->meta_return_buffer(buf_meta);
    }
    else{
      global_evict(meta, data, ai, s, w, delay);
    }
  }

  void global_evict(CMMetadataBase *meta, CMDataBase *data, int32_t ai, uint32_t s, uint32_t w, uint64_t *delay) {
    // global evict a block due to conflict
    Inner::evict(meta, data, ai, s, w, delay);
    static_cast<CT *>(InnerCohPortBase::cache)->get_data_meta(static_cast<MT *>(meta))->to_invalid();
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, bool>
  access_line(uint64_t addr, coh_cmd_t cmd, uint16_t prio, uint64_t *delay) override { // common function for access a line in the cache
    auto cache = static_cast<CT *>(InnerCohPortBase::cache);
    auto [hit, meta, data, ai, s, w] = this->check_hit_or_replace(addr, prio, true, delay);
    if(hit) {
      auto sync = Policy::access_need_sync(cmd, meta);
      if(sync.first) {
        auto [phit, pwb] = this->probe_req(addr, meta, data, sync.second, delay); // sync if necessary
        if(pwb){
          cache->replace_write(ai, s, w, false);
          cache->hook_write(addr, ai, s, w, true, meta, data, delay); // a write occurred during the probe
        }
      }
      auto [promote, promote_local, promote_cmd] = Policy::access_need_promote(cmd, meta);
      if(promote) { outer->acquire_req(addr, meta, data, promote_cmd, delay); hit = false; } // promote permission if needed
      else if(promote_local) meta->to_modified(-1);
    } else { // miss
      if(meta->is_valid()) evict(meta, data, ai, s, w, delay);
      bind_data(addr, meta, data, ai, s, w, delay);
      outer->acquire_req(addr, meta, data, coh::is_prefetch(cmd) ? cmd : Policy::cmd_for_outer_acquire(cmd), delay); // fetch the missing block
    }
    return std::make_tuple(meta, data, ai, s, w, hit);
  }

  virtual void flush_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) {
    auto [hit, meta, data, ai, s, w] = this->check_hit_or_replace(addr, XactPrio::flush, false, delay);
    if(!hit) return;
    Inner::flush_line(addr, cmd, delay);
    static_cast<CT *>(InnerCohPortBase::cache)->get_data_meta(static_cast<MT *>(meta))->to_invalid();
  }
};

// template helper for pass the compiler
template<bool EnReloc = true, int MaxRelocN = 3>
struct MirageInnerCohPortUncachedT {
  template<typename Policy, bool EnMT, typename MT, typename CT>
  using type = MirageInnerPortUncached<Policy, EnMT, MT, CT, EnReloc, MaxRelocN>;
};

#endif
