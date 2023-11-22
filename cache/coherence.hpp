#ifndef CM_CACHE_COHERENCE_HPP
#define CM_CACHE_COHERENCE_HPP

#include "cache/cache.hpp"
#include "cache/coh_policy.hpp"
#include "cache/slicehash.hpp"
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

  virtual void query_loc_req(uint64_t addr, std::list<LocInfo> *locs) = 0;
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

  std::pair<uint32_t, CohPolicyBase *> connect(CohClientBase *c) { coh.push_back(c); return std::make_pair(coh.size()-1, policy);}

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t outer_cmd, uint64_t *delay) = 0;
  virtual void writeback_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay, bool dirty = true) = 0;
  virtual std::pair<bool,bool> probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) { return std::make_pair(false,false); } // may not implement if not supported

  virtual void query_loc_resp(uint64_t addr, std::list<LocInfo> *locs) = 0;
  
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
    coh->writeback_resp(addr, data, outer_meta, outer_cmd, delay, meta ? meta->is_dirty() : false);
    policy->meta_after_writeback(outer_cmd, meta);
  }

  virtual void query_loc_req(uint64_t addr, std::list<LocInfo> *locs){
    coh->query_loc_resp(addr, locs);
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
        if(pwb) cache->hook_write(addr, ai, s, w, true, true, delay);
      }

      // writeback if dirty
      if(writeback = OPUC::policy->probe_need_writeback(outer_cmd, meta)) {
        if(data_outer) data_outer->copy(data);
      }
    }
    OPUC::policy->meta_after_probe(outer_cmd, meta, meta_outer, coh_id); // alway update meta
    cache->hook_manage(addr, ai, s, w, hit, OPUC::policy->is_outer_evict(outer_cmd), writeback, delay);
    return std::make_pair(hit, writeback);
  }
};

typedef OuterCohPortT<OuterCohPortUncached> OuterCohPort;

class InnerCohPortUncached : public InnerCohPortBase
{
public:
  InnerCohPortUncached(CohPolicyBase *policy) : InnerCohPortBase(policy) {}
  virtual ~InnerCohPortUncached() {}

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t outer_cmd, uint64_t *delay) {
    auto [meta, data, ai, s, w, hit] = access_line(addr, data_inner, outer_cmd, delay);

    if (data_inner && data) data_inner->copy(data);
    policy->meta_after_grant(outer_cmd, meta, meta_inner);
    cache->hook_read(addr, ai, s, w, hit, delay);
  }

  virtual void writeback_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay, bool dirty = true) {
    if(policy->is_flush(cmd))
      flush_line(addr, cmd, delay);
    else
      write_line(addr, data_inner, meta_inner, cmd, delay, dirty);
  }

  virtual void query_loc_resp(uint64_t addr, std::list<LocInfo> *locs){
    outer->query_loc_req(addr, locs);
    locs->push_front(cache->query_loc(addr));
  }

protected:
  virtual void evict(CMMetadataBase *meta, CMDataBase *data, int32_t ai, uint32_t s, uint32_t w, uint64_t *delay) {
    // evict a block due to conflict
    auto addr = meta->addr(s);
    assert(cache->hit(addr));
    auto sync = policy->writeback_need_sync(meta);
    if(sync.first) {
      auto [phit, pwb] = probe_req(addr, meta, data, sync.second, delay); // sync if necessary
      if(pwb) cache->hook_write(addr, ai, s, w, true, true, delay); // a write occurred during the probe
    }
    auto writeback = policy->writeback_need_writeback(meta);
    if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty
    policy->meta_after_evict(meta);
    cache->hook_manage(addr, ai, s, w, true, true, writeback.first, delay);
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, bool>
  access_line(uint64_t addr, CMDataBase* data_inner, coh_cmd_t cmd, uint64_t *delay) { // common function for access a line in the cache
    uint32_t ai, s, w;
    CMMetadataBase *meta;
    CMDataBase *data;
    bool hit = cache->hit(addr, &ai, &s, &w);
    if(hit) {
      std::tie(meta, data) = cache->access_line(ai, s, w);
      auto sync = policy->acquire_need_sync(cmd, meta);
      if(sync.first) {
        auto [phit, pwb] = probe_req(addr, meta, data, sync.second, delay); // sync if necessary
        if(pwb) cache->hook_write(addr, ai, s, w, true, true, delay); // a write occurred during the probe
      }
      auto promote = policy->acquire_need_promote(cmd, meta);
      if(promote.first) { outer->acquire_req(addr, meta, data, promote.second, delay); hit = false; } // promote permission if needed
    } else { // miss
      // get the way to be replaced
      cache->replace(addr, &ai, &s, &w);
      std::tie(meta, data) = cache->access_line(ai, s, w);
      if(meta->is_valid()) evict(meta, data, ai, s, w, delay);
      outer->acquire_req(addr, meta, data, policy->cmd_for_outer_acquire(cmd), delay); // fetch the missing block
    }
    return std::make_tuple(meta, data, ai, s, w, hit);
  }

  virtual void write_line(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay, bool dirty = true) {
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    bool hit = cache->hit(addr, &ai, &s, &w); assert(hit); // must hit
    std::tie(meta, data) = cache->access_line(ai, s, w);
    if(data_inner) data->copy(data_inner);
    policy->meta_after_release(cmd, meta, meta_inner);
    assert(meta_inner); // assume meta_inner is valid for all writebacks
    cache->hook_write(addr, ai, s, w, hit, true, delay);
  }

  virtual void flush_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    bool hit = false;

    auto flush = policy->flush_need_sync(cmd, meta);
    if(flush.first) {
      if(hit = cache->hit(addr, &ai, &s, &w)) {
        std::tie(meta, data) = cache->access_line(ai, s, w);
        auto [phit, pwb] = probe_req(addr, meta, data, flush.second, delay);
        if(pwb) cache->hook_write(addr, ai, s, w, true, true, delay); // a write occurred during the probe
        auto writeback = policy->writeback_need_writeback(meta);
        if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty
        policy->meta_after_flush(cmd, meta);
        cache->hook_manage(addr, ai, s, w, hit, policy->is_evict(cmd), writeback.first, delay);
      }
    } else outer->writeback_req(addr, nullptr, nullptr, policy->cmd_for_outer_flush(cmd), delay);
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
protected:
  uint64_t normalize(uint64_t addr) const { return addr & ~0x3full ;}

public:
  CoreInterface(CohPolicyBase *policy) : InnerCohPortUncached(policy) {}
  virtual ~CoreInterface() {}

  virtual const CMDataBase *read(uint64_t addr, uint64_t *delay) {
    addr = normalize(addr);
    auto cmd = policy->cmd_for_read();
    auto [meta, data, ai, s, w, hit] = access_line(addr, nullptr, cmd, delay);
    cache->hook_read(addr, ai, s, w, hit, delay);
    return data;
  }

  virtual void write(uint64_t addr, const CMDataBase *data, uint64_t *delay) {
    addr = normalize(addr);
    auto cmd = policy->cmd_for_write();
    auto [meta, m_data, ai, s, w, hit] = access_line(addr, nullptr, cmd, delay);
    meta->to_dirty();
    cache->hook_write(addr, ai, s, w, hit, false, delay);
    if(m_data) m_data->copy(data);
  }

  // flush a cache block from the whole cache hierarchy, (clflush in x86-64)
  virtual void flush(uint64_t addr, uint64_t *delay)     { addr = normalize(addr); flush_line(addr, policy->cmd_for_flush(), delay); }

  // if the block is dirty, write it back to memory, while leave the block cache in shared state (clwb in x86-64)
  virtual void writeback(uint64_t addr, uint64_t *delay) { addr = normalize(addr); flush_line(addr, policy->cmd_for_writeback(), delay); }

  // writeback and invalidate all dirty cache blocks, sync with NVM (wbinvd in x86-64)
  virtual void writeback_invalidate(uint64_t *delay) {
    assert(nullptr == "Error: L1.writeback_invalidate() is not implemented yet!");
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
  virtual void writeback_resp(uint64_t addr, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay, bool dirty = true) {}
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
  SliceDispatcher(const std::string &n) : CohMasterBase(nullptr), name(n) {}
  virtual ~SliceDispatcher() {}
  void connect(CohMasterBase *c) { cohm.push_back(c); }
  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay){
    cohm[hasher(addr)]->acquire_resp(addr, data_inner, meta_inner, cmd, delay);
  }
  virtual void writeback_resp(uint64_t addr, CMDataBase *data, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay, bool dirty = true){
    cohm[hasher(addr)]->writeback_resp(addr, data, meta_inner, cmd, delay, dirty);
  }
  virtual void query_loc_resp(uint64_t addr, std::list<LocInfo> *locs){
    cohm[hasher(addr)]->query_loc_resp(addr, locs);
  }
};

#endif
