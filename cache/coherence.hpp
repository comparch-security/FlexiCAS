#ifndef CM_CACHE_COHERENCE_HPP
#define CM_CACHE_COHERENCE_HPP

#include <type_traits>
#include <tuple>
#include "cache/cache.hpp"
#include "cache/slicehash.hpp"
#include "cache/coh_policy.hpp"

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

  inline void connect(CohMasterBase *h, std::pair<int32_t, CohPolicyBase *> info) { coh = h; coh_id = info.first; policy->connect(info.second); }

  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) = 0;
  virtual void writeback_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) = 0;
  virtual void probe_resp(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) {} // may not implement if not supported

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

  inline std::pair<uint32_t, CohPolicyBase *> connect(CohClientBase *c) { coh.push_back(c); return std::make_pair(coh.size()-1, policy);}

  virtual void acquire_resp(uint64_t addr, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) = 0;
  virtual void writeback_resp(uint64_t addr, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay, bool dirty = true) = 0;
  virtual void probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) {} // may not implement if not supported

  friend CoherentCacheBase; // deferred assignment for cache
};

// common behvior for uncached outer ports
class OuterCohPortUncached : public OuterCohPortBase
{
public:
  OuterCohPortUncached(CohPolicyBase *policy) : OuterCohPortBase(policy) {}
  virtual ~OuterCohPortUncached() {}

  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t outer_cmd, uint64_t *delay) {
    outer_cmd.id = this->coh_id;
    coh->acquire_resp(addr, data, outer_cmd, delay);
    policy->meta_after_fetch(outer_cmd, meta, addr);
  }
  virtual void writeback_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t outer_cmd, uint64_t *delay) {
    outer_cmd.id = this->coh_id;
    coh->writeback_resp(addr, data, outer_cmd, delay, meta?meta->is_dirty():false);
    policy->meta_after_writeback(outer_cmd, meta);
  }
};

// common behavior for cached outer ports
template<class OPUC, typename = typename std::enable_if<std::is_base_of<OuterCohPortUncached, OPUC>::value>::type>
class OuterCohPortT : public OPUC
{
public:
  OuterCohPortT(CohPolicyBase *policy) : OPUC(policy) {}
  virtual ~OuterCohPortT() {}

  virtual void probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, coh_cmd_t outer_cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    bool writeback = false;
    bool hit = this->cache->hit(addr, &ai, &s, &w);
    if(hit) {
      auto [meta, data] = this->cache->access_line(ai, s, w); // need c++17 for auto type infer

      // sync if necessary
      auto sync = OPUC::policy->probe_need_sync(outer_cmd, meta);
      if(sync.first) this->inner->probe_req(addr, meta, data, sync.second, delay);

      // writeback if necessary
      auto writeback = OPUC::policy->probe_need_writeback(outer_cmd, meta, meta_outer, this->coh_id);
      if(writeback.first) this->writeback_req(addr, meta, data, writeback.second, delay);

      // update meta
      OPUC::policy->meta_after_probe(outer_cmd, meta);
    }
    this->cache->hook_manage(addr, ai, s, w, hit, OPUC::policy->is_outer_evict(outer_cmd), writeback, delay);
  }
};

typedef OuterCohPortT<OuterCohPortUncached> OuterCohPort;

class InnerCohPortUncached : public InnerCohPortBase
{
public:
  InnerCohPortUncached(CohPolicyBase *policy) : InnerCohPortBase(policy) {}
  virtual ~InnerCohPortUncached() {}

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, coh_cmd_t cmd, uint64_t *delay) {
    auto [meta, data, ai, s, w, hit] = access_line(addr, cmd, delay);

    if (data_inner) data_inner->copy(this->cache->get_data(ai, s, w));
    policy->meta_after_grant(cmd, meta);
    this->cache->hook_read(addr, ai, s, w, hit, delay);
  }

  virtual void writeback_resp(uint64_t addr, CMDataBase *data_inner, coh_cmd_t cmd, uint64_t *delay, bool dirty = true) {
    if(policy->is_flush(cmd))
      flush_line(addr, cmd, delay);
    else
      write_line(addr, data_inner, cmd, delay, dirty);
  }

protected:
  virtual void evict(CMMetadataBase *meta, CMDataBase *data, int32_t ai, uint32_t s, uint32_t w, uint64_t *delay) {
    // evict a block due to conflict
    auto addr = meta->addr(s);
    assert(this->cache->hit(addr));
    auto sync = policy->writeback_need_sync(meta);
    if(sync.first) probe_req(addr, meta, data, sync.second, delay); // sync if necessary
    auto writeback = policy->writeback_need_writeback(meta);
    if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty
    policy->meta_after_evict(meta);
    this->cache->hook_manage(addr, ai, s, w, true, true, writeback.first, delay);
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, int32_t>
  access_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) { // common function for access a line in the cache
    uint32_t ai, s, w;
    CMMetadataBase *meta;
    CMDataBase *data;
    bool hit = this->cache->hit(addr, &ai, &s, &w);
    if(hit) {
      std::tie(meta, data) = this->cache->access_line(ai, s, w);
      auto sync = policy->acquire_need_sync(cmd, meta);
      if(sync.first) probe_req(addr, meta, data, sync.second, delay); // sync if necessary
      auto promote = policy->acquire_need_promote(cmd, meta);
      if(promote.first) { outer->acquire_req(addr, meta, data, promote.second, delay); hit = false; } // promote permission if needed
    } else { // miss
      // get the way to be replaced
      this->cache->replace(addr, &ai, &s, &w);
      std::tie(meta, data) = this->cache->access_line(ai, s, w);
      if(meta->is_valid()) evict(meta, data, ai, s, w, delay);
      outer->acquire_req(addr, meta, data, policy->cmd_for_outer_acquire(cmd), delay); // fetch the missing block
    }
    return std::make_tuple(meta, data, ai, s, w, hit);
  }

  virtual void write_line(uint64_t addr, CMDataBase *data_inner, coh_cmd_t cmd, uint64_t *delay, bool dirty = true) {
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    bool hit = this->cache->hit(addr, &ai, &s, &w); assert(hit); // must hit
    std::tie(meta, data) = this->cache->access_line(ai, s, w);
    if(data_inner) data->copy(data_inner);
    policy->meta_after_release(meta);
    this->cache->hook_write(addr, ai, s, w, hit, delay);
  }

  virtual void flush_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    bool hit = false;

    auto flush = policy->flush_need_sync(cmd, meta);
    if(flush.first) {
      if(hit = this->cache->hit(addr, &ai, &s, &w)) {
        std::tie(meta, data) = this->cache->access_line(ai, s, w);
        probe_req(addr, meta, data, flush.second, delay);
        auto writeback = policy->writeback_need_writeback(meta);
        if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty
        policy->meta_after_flush(cmd, meta);
        this->cache->hook_manage(addr, ai, s, w, hit, policy->is_evict(cmd), writeback.first, delay);
      }
    } else outer->writeback_req(addr, nullptr, nullptr, policy->cmd_for_outer_flush(cmd), delay);
  }

};

template<class IPUC, typename = typename std::enable_if<std::is_base_of<InnerCohPortUncached, IPUC>::value>::type> 
class InnerCohPortT : public IPUC
{
public:
  InnerCohPortT(CohPolicyBase *policy) : IPUC(policy) {}
  virtual ~InnerCohPortT() {}

  virtual void probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) {
    for(uint32_t i=0; i<this->coh.size(); i++) {
      auto probe = IPUC::policy->probe_need_probe(cmd, meta, i);
      if(probe.first) this->coh[i]->probe_resp(addr, meta, data, probe.second, delay);
    }
  }
};

typedef InnerCohPortT<InnerCohPortUncached> InnerCohPort;

// interface with the processing core is a special InnerCohPort
class CoreInterface : public InnerCohPortUncached {
public:
  CoreInterface(CohPolicyBase *policy) : InnerCohPortUncached(policy) {}
  virtual ~CoreInterface() {}

public:

  virtual const CMDataBase *read(uint64_t addr, uint64_t *delay) {
    auto cmd = policy->cmd_for_read();
    auto [meta, data, ai, s, w, hit] = access_line(addr, cmd, delay);
    this->cache->hook_read(addr, ai, s, w, hit, delay);
    return data;
  }

  virtual void write(uint64_t addr, const CMDataBase *data, uint64_t *delay) {
    auto cmd = policy->cmd_for_write();
    auto [meta, m_data, ai, s, w, hit] = access_line(addr, cmd, delay);
    meta->to_dirty();
    this->cache->hook_write(addr, ai, s, w, hit, delay);
    if(m_data) m_data->copy(data);
  }

  // flush a cache block from the whole cache hierarchy, (clflush in x86-64)
  virtual void flush(uint64_t addr, uint64_t *delay)     { flush_line(addr, policy->cmd_for_flush(), delay); }

  // if the block is dirty, write it back to memory, while leave the block cache in shared state (clwb in x86-64)
  virtual void writeback(uint64_t addr, uint64_t *delay) { flush_line(addr, policy->cmd_for_writeback(), delay); }

  // writeback and invalidate all dirty cache blocks, sync with NVM (wbinvd in x86-64)
  virtual void writeback_invalidate(uint64_t *delay) {
    assert(nullptr == "Error: L1.writeback_invalidate() is not implemented yet!");
  }

private:
  // hide and prohibit calling these functions
  virtual uint32_t connect(CohClientBase *c) { return 0;}
  virtual void acquire_resp(uint64_t addr, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) {}
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
template<typename CacheT, typename OuterT, typename InnerT,
         typename = typename std::enable_if<std::is_base_of<CacheBase, CacheT>::value>::type,  // CacheT <- CacheBase
         typename = typename std::enable_if<std::is_base_of<OuterCohPortBase, OuterT>::value>::type, // OuterCohPortBase <- OuterT
         typename = typename std::enable_if<std::is_base_of<InnerCohPortBase, InnerT>::value>::type> // InnerCohPortBase <- InnerT
class CoherentCacheNorm : public CoherentCacheBase
{
public:
  CoherentCacheNorm(CohPolicyBase *policy, std::string name = "") : CoherentCacheBase(new CacheT(), new OuterT(policy), new InnerT(policy), policy, name) {}
  virtual ~CoherentCacheNorm() {}
};

// Normal L1 coherent cache
template<typename CacheT, typename OuterT, typename CoreT,
         typename = typename std::enable_if<std::is_base_of<CoreInterface, CoreT>::value>::type> // CoreInterfaceBase <= CoreT
using CoherentL1CacheNorm = CoherentCacheNorm<CacheT, OuterT, CoreT>;

/////////////////////////////////
// Slice dispatcher needed normally needed for sliced LLC

// generic dispatcher
// HT: hasher type
template<typename HT,
         typename = typename std::enable_if<std::is_base_of<SliceHashBase, HT>::value>::type > // HT <- SliceHashBase
class SliceDispatcher : public CohMasterBase
{
protected:
  const std::string name;
  std::vector<CohMasterBase*> cohm;
  HT hasher;
public:
  SliceDispatcher(const std::string &n) : name(n) {}
  virtual ~SliceDispatcher() {}
  inline void connect(CohMasterBase *c) { cohm.push_back(c); }
  virtual void acquire_resp(uint64_t addr, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay){
    this->cohm[hasher(addr)]->acquire_resp(addr, data, cmd, delay);
  }
  virtual void writeback_resp(uint64_t addr, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay, bool dirty = true){
    this->cohm[hasher(addr)]->writeback_resp(addr, data, cmd, delay, dirty);
  }
};

#endif
