#ifndef CM_CACHE_MIRAGE_MSI_HPP
#define CM_CACHE_MIRAGE_MSI_HPP

#include <stack>
#include "cache/coherence.hpp"
#include "cache/msi.hpp"

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

// Mirage 
// IW: index width, NW: number of ways, EW: extra tag ways, P: number of partitions, MaxRelocN: max number of relocations
// MT: metadata type, DT: data type (void if not in use), DTMT: data meta type 
// MIDX: indexer type, MRPC: replacer type
// DIDX: data indexer type, DRPC: data replacer type
// EnMon: whether to enable monitoring
// EnableRelocation : whether to enable relocation
template<int IW, int NW, int EW, int P, int MaxRelocN, typename MT, typename DT,
         typename DTMT, typename MIDX, typename DIDX, typename MRPC, typename DRPC, typename DLY, bool EnMon, bool EnableRelocation,
         bool EnMT = false, int MSHR = 4>
  requires C_DERIVE<MT, MetadataBroadcastBase, MirageMetadataSupport> && C_DERIVE_OR_VOID<DT, CMDataBase> &&
           C_DERIVE<DTMT, MirageDataMeta>  && C_DERIVE<MIDX, IndexFuncBase>   && C_DERIVE<DIDX, IndexFuncBase> &&
           C_DERIVE_OR_VOID<DLY, DelayBase>
class MirageCache : public CacheSkewed<IW, NW+EW, P, MT, void, MIDX, MRPC, DLY, EnMon, EnMT, MSHR>
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
    d_s =  d_indexer.index(addr, 0);
    d_replacer.replace(d_s, &d_w);
    return std::make_pair(d_s, d_w);
  }

  virtual bool replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, uint16_t prio, unsigned int genre = 0) override {
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
    replacer[*ai].replace(*s, w);
    return true; // ToDo: support multithread
  }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, const CMMetadataBase * meta, const CMDataBase *data, uint64_t *delay) override {
    if(ai < P) {
      auto [ds, dw] = static_cast<MT *>(this->access(ai, s, w))->pointer();
      d_replacer.access(ds, dw, true, false);
    }
    CacheT::hook_read(addr, ai, s, w, hit, meta, data, delay);
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool demand_acc, const CMMetadataBase * meta, const CMDataBase *data, uint64_t *delay) override {
    if(ai < P) {
      auto [ds, dw] = static_cast<MT *>(this->access(ai, s, w))->pointer();
      d_replacer.access(ds, dw, demand_acc, false);
    }
    CacheT::hook_write(addr, ai, s, w, hit, demand_acc, meta, data, delay);
  }

  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, const CMMetadataBase * meta, const CMDataBase *data, uint64_t *delay) override {
    if(ai < P && hit && evict) {
      auto [ds, dw] = static_cast<MT *>(this->access(ai, s, w))->pointer();
      d_replacer.invalid(ds, dw);
    }
    CacheT::hook_manage(addr, ai, s, w, hit, evict, writeback, meta, data, delay);
  }

  void cuckoo_search(uint32_t *ai, uint32_t *s, uint32_t *w, CMMetadataBase* &meta, std::stack<std::tuple<uint32_t, uint32_t, uint32_t> > &stack) {
    if constexpr (EnableRelocation) {
      uint32_t relocation = 0;
      uint32_t m_ai, m_s, m_w;
      std::unordered_set<uint64_t> remapped;
      auto addr = meta->addr(*s);
      while(meta->is_valid() && relocation++ < MaxRelocN){
        m_ai = (*ai+1)%P; // Do we need total random selection of partition here, does not matter for Mirage as P=2
        m_s  = indexer.index(addr, m_ai);
        replacer[m_ai].replace(m_s, &m_w);
        auto m_meta = static_cast<MT *>(this->access(m_ai, m_s, m_w));
        auto m_addr = m_meta->addr(m_s);
        if(remapped.count(m_addr)) break; // ToDo: here will break the replace state! allocate() without access()
        remapped.insert(addr);
        stack.push(std::make_tuple(*ai, *s, *w));
        std::tie(meta, addr, *ai, *s, *w) = std::make_tuple(m_meta, m_addr, m_ai, m_s, m_w);
      }
    }
  }

  void cuckoo_relocate(uint32_t *ai, uint32_t *s, uint32_t *w, std::stack<std::tuple<uint32_t, uint32_t, uint32_t> > &stack, uint64_t *delay) {
    while(!stack.empty()) {
      auto [m_ai, m_s, m_w] = stack.top(); stack.pop();
      auto [meta, addr] = this->relocate(m_ai, m_s, m_w, *ai, *s, *w);
      get_data_meta(static_cast<MT *>(meta))->bind(*ai, *s, *w);
      CacheT::hook_manage(addr, m_ai, m_s, m_w, true, true, false, nullptr, nullptr, delay);
      CacheT::hook_read(addr, *ai, *s, *w, false, nullptr, nullptr, delay); // read or write? // hit is true or false? may have impact on delay
      std::tie(*ai, *s, *w) = std::make_tuple(m_ai, m_s, m_w);
    }
  }

};

// uncached MSI inner port:
//   no support for reverse probe as if there is no internal cache
//   or the interl cache does not participate in the coherence communication
template<typename MT, typename CT, bool EnMT>
  requires C_DERIVE<MT, MetadataBroadcastBase, MirageMetadataSupport> && C_DERIVE<CT, CacheBase>
class MirageInnerPortUncached : public InnerCohPortUncached<EnMT>
{
protected:
  using InnerCohPortBase::policy;
  using InnerCohPortBase::outer;
public:
  MirageInnerPortUncached(policy_ptr policy) : InnerCohPortUncached<EnMT>(policy) {}

protected:
  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, bool>
  access_line(uint64_t addr, coh_cmd_t cmd, uint16_t prio, uint64_t *delay) override { // common function for access a line in the cache
    uint32_t ai, s, w;
    CMMetadataBase *meta;
    CMDataBase *data;
    auto cache = static_cast<CT *>(InnerCohPortBase::cache);
    bool hit = cache->hit(addr, &ai, &s, &w, prio, EnMT);
    if(hit) {
      std::tie(meta, data) = cache->access_line(ai, s, w);
      auto sync = policy->access_need_sync(cmd, meta);
      if(sync.first) {
        auto [phit, pwb] = this->probe_req(addr, meta, data, sync.second, delay); // sync if necessary
        if(pwb) cache->hook_write(addr, ai, s, w, true, false, meta, data, delay); // a write occurred during the probe
      }
      auto [promote, promote_local, promote_cmd] = policy->access_need_promote(cmd, meta);
      if(promote) { outer->acquire_req(addr, meta, data, promote_cmd, delay); hit = false; } // promote permission if needed
      else if(promote_local) meta->to_modified(-1);
    } else { // miss
      // do the cuckoo replacement
      cache->replace(addr, &ai, &s, &w, prio);
      meta = static_cast<CMMetadataBase *>(cache->access(ai, s, w));
      std::stack<std::tuple<uint32_t, uint32_t, uint32_t> > stack;
      if(meta->is_valid()) cache->cuckoo_search(&ai, &s, &w, meta, stack);
      if(meta->is_valid()) { // associative eviction!
        this->evict(meta, cache->get_data_data(static_cast<MT *>(meta)), ai, s, w, delay);
        cache->get_data_meta(static_cast<MT *>(meta))->to_invalid();
      }
      cache->cuckoo_relocate(&ai, &s, &w, stack, delay);
      meta = static_cast<CMMetadataBase *>(cache->access(ai, s, w));
      auto data_pointer = cache->replace_data(addr);
      auto data_meta = cache->get_data_meta(data_pointer);
      data = cache->get_data_data(data_pointer);
      if(data_meta->is_valid()) {
        auto meta_pointer = data_meta->pointer();
        auto replace_meta = cache->get_meta_meta(meta_pointer);
        this->evict(replace_meta, data, std::get<0>(meta_pointer), std::get<1>(meta_pointer), std::get<2>(meta_pointer), delay);
        replace_meta->to_invalid();
      }
      static_cast<MT *>(meta)->bind(data_pointer.first, data_pointer.second);
      data_meta->bind(ai, s, w);

      outer->acquire_req(addr, meta, data, policy->cmd_for_outer_acquire(cmd), delay); // fetch the missing block
    }
    return std::make_tuple(meta, data, ai, s, w, hit);
  }
};

template<typename MT, typename CT, bool EnMT>
  requires C_DERIVE<MT, MetadataBroadcastBase, MirageMetadataSupport> && C_DERIVE<CT, CacheBase>
using MirageInnerCohPort = InnerCohPortT<MirageInnerPortUncached<MT, CT, EnMT>, EnMT>;

#endif
