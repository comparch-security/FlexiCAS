#ifndef CM_CACHE_COHERENCE_HPP
#define CM_CACHE_COHERENCE_HPP

#include "cache/cache.hpp"
#include "cache/coh_policy.hpp"
#include "cache/slicehash.hpp"
#include "mesi.hpp"
#include "metadata.hpp"
#include "util/random.hpp"
#include <tuple>

class OuterCohPortBase;
class InnerCohPortBase;
class CoherentCacheBase;

// proactive support for future parallel simulation using multi-thread
//   When multi-thread is supported,
//   actual coherence client and master helper classes will implement the cross-thread FIFOs
typedef OuterCohPortBase CohClientBase;
typedef InnerCohPortBase CohMasterBase;

/////////////////////////////////
// Base interface for outer ports

class OuterCohPortBase
{
protected:
  CacheBase *cache;        // reverse pointer for the cache parent
  InnerCohPortBase *inner; // inner port for probe when sync
  CohMasterBase *coh;      // hook up with the coherence hub
  int32_t coh_id;          // the identifier used in locating this cache client by the coherence master
  CohPolicyBase *policy;   // the coherence policy

public:
  OuterCohPortBase(CohPolicyBase *policy) : policy(policy) {}
  virtual ~OuterCohPortBase() {}

  void connect(CohMasterBase *h, std::pair<int32_t, CohPolicyBase *> info) { coh = h; coh_id = info.first; policy->connect(info.second); }

  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) = 0;
  virtual void writeback_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) = 0;
  virtual std::pair<bool, bool> probe_resp(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) { return std::make_pair(false,false); } // may not implement if not supported

  bool is_uncached() const { return coh_id == -1; }
  virtual void query_loc_req(uint64_t addr, std::list<LocInfo> *locs) = 0;

  virtual void remap_req(uint64_t addr) = 0;

  friend CoherentCacheBase; // deferred assignment for cache
};

/////////////////////////////////
// Base interface for inner ports

class InnerCohPortBase
{
protected:
  CacheBase *cache; // reverse pointer for the cache parent
  OuterCohPortBase *outer; // outer port for writeback when replace
  std::vector<CohClientBase *> coh; // hook up with the inner caches, indexed by vector index
  CohPolicyBase *policy; // the coherence policy
public:
  InnerCohPortBase(CohPolicyBase *policy) : policy(policy) {}
  virtual ~InnerCohPortBase() { delete policy; }

  std::pair<uint32_t, CohPolicyBase *> connect(CohClientBase *c, bool uncached = false) {
    if(uncached) {
      return std::make_pair(-1, policy);
    } else {
      coh.push_back(c);
      return std::make_pair(coh.size()-1, policy);
    }
  }

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t outer_cmd, uint64_t *delay) = 0;
  virtual void writeback_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) = 0;
  virtual std::pair<bool,bool> probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) { return std::make_pair(false,false); } // may not implement if not supported

  virtual void query_loc_resp(uint64_t addr, std::list<LocInfo> *locs) = 0;

  virtual void remap() {};
  virtual void remap_resp(uint64_t addr) = 0;
  
  friend CoherentCacheBase; // deferred assignment for cache
};

// common behvior for uncached outer ports
class OuterCohPortUncached : public OuterCohPortBase
{
public:
  OuterCohPortUncached(CohPolicyBase *policy) : OuterCohPortBase(policy) {}
  virtual ~OuterCohPortUncached() {}

  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t outer_cmd, uint64_t *delay) {
    outer_cmd.id = coh_id;
    coh->acquire_resp(addr, data, meta->get_outer_meta(), outer_cmd, delay);
    policy->meta_after_fetch(outer_cmd, meta, addr);
  }

  virtual void writeback_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t outer_cmd, uint64_t *delay) {
    outer_cmd.id = coh_id;
    CMMetadataBase *outer_meta = meta ? meta->get_outer_meta() : nullptr;
    coh->writeback_resp(addr, data, outer_meta, outer_cmd, delay);
    policy->meta_after_writeback(outer_cmd, meta);
  }

  virtual void query_loc_req(uint64_t addr, std::list<LocInfo> *locs){
    coh->query_loc_resp(addr, locs);
  }

  virtual void remap_req(uint64_t addr){ 
    coh->remap_resp(addr);
  }
};

// common behavior for cached outer ports
template<class OPUC> requires C_DERIVE(OPUC, OuterCohPortUncached)
class OuterCohPortT : public OPUC
{
protected:
  using OuterCohPortBase::cache;
  using OuterCohPortBase::coh_id;
  using OuterCohPortBase::inner;
  using OPUC::writeback_req;
public:
  OuterCohPortT(CohPolicyBase *policy) : OPUC(policy) {}
  virtual ~OuterCohPortT() {}

  virtual std::pair<bool,bool> probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, coh_cmd_t outer_cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    bool writeback = false;
    bool hit = cache->hit(addr, &ai, &s, &w);
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    if(hit) {
      std::tie(meta, data) = cache->access_line(ai, s, w); // need c++17 for auto type infer

      // sync if necessary
      auto sync = OPUC::policy->probe_need_sync(outer_cmd, meta);
      if(sync.first) {
        auto [phit, pwb] = inner->probe_req(addr, meta, data, sync.second, delay);
        if(pwb) cache->hook_write(addr, ai, s, w, true, true, meta, data, delay);
      }

      // writeback if dirty
      if((writeback = OPUC::policy->probe_need_writeback(outer_cmd, meta))) {
        if(data_outer) data_outer->copy(data);
      }
    }
    OPUC::policy->meta_after_probe(outer_cmd, meta, meta_outer, coh_id, writeback); // alway update meta
    cache->hook_manage(addr, ai, s, w, hit, OPUC::policy->is_outer_evict(outer_cmd), writeback, meta, data, delay);
    return std::make_pair(hit, writeback);
  }
};

typedef OuterCohPortT<OuterCohPortUncached> OuterCohPort;

class InnerCohPortUncached : public InnerCohPortBase
{
public:
  InnerCohPortUncached(CohPolicyBase *policy) : InnerCohPortBase(policy) {}
  virtual ~InnerCohPortUncached() {}

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) {
    auto [meta, data, ai, s, w, hit] = access_line(addr, cmd, delay);

    if (data_inner && data) data_inner->copy(data);
    policy->meta_after_grant(cmd, meta, meta_inner);
    cache->hook_read(addr, ai, s, w, hit, meta, data, delay);
  }

  virtual void writeback_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) {
    if(policy->is_flush(cmd))
      flush_line(addr, cmd, delay);
    else
      write_line(addr, data_inner, meta_inner, cmd, delay);
  }

  virtual void query_loc_resp(uint64_t addr, std::list<LocInfo> *locs){
    outer->query_loc_req(addr, locs);
    locs->push_front(cache->query_loc(addr));
  }

  virtual void remap_resp(uint64_t addr){
    if(cache->monitors->remap_monitor()) remap();
    outer->remap_req(addr);
  }

  virtual void remap() override{
    auto[ai, nset, nway] = cache->size();
    ai = ai - 1;
    cache->monitors->reset_monitor();
    if(coh.size() > 0){ // judge std::vector<CohClientBase *> coh, if coh.size = 1, there is a L1 client
      for(uint32_t idx = 0; idx < nset; idx++){
        for(uint32_t way = 0; way < nway; way++){
          CMMetadataBase *meta;
          CMDataBase *data; 
          std::tie(meta, data) = cache->access_line(ai, idx, way);
          if(!meta->is_valid()) continue;
          if(meta->is_modified()){
            uint64_t addr = meta->addr(idx);
            coh_cmd_t cmd = policy->cmd_for_probe_writeback(-1);
            probe_req(addr, meta, data, cmd, NULL);
          }
        }
      }
    }
    std::vector<uint64_t> newSeeds;
    for(int i=0; i<ai+1; i++) newSeeds.push_back(cm_get_random_uint64());
    cache->seed(newSeeds);
    std::unordered_set<uint64_t> remapped;
    for(uint32_t ridx = 0; ridx < nset; ridx++){
      for(uint32_t rway = 0; rway < nway; rway++){
        uint32_t idx = ridx;
        uint32_t way = rway;
        CMMetadataBase *meta;
        CMDataBase *data; 
        std::tie(meta, data) = cache->access_line(ai, idx, way);
        auto c_meta = cache->meta_copy_buffer();
        c_meta->init(meta->addr(idx));
        c_meta->copy(meta);
        auto c_data = cache->data_copy_buffer();
        if(data) c_data->copy(data);
        uint64_t c_addr = c_meta->addr(idx);
        if(meta->is_valid() && !remapped.count(c_addr)){
          meta->to_invalid();
          cache->hook_manage(c_addr, ai, idx, way, true, true, true, c_meta, c_data, nullptr);
          while(c_meta->is_valid() && !remapped.count(c_addr)){
            uint32_t new_idx, new_way;
            cache->replace(c_addr, &ai, &new_idx, &new_way);
            CMMetadataBase *m_meta;
            CMDataBase *m_data; 
            std::tie(m_meta, m_data) = cache->access_line(ai, new_idx, new_way);
            uint64_t m_addr = m_meta->addr(new_idx); 
            auto c_m_meta = cache->meta_copy_buffer();
            c_m_meta->init(m_addr);
            c_m_meta->copy(m_meta);
            auto c_m_data = cache->data_copy_buffer();
            if(m_data) c_m_data->copy(m_data);
            coh_cmd_t cmd = policy->cmd_for_probe_release();
            cache->hook_manage(m_addr, ai, new_idx, new_way, true, true, true, c_m_meta, c_m_data, nullptr);
            if(remapped.count(m_addr) && c_m_meta->is_valid()) evict(m_meta, m_data, ai, new_idx, new_way, nullptr);
            m_meta->init(c_addr); 
            m_meta->copy(c_meta);
            if(c_data) m_data->copy(c_data);
            cache->hook_read(c_addr, ai, new_idx, new_way, true, m_meta, m_data, nullptr);
            remapped.insert(c_addr);
            c_addr = m_addr;
            c_meta->init(m_addr);
            c_meta->copy(c_m_meta);
            if(c_m_data) c_data->copy(c_m_data);
            cache->meta_return_buffer(c_m_meta);
            if(c_m_data) cache->data_return_buffer(c_m_data);
          }
        }
        cache->meta_return_buffer(c_meta);
        if(c_data) cache->data_return_buffer(c_data);
      }
    }
    cache->monitors->resume_monitor();
  }

protected:
  virtual void evict(CMMetadataBase *meta, CMDataBase *data, int32_t ai, uint32_t s, uint32_t w, uint64_t *delay) {
    // evict a block due to conflict
    auto addr = meta->addr(s);
    assert(cache->hit(addr));
    auto sync = policy->writeback_need_sync(meta);
    if(sync.first) {
      auto [phit, pwb] = probe_req(addr, meta, data, sync.second, delay); // sync if necessary
      if(pwb) cache->hook_write(addr, ai, s, w, true, true, meta, data, delay); // a write occurred during the probe
    }
    auto writeback = policy->writeback_need_writeback(meta, outer->is_uncached());
    if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty
    policy->meta_after_evict(meta);
    cache->hook_manage(addr, ai, s, w, true, true, writeback.first, meta, data, delay);
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t>
  replace_line(uint64_t addr, uint64_t *delay) {
    uint32_t ai, s, w;
    CMMetadataBase *meta;
    CMDataBase *data;
    cache->replace(addr, &ai, &s, &w);
    std::tie(meta, data) = cache->access_line(ai, s, w);
    if(meta->is_valid()) evict(meta, data, ai, s, w, delay);
    return std::make_tuple(meta, data, ai, s, w);
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, bool>
  access_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) { // common function for access a line in the cache
    uint32_t ai, s, w;
    CMMetadataBase *meta;
    CMDataBase *data;
    bool hit = cache->hit(addr, &ai, &s, &w);
    if(hit) {
      std::tie(meta, data) = cache->access_line(ai, s, w);
      auto sync = policy->access_need_sync(cmd, meta);
      if(sync.first) {
        auto [phit, pwb] = probe_req(addr, meta, data, sync.second, delay); // sync if necessary
        if(pwb) cache->hook_write(addr, ai, s, w, true, true, meta, data, delay); // a write occurred during the probe
      }
      auto [promote, promote_local, promote_cmd] = policy->access_need_promote(cmd, meta);
      if(promote) { outer->acquire_req(addr, meta, data, promote_cmd, delay); hit = false; } // promote permission if needed
      else if(promote_local) meta->to_modified(-1);
    } else { // miss
      std::tie(meta, data, ai, s, w) = replace_line(addr, delay);
      outer->acquire_req(addr, meta, data, policy->cmd_for_outer_acquire(cmd), delay); // fetch the missing block
    }
    return std::make_tuple(meta, data, ai, s, w, hit);
  }

  virtual void write_line(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) {
    auto [meta, data, ai, s, w, hit] = access_line(addr, cmd, delay);
    assert(hit || cmd.id == -1); // must hit if the inner is cached
    if(data_inner) data->copy(data_inner);
    policy->meta_after_release(cmd, meta, meta_inner);
    assert(meta_inner); // assume meta_inner is valid for all writebacks
    cache->hook_write(addr, ai, s, w, hit, true, meta, data, delay);
  }

  virtual void flush_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    bool hit = cache->hit(addr, &ai, &s, &w);
    if(hit) std::tie(meta, data) = cache->access_line(ai, s, w);

    auto [flush, probe, probe_cmd] = policy->flush_need_sync(cmd, meta, outer->is_uncached());
    if(!flush) {
      // do not handle flush at this level, and send it to the outer cache
      outer->writeback_req(addr, nullptr, nullptr, policy->cmd_for_flush(), delay);
      return;
    }

    if(!hit) return;

    if(probe) {
      auto [phit, pwb] = probe_req(addr, meta, data, probe_cmd, delay); // sync if necessary
      if(pwb) cache->hook_write(addr, ai, s, w, true, true, meta, data, delay); // a write occurred during the probe
    }

    auto writeback = policy->writeback_need_writeback(meta, outer->is_uncached());
    if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty

    policy->meta_after_flush(cmd, meta);
    cache->hook_manage(addr, ai, s, w, hit, policy->is_evict(cmd), writeback.first, meta, data, delay);
  }

};

template<class IPUC> requires C_DERIVE(IPUC, InnerCohPortUncached)
class InnerCohPortT : public IPUC
{
protected:
  using IPUC::coh;
public:
  InnerCohPortT(CohPolicyBase *policy) : IPUC(policy) {}
  virtual ~InnerCohPortT() {}

  virtual std::pair<bool, bool> probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) {
    bool hit = false, writeback = false;
    for(uint32_t i=0; i<coh.size(); i++) {
      auto probe = IPUC::policy->probe_need_probe(cmd, meta, i);
      if(probe.first) {
        auto [phit, pwb] = coh[i]->probe_resp(addr, meta, data, probe.second, delay);
        hit       |= phit;
        writeback |= pwb;
      }
    }
    return std::make_pair(hit, writeback);
  }
};

typedef InnerCohPortT<InnerCohPortUncached> InnerCohPort;

// interface with the processing core is a special InnerCohPort
class CoreInterface : public InnerCohPortUncached {
public:
  CoreInterface(CohPolicyBase *policy) : InnerCohPortUncached(policy) {}
  virtual ~CoreInterface() {}

  uint64_t normalize(uint64_t addr) const { return addr & ~0x3full; }

  virtual const CMDataBase *read(uint64_t addr, uint64_t *delay) {
    addr = normalize(addr);
    auto cmd = policy->cmd_for_read();
    auto [meta, data, ai, s, w, hit] = access_line(addr, cmd, delay);
    cache->hook_read(addr, ai, s, w, hit, meta, data, delay);
    remap_resp(addr);
    return data;
  }

  virtual void write(uint64_t addr, const CMDataBase *m_data, uint64_t *delay) {
    addr = normalize(addr);
    auto cmd = policy->cmd_for_write();
    auto [meta, data, ai, s, w, hit] = access_line(addr, cmd, delay);
    meta->to_dirty();
    if(data) data->copy(m_data);
    cache->hook_write(addr, ai, s, w, hit, false, meta, data, delay);
    remap_resp(addr);
  }

  // flush a cache block from the whole cache hierarchy, (clflush in x86-64)
  virtual void flush(uint64_t addr, uint64_t *delay)     { addr = normalize(addr); flush_line(addr, policy->cmd_for_flush(), delay); remap_resp(addr);}

  // if the block is dirty, write it back to memory, while leave the block cache in shared state (clwb in x86-64)
  virtual void writeback(uint64_t addr, uint64_t *delay) { addr = normalize(addr); flush_line(addr, policy->cmd_for_writeback(), delay); remap_resp(addr);}

  // writeback and invalidate all dirty cache blocks, sync with NVM (wbinvd in x86-64)
  virtual void writeback_invalidate(uint64_t *delay) {
    assert(nullptr == "Error: L1.writeback_invalidate() is not implemented yet!");
  }

  // flush the whole cache
  virtual void flush_cache(uint64_t *delay) {
    auto [npar, nset, nway] = cache->size();
    for(uint32_t ipar=0; ipar<npar; ipar++)
      for(uint32_t iset=0; iset < nset; iset++)
        for(uint32_t iway=0; iway < nway; iway++) {
          auto [meta, data] = cache->access_line(ipar, iset, iway);
          if(meta->is_valid())
            flush_line(meta->addr(iset), policy->cmd_for_flush(), delay);
        }
  }

  virtual void query_loc(uint64_t addr, std::list<LocInfo> *locs){
    addr = normalize(addr);
    outer->query_loc_req(addr, locs);
    locs->push_front(cache->query_loc(addr));
  }

private:
  // hide and prohibit calling these functions
  virtual uint32_t connect(CohClientBase *c) { return 0;}
  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) {}
  virtual void writeback_resp(uint64_t addr, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) {}
};


/////////////////////////////////
// Base class for a Cache supporting coherence operations

class CoherentCacheBase
{
protected:
  const std::string name;               // an optional name to describe this cache
  CacheBase *cache; // pointer to the actual cache

public:
  OuterCohPortBase *outer; // coherence outer port, nullptr if last level
  InnerCohPortBase *inner; // coherence inner port, always has inner

  CoherentCacheBase(CacheBase *cache, OuterCohPortBase *outer, InnerCohPortBase *inner, CohPolicyBase *policy, std::string name)
    : name(name), cache(cache), outer(outer), inner(inner)
  {
    // deferred assignment for the reverse pointer to cache
    outer->cache = cache; outer->inner = inner; outer->policy = policy;
    inner->cache = cache; inner->outer = outer; inner->policy = policy;
    policy->cache = cache;
  }

  virtual ~CoherentCacheBase() {
    delete cache;
    delete outer;
    delete inner;
  }

  // monitor related
  void attach_monitor(MonitorBase *m) { cache->monitors->attach_monitor(m); }
  // support run-time assign/reassign mointors
  void detach_monitor() { cache->monitors->detach_monitor(); }
};


// Normal coherent cache
template<typename CacheT, typename OuterT = OuterCohPort, typename InnerT = InnerCohPort>
  requires C_DERIVE(CacheT, CacheBase) && C_DERIVE(OuterT, OuterCohPortBase) && C_DERIVE(InnerT, InnerCohPortBase)
class CoherentCacheNorm : public CoherentCacheBase
{
public:
  CoherentCacheNorm(CohPolicyBase *policy, std::string name = "") : CoherentCacheBase(new CacheT(name), new OuterT(policy), new InnerT(policy), policy, name) {}
  virtual ~CoherentCacheNorm() {}
};

// Normal L1 coherent cache
template<typename CacheT, typename OuterT = OuterCohPort, typename CoreT = CoreInterface> requires C_DERIVE(CoreT, CoreInterface)
using CoherentL1CacheNorm = CoherentCacheNorm<CacheT, OuterT, CoreT>;

/////////////////////////////////
// Slice dispatcher needed normally needed for sliced LLC

// generic dispatcher
// HT: hasher type
template<typename HT> requires C_DERIVE(HT, SliceHashBase)
class SliceDispatcher : public CohMasterBase
{
protected:
  const std::string name;
  std::vector<CohMasterBase*> cohm;
  HT hasher;
public:
  SliceDispatcher(const std::string &n, int slice) : CohMasterBase(nullptr), name(n), hasher(slice) {}
  virtual ~SliceDispatcher() {}
  void connect(CohMasterBase *c) { cohm.push_back(c); }
  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay){
    cohm[hasher(addr)]->acquire_resp(addr, data_inner, meta_inner, cmd, delay);
  }
  virtual void writeback_resp(uint64_t addr, CMDataBase *data, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay){
    cohm[hasher(addr)]->writeback_resp(addr, data, meta_inner, cmd, delay);
  }
  virtual void query_loc_resp(uint64_t addr, std::list<LocInfo> *locs){
    cohm[hasher(addr)]->query_loc_resp(addr, locs);
  }
  virtual void remap_resp(uint64_t addr){
    cohm[hasher(addr)]->remap_resp(addr);
  }
};

#endif
