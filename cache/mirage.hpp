#ifndef CM_CACHE_MIRAGE_MSI_HPP
#define CM_CACHE_MIRAGE_MSI_HPP

#include <stack>
#include "cache/msi.hpp"

class MirageDataMeta : public CMMetadataBase
{
protected:
  bool state; // false: invalid, true: valid
  uint32_t mai, ms, mw; // data meta pointer to meta

public:
  MirageDataMeta() : state(false), mai(0), ms(0), mw(0) {}
  void bind(uint32_t ai, uint32_t s, uint32_t w) { mai = ai; ms = s; mw = w; state = true; }
  std::tuple<uint32_t, uint32_t, uint32_t> pointer() { return std::make_tuple(mai, ms, mw);} // return the pointer to data
  virtual void to_invalid() { state = false; }
  virtual bool is_valid() const { return state; }
  
  virtual void copy(const CMMetadataBase *m_meta) {
    auto meta = static_cast<MirageDataMeta *>(m_meta);
    auto [state, mai, ms, mw] = {meta->state, meta->mai, meta->ms, meta->mw};
  }

  virtual ~MirageDataMeta() {}

private:
  virtual void to_shared() {}
  virtual void to_modified() {}
  virtual void to_owned() {}
  virtual void to_exclusive() {}
  virtual void to_dirty() {}
  virtual void to_clean() {}
  virtual void init(uint64_t addr) {}
  virtual uint64_t addr(uint32_t s) const { return 0; }
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
};

// Metadata with match function
// AW    : address width
// IW    : index width
// TOfst : tag offset
template <int AW, int IW, int TOfst>
class MirageMetadataMSI : public MetadataMSI<AW, IW, TOfst>, public MirageMetadataSupport
{
public:
  MirageMetadataMSI() : MetadataMSI(), MirageMetadataSupport() {}
  virtual ~MirageMetadataMSI() {}

  virtual void copy(const CMMetadataBase *m_meta) {
    MetadataMSI::copy(m_meta);
    auto meta = static_cast<MirageMetadataMSI *>(m_meta);
    auto [ds, dw] = {meta->ds, meta->dw};
  }
};

// MirageMSI protocol
template<typename MT, typename CT,
         typename = typename std::enable_if<std::is_base_of<MetadataMSIBase, MT>::value>::type,       // MT <- MetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<MetadataMSISupport, MT>::value>::type,    // MT <- MetadataMSISupport
         typename = typename std::enable_if<std::is_base_of<MirageMetadataSupport, MT>::value>::type, // MT <- MirageMetadataSupport
         typename = typename std::enable_if<std::is_base_of<CacheBase, CT>::value>::type>             // CT <- CacheBase
class MirageMSIPolicy : public MSIPolicy<MT, false, true> // always LLC, always not L1
{
public:
  MirageMSIPolicy() : MSIPolicy() {}
  virtual ~MirageMSIPolicy() {}

  virtual void meta_after_probe(coh_cmd_t cmd, CMMetadataBase *meta) const {
    assert(is_probe(cmd));
    if(is_evict(cmd)) invalidate_cache_line(meta);
    else {
      assert(is_writeback(cmd));
      meta->to_shared();
    }
  }

  virtual void meta_after_flush(coh_cmd_t cmd, CMMetadataBase *meta) const  {
    assert(is_flush(cmd));
    if(is_evict(cmd)) invalidate_cache_line(meta);
  }

private:
  inline void invalidate_cache_line(CMMetadataBase *meta) const {
    static_cast<CT *>(cache)->get_data_meta(static_cast<MT *>(meta))->to_invalid();
    meta->to_invalid();
  }

};

// Mirage 
// IW: index width, NW: number of ways, EW: extra tag ways, P: number of partitions, RW: max number of relocations
// MT: metadata type, DT: data type (void if not in use), DTMT: data meta type 
// MIDX: indexer type, MRPC: replacer type
// DIDX: data indexer type, DRPC: data replacer type
// EnMon: whether to enable monitoring
// EnableRelocation : whether to enable relocation
template<int IW, int NW, int EW, int P, int RW, typename MT, typename DT,
         typename DTMT, typename MIDX, typename DIDX, typename MRPC, typename DRPC, typename DLY, bool EnMon, bool EnableRelocation, 
         typename = typename std::enable_if<std::is_base_of<MetadataMSIBase, MT>::value>::type,  // MT <- MetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<MirageMetadataSupport, MT>::value>::type,  // MT <- MirageMetadataSupport
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type, // DT <- CMDataBase or void
         typename = typename std::enable_if<std::is_base_of<CMMetadataBase, DTMT>::value>::type,  // DTMT <- MirageDataMeta
         typename = typename std::enable_if<std::is_base_of<IndexFuncBase, MIDX>::value>::type,  // MIDX <- IndexFuncBase
         typename = typename std::enable_if<std::is_base_of<IndexFuncBase, DIDX>::value>::type,  // DIDX <- IndexFuncBase
         typename = typename std::enable_if<std::is_base_of<ReplaceFuncBase, MRPC>::value>::type,  // MRPC <- ReplaceFuncBase
         typename = typename std::enable_if<std::is_base_of<ReplaceFuncBase, DRPC>::value>::type,  // DRPC <- ReplaceFuncBase
         typename = typename std::enable_if<std::is_base_of<DelayBase, DLY>::value || std::is_void<DLY>::value>::type>  // DLY <- DelayBase or void
class CacheMirage : public CacheSkewed<IW, NW+EW, P, MT, void, MIDX, MRPC, DLY, EnMon>
{
// see: https://www.usenix.org/system/files/sec21fall-saileshwar.pdf
protected:
  DIDX d_indexer;   // data index resolver
  DRPC d_replacer;  // data replacer

public:
  CacheMirage(std::string name = "")
    : CacheSkewed(name, 1), MirageCacheSupport()
  { 
    // CacheMirage has P+1 CacheArray
    arrays[P] = new CacheArrayNorm<IW,P*NW,DTMT,DT>(); // the separated data array
  }

  virtual ~CacheMirage() {}

  virtual CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w) {
    auto pointer = static_cast<MT *>(access(ai, s, w))->pointer();
    return arrays[P]->get_data(pointer.first, pointer.second);
  }

  virtual std::pair<CMMetadataBase *, CMDataBase *> access_line(uint32_t ai, uint32_t s, uint32_t w) {
    auto meta = static_cast<MT *>(arrays[ai]->get_meta(s, w));
    if constexpr (!std::is_void<DT>::value)
      return std::make_pair(meta, get_data_data(meta));
    else
      return std::make_pair(meta, nullptr);
  }

  inline MirageDataMeta *get_data_meta(const std::pair<uint32_t, uint32_t>& pointer) {
    return static_cast<MirageDataMeta *>(arrays[P]->get_meta(pointer.first, pointer.second));
  }

  inline CMDataBase *get_data_data(const std::pair<uint32_t, uint32_t>& pointer) {
    return arrays[P]->get_data(pointer.first, pointer.second);
  }

  inline CMMetadataBase *get_meta_meta(const std::tuple<uint32_t, uint32_t, uint32_t>& pointer) {
    return arrays[std::get<0>(pointer)]->get_meta(std::get<1>(pointer), std::get<2>(pointer));
  }

  // grammer sugar
  inline MirageDataMeta *get_data_meta(const MT *meta)   { return get_data_meta(meta->pointer()); }
  inline CMDataBase     *get_data_data(const MT *meta)   { return get_data_data(meta->pointer()); }
  inline CMMetadataBase *get_meta_meta(const DTMT *meta) { return get_meta_meta(meta->pointer()); }

  virtual void replace_data(uint64_t addr, uint32_t *d_s, uint32_t *d_w) { 
    *d_s =  d_indexer.index(addr, 0);
    d_replacer.replace(*d_s, d_w); 
  }

  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w) {
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t> > equal_set(0);
    uint32_t max_free = 0;
    for(int i=0; i<P; i++) {
      uint32_t m_s, m_w, free_num = 0;
      m_s = m_indexer.index(addr, i);
      m_replacer[i].replace(m_s, &m_w);
      for(int j=0; j<NW+EW; j++) if(!access(i, m_s, j)->is_valid()) free_num++;
      if(free_num > max_free) {
        max_free = free_num;
        equal_set.clear();
        equal_set.resize(0);
      }
      if(free_num >= max_free) equal_set.push_back(std::make_tuple(i, m_s, m_w));
    }
    std::tie(*ai, *s, *w) = equal_set[cm_get_random_uint32() % equal_set.size()];
  }

  virtual void replace(uint64_t addr, uint32_t ai, uint32_t *s, uint32_t *w){
    *s = m_indexer.index(addr, ai);
    m_replacer[ai].replace(*s, w);
  }

  virtual void cuckoo_replace(uint64_t addr, uint32_t ai, uint32_t *m_ai, uint32_t *m_s, uint32_t *m_w, bool add){
    if(add) *m_ai = (ai+1)%P; else *m_ai = (ai-1+P)%P;
    replace(addr, *m_ai, m_s, m_w);
  }

  virtual void cuckoo_search(uint32_t *ai, uint32_t *s, uint32_t *w, CMMetadataBase *meta, std::stack<std::tuple<uint32_t, uint32_t, uint32_t> > &addr_stack){
    uint32_t relocation = 0;
    uint64_t addr;
    uint32_t m_ai, m_s, m_w;
    CMMetadataBase *mmeta;
    std::unordered_set<uint64_t> remapped;
    while(EnableRelocation && meta->is_valid() && relocation < RW){
      relocation++;
      addr = meta->addr(*s);
      cuckoo_replace(addr, *ai, &m_ai, &m_s, &m_w, true);
      mmeta = access(m_ai, m_s, m_w);
      if(remapped.count(mmeta->addr(*s))) break;
      remapped.insert(addr);
      addr_stack.push(addr);
      meta = mmeta;
      *ai = m_ai;
      *s  = m_s;
      *w  = m_w;
    }
  }

  virtual void cuckoo_relocate(uint32_t *ai, uint32_t *s, uint32_t *w, CMMetadataBase *meta, std::stack<std::tuple<uint32_t, uint32_t, uint32_t> > &stack, uint64_t *delay) {
    auto [m_ai, m_s, m_w] = stack.top(); stack.pop();
    auto m_meta = access(m_ai, m_s, m_w);
    auto m_data_meta = get_data_meta(static_cast<MT *>(m_meta));
    meta_copy_state(addr, ai, s, w, meta, m_meta, m_data_meta);
    m_meta->to_clean(); m_meta->to_invalid();
    cache->hook_manage(addr, m_ai, m_s, m_w, true, true, false, delay);
    cache->hook_read(addr, ai, s, w, false, delay); // hit is true or false? may have impact on delay
    std::tie(ai, s, w, meta) = std::make_tuple(m_ai, m_s, m_w, m_meta);
  }

};

typedef OuterCohPortUncachedBase OuterPortMSI; // MirageCache is always the LLC, no need to support probe() from memory

// MirageInnerPortSupport:
//   helper class for the common methods used in all MIRAGE inner ports
template<typename MT>
class MirageInnerPortSupport
{
public:
  void meta_copy_state(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, MT *meta, MT* mmeta, MirageDataMeta* data_mmeta){
    uint32_t m_ds, m_dw;
    mmeta->data(&m_ds, &m_dw);
    meta->init(addr);
    meta->bind(m_ds, m_dw); data_mmeta->bind(ai, s, w);
    if(mmeta->is_dirty()) meta->to_dirty();
    if(mmeta->is_shared()) meta->to_shared(); else meta->to_modified();
  }
};

// uncached MSI inner port:
//   no support for reverse probe as if there is no internal cache
//   or the interl cache does not participate in the coherence communication
template<typename MT, typename CT,
         typename = typename std::enable_if<std::is_base_of<MetadataMSIBase, MT>::value>::type,  // MT <- MetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<MirageMetadataSupport, MT>::value>::type,  // MT <- MirageMetadataSupport
         typename = typename std::enable_if<std::is_base_of<CacheBase, CT>::value>::type>             // CT <- CacheBase
class MirageInnerPortUncached : public InnerCohPortUncachedBase, protected MirageInnerPortSupport<MT>
{
protected:
  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, bool>
  access_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) { // common function for access a line in the cache
    uint32_t ai, s, w;
    CMMetadataBase *meta;
    CMDataBase *data;
    auto cache = static_cast<CT *>(this->cache);
    bool hit = cache->hit(addr, &ai, &s, &w);
    if(hit) {
      std::tie(meta, data) = cache->access_line(ai, s, w);
      auto sync = policy->acquire_need_sync(cmd, meta);
      if(sync.first) probe_req(addr, meta, data, sync.second, delay); // sync if necessary
      auto promote = policy->acquire_need_promote(cmd, meta);
      if(promote.first) { outer->acquire_req(addr, meta, data, promote.second, delay); hit = false; } // promote permission if needed
    } else { // miss
      // get the way to be replaced
      cache->replace(addr, &ai, &s, &w);
      meta = cache->access(ai, s, w);
      std::stack<std::tuple<uint32_t, uint32_t, uint32_t> > stack;
      if(meta->is_valid()) cache->cuckoo_search(&ai, &s, &w, meta, stack);
      if(meta->is_valid()) { // associative eviction!
        evict(meta, cache->get_data_data(static_cast<MT *>(meta)), ai, s, w, delay);
        cache->get_data_meta(static_cast<MT *>(meta))->to_invalid();
      }
      while(!stack.empty()) cache->cuckoo_relocate(&ai, &s, &w, meta, stack, delay);
      meta = cache->access(ai, s, w);
      auto data_pointer = cache->replace_data(addr);
      auto data_meta = cache->get_data_meta(data_pointer);
      data = cache->get_data_data(data_pointer);
      if(data_meta->is_valid()) {
        auto meta_pointer = data_meta->pointer();
        auto replace_meta = cache->get_meta_meta(meta_pointer);
        evict(replace_meta, data, std::get<0>(meta_pointer), std::get<1>(meta_pointer), std::get<2>(meta_pointer), delay);
        replace_meta->to_invalid();
      }
      meta->bind(data_pointer.first, data_pointer.second);
      data_meta->bind(ai, s, w);
      outer->acquire_req(addr, meta, data, cmd, delay); // fetch the missing block
    }
    return std::make_tuple(meta, data, ai, s, w, hit);
  }
};

template<typename MT, typename CT,
         typename = typename std::enable_if<std::is_base_of<MetadataMSIBase, MT>::value>::type,  // MT <- MetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<MirageMetadataSupport, MT>::value>::type,  // MT <- MirageMetadataSupport
         typename = typename std::enable_if<std::is_base_of<CacheBase, CT>::value>::type>             // CT <- CacheBase
using MirageInnerPort = InnerCohPortBaseT<MirageInnerPortUncached<MT, CT> >;

#endif
