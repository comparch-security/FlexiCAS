#ifndef CM_CACHE_MIRAGE_MSI_HPP
#define CM_CACHE_MIRAGE_MSI_HPP

#include <stack>
#include "cache/coherence.hpp"
#include "cache/msi.hpp"

class MirageDataMeta : public CMMetadataCommon
{
protected:
  bool state; // false: invalid, true: valid
  uint32_t mai, ms, mw; // data meta pointer to meta

public:
  MirageDataMeta() : state(false), mai(0), ms(0), mw(0) {}
  void bind(uint32_t ai, uint32_t s, uint32_t w) { mai = ai; ms = s; mw = w; state = true; }
  std::tuple<uint32_t, uint32_t, uint32_t> pointer() { return std::make_tuple(mai, ms, mw);} // return the pointer to data
  virtual void to_invalid() { state = false; }
  virtual void to_extend() {} // dummy to meet the virtual class requirement
  virtual bool is_valid() const { return state; }
  virtual bool match(uint64_t addr) const { return false; } // dummy to meet the virtual class requirement
  virtual void copy(const MirageDataMeta *meta) {
    std::tie(state, mai, ms, mw) = std::make_tuple(meta->state, meta->mai, meta->ms, meta->mw);
  }

  virtual ~MirageDataMeta() {}
};

// metadata supporting MSI coherency
class MirageMetadataSupport
{
protected:
  uint32_t ds, dw;
public:
  MirageMetadataSupport() : ds(0), dw(0) {}
  virtual ~MirageMetadataSupport() {}

  // special methods needed for Mirage Cache
  void bind(uint32_t s, uint32_t w) { ds = s; dw = w; }                     // initialize meta pointer
  std::pair<uint32_t, uint32_t> pointer() { return std::make_pair(ds, dw);} // return meta pointer to data meta
  virtual std::string to_string() const;
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
  MirageMetadataMSIBroadcast() : MetadataT(), MirageMetadataSupport() {}
  virtual ~MirageMetadataMSIBroadcast() {}

  virtual std::string to_string() const {
    return CMMetadataBase::to_string() + MirageMetadataSupport::to_string();
  }

  virtual void copy(const CMMetadataBase *m_meta) {
    MetadataT::copy(m_meta);
    auto meta = static_cast<const MirageMetadataMSIBroadcast<AW, IW, TOfst> *>(m_meta);
    std::tie(ds, dw) = std::make_tuple(meta->ds, meta->dw);
  }
};

// MirageMSI protocol
template<typename MT, typename CT>
  requires C_DERIVE<MT, MetadataBroadcastBase, MirageMetadataSupport> && C_DERIVE<CT, CacheBase>
class MirageMSIPolicy : public MSIPolicy<MT, false, true> // always LLC, always not L1
{
  using CohPolicyBase::is_flush;
  using CohPolicyBase::is_evict;
public:
  MirageMSIPolicy() : MSIPolicy<MT, false, true>() {}
  virtual ~MirageMSIPolicy() {}

  virtual void meta_after_flush(coh_cmd_t cmd, CMMetadataBase *meta) const {
    assert(is_flush(cmd));
    if(is_evict(cmd)) invalidate_cache_line(meta);
  }

private:
  void invalidate_cache_line(CMMetadataBase *meta) const {
    static_cast<CT *>(CohPolicyBase::cache)->get_data_meta(static_cast<MT *>(meta))->to_invalid();
    meta->to_invalid();
  }

};

// Mirage 
// IW: index width, NW: number of ways, EW: extra tag ways, P: number of partitions, MaxRelocN: max number of relocations
// MT: metadata type, DT: data type (void if not in use), DTMT: data meta type 
// MIDX: indexer type, MRPC: replacer type
// DIDX: data indexer type, DRPC: data replacer type
// EnMon: whether to enable monitoring
// EnableRelocation : whether to enable relocation
// EF: empty first in replacer
template<int IW, int NW, int EW, int P, int MaxRelocN, typename MT, typename DT,
         typename DTMT, typename MIDX, typename DIDX, typename MRPC, typename DRPC, typename DLY, bool EnMon, bool EnableRelocation,
         bool EF = true, bool EnMT = false, int MSHR = 4>
  requires C_DERIVE<MT, MetadataBroadcastBase, MirageMetadataSupport> && C_DERIVE_OR_VOID<DT, CMDataBase> &&
           C_DERIVE<DTMT, MirageDataMeta>  && C_DERIVE<MIDX, IndexFuncBase>   && C_DERIVE<DIDX, IndexFuncBase> &&
C_DERIVE<MRPC, ReplaceFuncBase<EF,false> > && C_DERIVE<DRPC, ReplaceFuncBase<EF,false> > && C_DERIVE_OR_VOID<DLY, DelayBase>
class MirageCache : public CacheSkewed<IW, NW+EW, P, MT, void, MIDX, MRPC, DLY, EnMon, EF, EnMT, MSHR>
{
// see: https://www.usenix.org/system/files/sec21fall-saileshwar.pdf

  typedef CacheSkewed<IW, NW+EW, P, MT, void, MIDX, MRPC, DLY, EnMon, EF, EnMT, MSHR> CacheT;
protected:
  using CacheT::arrays;
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

  virtual ~MirageCache() {}

  virtual CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w) {
    auto pointer = static_cast<MT *>(this->access(ai, s, w))->pointer();
    return arrays[P]->get_data(pointer.first, pointer.second);
  }

  virtual std::pair<CMMetadataBase *, CMDataBase *> access_line(uint32_t ai, uint32_t s, uint32_t w) {
    auto meta = static_cast<CMMetadataBase *>(arrays[ai]->get_meta(s, w));
    if constexpr (!C_VOID<DT>)
      return std::make_pair(meta, get_data_data(static_cast<MT *>(meta)));
    else
      return std::make_pair(meta, nullptr);
  }

  MirageDataMeta *get_data_meta(const std::pair<uint32_t, uint32_t>& pointer) {
    return static_cast<MirageDataMeta *>(arrays[P]->get_meta(pointer.first, pointer.second));
  }

  CMDataBase *get_data_data(const std::pair<uint32_t, uint32_t>& pointer) {
    return arrays[P]->get_data(pointer.first, pointer.second);
  }

  CMMetadataBase *get_meta_meta(const std::tuple<uint32_t, uint32_t, uint32_t>& pointer) {
    return static_cast<CMMetadataBase *>(this->access(std::get<0>(pointer), std::get<1>(pointer), std::get<2>(pointer)));
  }

  // grammer sugar
  MirageDataMeta *get_data_meta(MT *meta)   { return get_data_meta(meta->pointer()); }
  CMDataBase     *get_data_data(MT *meta)   { return get_data_data(meta->pointer()); }

  std::pair<uint32_t, uint32_t> replace_data(uint64_t addr) {
    uint32_t d_s, d_w;
    d_s =  d_indexer.index(addr, 0);
    d_replacer.replace(d_s, &d_w);
    return std::make_pair(d_s, d_w);
  }

  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, unsigned int genre = 0) {
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
  }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, const CMMetadataBase * meta, const CMDataBase *data, uint64_t *delay) {
    if(ai < P) {
      auto [ds, dw] = static_cast<MT *>(this->access(ai, s, w))->pointer();
      d_replacer.access(ds, dw, false);
    }
    CacheT::hook_read(addr, ai, s, w, hit, meta, data, delay);
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool is_release, const CMMetadataBase * meta, const CMDataBase *data, uint64_t *delay) {
    if(ai < P) {
      auto [ds, dw] = static_cast<MT *>(this->access(ai, s, w))->pointer();
      d_replacer.access(ds, dw, is_release);
    }
    CacheT::hook_write(addr, ai, s, w, hit, is_release, meta, data, delay);
  }

  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, const CMMetadataBase * meta, const CMDataBase *data, uint64_t *delay) {
    if(ai < P && hit && evict) {
      auto [ds, dw] = static_cast<MT *>(this->access(ai, s, w))->pointer();
      d_replacer.invalid(ds, dw);
    }
    CacheT::hook_manage(addr, ai, s, w, hit, evict, writeback, meta, data, delay);
  }

  void cuckoo_search(uint32_t *ai, uint32_t *s, uint32_t *w, CMMetadataBase* &meta, std::stack<std::tuple<uint32_t, uint32_t, uint32_t> > &stack){
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
        if(remapped.count(m_addr)) break;
        remapped.insert(addr);
        stack.push(std::make_tuple(*ai, *s, *w));
        std::tie(meta, addr, *ai, *s, *w) = std::make_tuple(m_meta, m_addr, m_ai, m_s, m_w);
      }
    }
  }

  void cuckoo_relocate(uint32_t *ai, uint32_t *s, uint32_t *w, CMMetadataBase* &meta, std::stack<std::tuple<uint32_t, uint32_t, uint32_t> > &stack, uint64_t *delay) {
    auto [m_ai, m_s, m_w] = stack.top(); stack.pop();
    auto m_meta = static_cast<MT *>(this->access(m_ai, m_s, m_w));
    auto addr = m_meta->addr(m_s);
    meta->init(addr); meta->copy(m_meta); m_meta->to_clean(); m_meta->to_invalid();
    get_data_meta(static_cast<MT *>(meta))->bind(*ai, *s, *w);
    CacheT::hook_manage(addr, m_ai, m_s, m_w, true, true, false, nullptr, nullptr, delay);
    CacheT::hook_read(addr, *ai, *s, *w, false, nullptr, nullptr, delay); // hit is true or false? may have impact on delay
    std::tie(*ai, *s, *w, meta) = std::make_tuple(m_ai, m_s, m_w, m_meta);
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
  typedef InnerCohPortUncached<EnMT> BaseT;
public:
  MirageInnerPortUncached(policy_ptr policy) : InnerCohPortUncached<EnMT>(policy) {}
protected:
  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t>
  replace_line(uint64_t addr, uint64_t *delay) {
    uint32_t ai, s, w;
    CMMetadataBase *meta;
    CMDataBase *data;
    auto cache = static_cast<CT *>(InnerCohPortBase::cache);
    // get the way to be replaced
    cache->replace(addr, &ai, &s, &w);
    meta = static_cast<CMMetadataBase *>(cache->access(ai, s, w));
    std::stack<std::tuple<uint32_t, uint32_t, uint32_t> > stack;
    if(meta->is_valid()) cache->cuckoo_search(&ai, &s, &w, meta, stack);
    if(meta->is_valid()) { // associative eviction!
      BaseT::evict(meta, cache->get_data_data(static_cast<MT *>(meta)), ai, s, w, delay);
      cache->get_data_meta(static_cast<MT *>(meta))->to_invalid();
    }
    while(!stack.empty()) cache->cuckoo_relocate(&ai, &s, &w, meta, stack, delay);
    meta = static_cast<CMMetadataBase *>(cache->access(ai, s, w));
    auto data_pointer = cache->replace_data(addr);
    auto data_meta = cache->get_data_meta(data_pointer);
    data = cache->get_data_data(data_pointer);
    if(data_meta->is_valid()) {
      auto meta_pointer = data_meta->pointer();
      auto replace_meta = cache->get_meta_meta(meta_pointer);
      BaseT::evict(replace_meta, data, std::get<0>(meta_pointer), std::get<1>(meta_pointer), std::get<2>(meta_pointer), delay);
      replace_meta->to_invalid();
    }
    static_cast<MT *>(meta)->bind(data_pointer.first, data_pointer.second);
    data_meta->bind(ai, s, w);
    return std::make_tuple(meta, data, ai, s, w);
  }
};

template<typename MT, typename CT, bool EnMT>
  requires C_DERIVE<MT, MetadataBroadcastBase, MirageMetadataSupport> && C_DERIVE<CT, CacheBase>
using MirageInnerCohPort = InnerCohPortT<MirageInnerPortUncached<MT, CT, EnMT>, EnMT>;

#endif
