#ifndef CM_CACHE_COHERENCE_HPP
#define CM_CACHE_COHERENCE_HPP

#include "cache/cache.hpp"
#include "cache/coh_policy.hpp"
#include "cache/slicehash.hpp"
#include <tuple>
#include <memory>
#include <unordered_map>

/////////////////////////////////
// Priority of transactions (only useful for multithread simulation):
// transactions with higher priority can pre-empt transactions with lower priority on the same cache set
struct XactPrio{
  static const uint16_t acquire       = 0x0001;
  static const uint16_t flush         = 0x0001;
  static const uint16_t probe         = 0x0010; // acquire miss, requiring lower cahce which back-probe this cache
  static const uint16_t evict         = 0x0100; // do we still need this priority level?
  static const uint16_t release       = 0x1000; // acquire hit but need back probe and writeback from inner
};

class OuterCohPortBase;
class InnerCohPortBase;
class CoherentCacheBase;

// proactive support for future parallel simulation using multi-thread
//   When multi-thread is supported,
//   actual coherence client and master helper classes will implement the cross-thread FIFOs
typedef OuterCohPortBase CohClientBase;
typedef InnerCohPortBase CohMasterBase;

typedef std::shared_ptr<CohPolicyBase> policy_ptr;

/////////////////////////////////
// Base interface for outer ports

class OuterCohPortBase
{
protected:
  CacheBase *cache;        // reverse pointer for the cache parent
  InnerCohPortBase *inner; // inner port for probe when sync
  CohMasterBase *coh;      // hook up with the coherence hub
  int32_t coh_id;          // the identifier used in locating this cache client by the coherence master
  policy_ptr policy;       // the coherence policy

public:
  OuterCohPortBase(policy_ptr policy) : policy(policy) {}
  virtual ~OuterCohPortBase() {}

  void connect(CohMasterBase *h, std::pair<int32_t, policy_ptr> info) { coh = h; coh_id = info.first; policy->connect(info.second.get()); }

  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) = 0;
  virtual void writeback_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) = 0;

  // may not implement probe_resp() and finish_req() if the port is uncached
  virtual std::pair<bool, bool> probe_resp(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) { return std::make_pair(false,false); }
  virtual void finish_req(uint64_t addr) {}

  __always_inline bool is_uncached() const { return coh_id == -1; }
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
  policy_ptr policy; // the coherence policy
public:
  InnerCohPortBase(policy_ptr policy) : policy(policy) {}
  virtual ~InnerCohPortBase() {}

  virtual std::pair<uint32_t, policy_ptr> connect(CohClientBase *c, bool uncached = false) {
    if(uncached) {
      return std::make_pair(-1, policy);
    } else {
      coh.push_back(c);
      assert(coh.size() <= 63 || 0 ==
             "Only 63 coherent inner caches are supported for now as the directory in class MetadataDirectoryBase is implemented as a 64-bit bitmap.");
      return std::make_pair(coh.size()-1, policy);
    }
  }

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t outer_cmd, uint64_t *delay) = 0;
  virtual void writeback_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t outer_cmd, uint64_t *delay) = 0;

  // may not implement probe_req() and finish_resp() if the port is uncached
  virtual std::pair<bool,bool> probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) { return std::make_pair(false,false); }
  virtual void finish_record(uint64_t addr, coh_cmd_t outer_cmd, bool forward, CMMetadataBase *meta, uint32_t ai, uint32_t s) {}
  virtual void finish_resp(uint64_t addr, coh_cmd_t outer_cmd) {};

  virtual void query_loc_resp(uint64_t addr, std::list<LocInfo> *locs) = 0;
  
  friend CoherentCacheBase; // deferred assignment for cache
};

// common behvior for uncached outer ports
template<bool EnMT>
class OuterCohPortUncached : public OuterCohPortBase
{
public:
  OuterCohPortUncached(policy_ptr policy) : OuterCohPortBase(policy) {}
  virtual ~OuterCohPortUncached() override {}

  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t outer_cmd, uint64_t *delay) override {
    outer_cmd.id = coh_id;

    // In the multithread env, an outer probe may invalidate the cache line during a fetch/promotion.
    // To void concurrent write on the same metadata or data (by probe and acquire)
    // use a copy buffer for the outer acquire
    CMMetadataBase * mmeta; CMDataBase * mdata;  // I think allocating data buffer is unnecessary, but play safe for now
    if constexpr (EnMT) {
      mmeta = cache->meta_copy_buffer(); mdata = data ? cache->data_copy_buffer() : nullptr;
      mmeta->copy(meta); // some derived cache may store key info inside the meta, such as the data set/way in MIRAGE
      meta->unlock();
    } else {
      mmeta = meta; mdata = data;
    }

    coh->acquire_resp(addr, mdata, mmeta->get_outer_meta(), outer_cmd, delay);

    if constexpr (EnMT) {
      meta->lock();
      meta->copy(mmeta); if(data) data->copy(mdata);
      cache->meta_return_buffer(mmeta); cache->data_return_buffer(mdata);
    }

    policy->meta_after_fetch(outer_cmd, meta, addr);
  }

  virtual void writeback_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t outer_cmd, uint64_t *delay) override {
    outer_cmd.id = coh_id;
    CMMetadataBase *outer_meta = meta ? meta->get_outer_meta() : nullptr;
    coh->writeback_resp(addr, data, outer_meta, outer_cmd, delay);
    policy->meta_after_writeback(outer_cmd, meta);
  }

  virtual void query_loc_req(uint64_t addr, std::list<LocInfo> *locs) override {
    coh->query_loc_resp(addr, locs);
  }
};

// common behavior for cached outer ports
template<class OPUC, bool EnMT> requires C_DERIVE<OPUC, OuterCohPortUncached<EnMT> >
class OuterCohPortT : public OPUC
{
protected:
  using OuterCohPortBase::cache;
  using OuterCohPortBase::coh_id;
  using OuterCohPortBase::policy;
public:
  OuterCohPortT(policy_ptr policy) : OPUC(policy) {}
  virtual ~OuterCohPortT() override {}

  virtual std::pair<bool,bool> probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, coh_cmd_t outer_cmd, uint64_t *delay) override {
    uint32_t ai, s, w;
    bool writeback = false;
    bool hit;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;

    if constexpr (EnMT) {
      while(true) {
        hit = cache->hit(addr, &ai, &s, &w, XactPrio::probe, true);
        if(hit) {
          std::tie(meta, data) = cache->access_line(ai, s, w); meta->lock();
          if(!meta->match(addr)) { // cache line is invalidated by transactions with higher priority
            meta->unlock(); meta = nullptr; data = nullptr;
            cache->reset_mt_state(ai, s, XactPrio::probe); continue; // redo the hit check
          }
        }
        break;
      }
    } else {
      hit = cache->hit(addr, &ai, &s, &w);
      if(hit) std::tie(meta, data) = cache->access_line(ai, s, w);
    }

    if(hit) {
      if constexpr (EnMT) meta_outer->lock();
      // sync if necessary
      auto sync = policy->probe_need_sync(outer_cmd, meta);
      if(sync.first) {
        auto [phit, pwb] = OuterCohPortBase::inner->probe_req(addr, meta, data, sync.second, delay);
        if(pwb) cache->hook_write(addr, ai, s, w, true, false, meta, data, delay);
      }

      // writeback if dirty
      if((writeback = policy->probe_need_writeback(outer_cmd, meta))) {
        if(data_outer) data_outer->copy(data);
      }
      policy->meta_after_probe(outer_cmd, meta, meta_outer, coh_id, writeback); // alway update meta
      cache->hook_manage(addr, ai, s, w, hit, policy->is_outer_evict(outer_cmd), writeback, meta, data, delay);
      if constexpr (EnMT) { meta_outer->unlock(); meta->unlock(); cache->reset_mt_state(ai, s, XactPrio::probe); }
    } else {
      if constexpr (EnMT) meta_outer->lock();
      policy->meta_after_probe(outer_cmd, meta, meta_outer, coh_id, writeback); // alway update meta
      cache->hook_manage(addr, ai, s, w, hit, policy->is_outer_evict(outer_cmd), writeback, meta, data, delay);
      if constexpr (EnMT) meta_outer->unlock();
    }
    return std::make_pair(hit, writeback);
  }

  virtual void finish_req(uint64_t addr) override {
    assert(!this->is_uncached());
    OuterCohPortBase::coh->finish_resp(addr, policy->cmd_for_finish(coh_id));
  }

};

template <bool EnMT = false>
using OuterCohPort = OuterCohPortT<OuterCohPortUncached<EnMT>, EnMT> ;

template<bool EnMT>
class InnerCohPortUncached : public InnerCohPortBase
{
public:
  InnerCohPortUncached(policy_ptr policy) : InnerCohPortBase(policy) {}
  virtual ~InnerCohPortUncached() override {}

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) override {
    auto [meta, data, ai, s, w, hit] = access_line(addr, cmd, XactPrio::acquire, delay);

    if (data_inner && data) data_inner->copy(data);
    policy->meta_after_grant(cmd, meta, meta_inner);
    cache->hook_read(addr, ai, s, w, hit, meta, data, delay);
    finish_record(addr, policy->cmd_for_finish(cmd.id), !hit, meta, ai, s);
    if(cmd.id == -1) finish_resp(addr, policy->cmd_for_finish(cmd.id));
  }

  virtual void writeback_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) override {
    if(policy->is_flush(cmd))
      flush_line(addr, cmd, delay);
    else
      write_line(addr, data_inner, meta_inner, cmd, delay);
  }

  virtual void query_loc_resp(uint64_t addr, std::list<LocInfo> *locs) override {
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
      if(pwb) cache->hook_write(addr, ai, s, w, true, false, meta, data, delay); // a write occurred during the probe
    }
    auto writeback = policy->writeback_need_writeback(meta, outer->is_uncached());
    if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty
    policy->meta_after_evict(meta);
    cache->hook_manage(addr, ai, s, w, true, true, writeback.first, meta, data, delay);
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, bool>
  access_line(uint64_t addr, coh_cmd_t cmd, uint16_t prio, uint64_t *delay) { // common function for access a line in the cache
    uint32_t ai, s, w;
    CMMetadataBase *meta;
    CMDataBase *data;
    bool hit;

    if constexpr (EnMT) {
      while(true) {
        hit = cache->hit(addr, &ai, &s, &w, prio, true);
        if(hit) {
          std::tie(meta, data) = cache->access_line(ai, s, w); meta->lock();
          if(!meta->match(addr)) { // cache line is invalidated by transactions with higher priority
            meta->unlock(); meta = nullptr; data = nullptr;
            cache->reset_mt_state(ai, s, prio);
            continue; // redo the hit check
          }
        } else { // miss
          if(cache->replace(addr, &ai, &s, &w, prio)) { // lock the cache set and get a replacement candidate
            std::tie(meta, data) = cache->access_line(ai, s, w);
            meta->lock();
          } else
            continue; // redo the hit check
        }
        break;
      }
    } else {
      hit = cache->hit(addr, &ai, &s, &w);
      if(!hit) cache->replace(addr, &ai, &s, &w, prio);
      std::tie(meta, data) = cache->access_line(ai, s, w);
    }

    if(hit) {
      auto sync = policy->access_need_sync(cmd, meta);
      if(sync.first) {
        auto [phit, pwb] = probe_req(addr, meta, data, sync.second, delay); // sync if necessary
        if(pwb) cache->hook_write(addr, ai, s, w, true, false, meta, data, delay); // a write occurred during the probe
      }
      auto [promote, promote_local, promote_cmd] = policy->access_need_promote(cmd, meta);
      if(promote) { outer->acquire_req(addr, meta, data, promote_cmd, delay); hit = false; } // promote permission if needed
      else if(promote_local) meta->to_modified(-1);
    } else { // miss
      if(meta->is_valid()) evict(meta, data, ai, s, w, delay);
      outer->acquire_req(addr, meta, data, policy->cmd_for_outer_acquire(cmd), delay); // fetch the missing block
    }
    return std::make_tuple(meta, data, ai, s, w, hit);
  }

  virtual void write_line(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) {
    auto [meta, data, ai, s, w, hit] = access_line(addr, cmd, XactPrio::release, delay);
    assert(hit || cmd.id == -1); // must hit if the inner is cached
    if(data_inner) data->copy(data_inner);
    policy->meta_after_release(cmd, meta, meta_inner);
    assert(meta_inner); // assume meta_inner is valid for all writebacks
    cache->hook_write(addr, ai, s, w, hit, false, meta, data, delay);
    if constexpr (EnMT) { meta->unlock(); cache->reset_mt_state(ai, s, XactPrio::release); }
  }

  virtual void flush_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    bool hit;

    if constexpr (EnMT) {
      while(true) {
        hit = cache->hit(addr, &ai, &s, &w, XactPrio::flush, true);
        if(hit) {
          std::tie(meta, data) = cache->access_line(ai, s, w); meta->lock();
          if(!meta->match(addr)) { // cache line is invalidated by transactions with higher priority
            meta->unlock(); meta = nullptr; data = nullptr;
            cache->reset_mt_state(ai, s, XactPrio::flush); continue; // redo the hit check
          }
        }
        break;
      }
    } else {
      hit = cache->hit(addr, &ai, &s, &w);
      if(hit) std::tie(meta, data) = cache->access_line(ai, s, w);
    }

    auto [flush, probe, probe_cmd] = policy->flush_need_sync(cmd, meta, outer->is_uncached());
    if(!flush) {
      // do not handle flush at this level, and send it to the outer cache
      outer->writeback_req(addr, nullptr, nullptr, policy->cmd_for_flush(), delay);
      return;
    }

    if(!hit) return;

    if(probe) {
      auto [phit, pwb] = probe_req(addr, meta, data, probe_cmd, delay); // sync if necessary
      if(pwb) cache->hook_write(addr, ai, s, w, true, false, meta, data, delay); // a write occurred during the probe
    }

    auto writeback = policy->writeback_need_writeback(meta, outer->is_uncached());
    if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty

    policy->meta_after_flush(cmd, meta);
    cache->hook_manage(addr, ai, s, w, hit, policy->is_evict(cmd), writeback.first, meta, data, delay);

    if constexpr (EnMT) { meta->unlock(); cache->reset_mt_state(ai, s, XactPrio::flush); }
  }

};

template<class IPUC, bool EnMT> requires C_DERIVE<IPUC, InnerCohPortUncached<EnMT> >
class InnerCohPortT : public IPUC
{
private:
  PendingXact<EnMT> pending_xact; // record the pending finish message from inner caches
protected:
  using InnerCohPortBase::cache;
  using InnerCohPortBase::coh;
  using InnerCohPortBase::outer;
  using InnerCohPortBase::policy;
public:
  InnerCohPortT(policy_ptr policy) : IPUC(policy) {}
  virtual ~InnerCohPortT() override {}

  virtual std::pair<bool, bool> probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) override {
    bool hit = false, writeback = false;
    if constexpr (EnMT) meta->unlock();
    for(uint32_t i=0; i<coh.size(); i++) {
      auto probe = policy->probe_need_probe(cmd, meta, i);
      if(probe.first) {
        auto [phit, pwb] = coh[i]->probe_resp(addr, meta, data, probe.second, delay);
        hit       |= phit;
        writeback |= pwb;
      }
    }
    if constexpr (EnMT) meta->lock();
    return std::make_pair(hit, writeback);
  }

  // record pending finish
  virtual void finish_record(uint64_t addr, coh_cmd_t outer_cmd, bool forward, CMMetadataBase *meta, uint32_t ai, uint32_t s) override {
    pending_xact.insert(addr, outer_cmd.id, forward, meta, ai, s);
  }

  // only forward the finish message recorded by previous acquire
  virtual void finish_resp(uint64_t addr, coh_cmd_t outer_cmd) override {
    auto [valid, forward, meta, ai, s] = pending_xact.read(addr, outer_cmd.id);
    if(valid) {
      if(forward) outer->finish_req(addr);
      // avoid probe to the same cache line happens between a grant and a finish,
      // unlock the cache line until a finish is received (only needed for coherent inner cache)
      if constexpr (EnMT) { meta->unlock(); cache->reset_mt_state(ai, s, XactPrio::acquire); }
      pending_xact.remove(addr, outer_cmd.id);
    }
  }
};

template <typename IPUC, typename CT, bool EnMT = false> 
  requires C_DERIVE<IPUC, InnerCohPortUncached<EnMT>>
        && C_DERIVE<CT, CacheBase, CacheBaseDRSupport> 
class InnerCohPortRemapT : public InnerCohPortT<IPUC, EnMT>
{
  typedef InnerCohPortT<IPUC, EnMT> InnerT;
  std::unordered_set<uint64_t> remapped;

  // copy a cache line (both meta and data)
  void copy(CMMetadataBase* meta, CMDataBase* data, CMMetadataBase* c_meta, CMDataBase* c_data, uint64_t addr) {
    c_meta->init(addr);
    c_meta->copy(meta);
    if(data) c_data->copy(data);
  }

protected:
  using InnerT::cache;

public:
  InnerCohPortRemapT(policy_ptr policy) : InnerT(policy){}
  virtual ~InnerCohPortRemapT() {} 

  void remap(){
    auto cache = static_cast<CT *>(InnerCohPortBase::cache);
    auto[P, nset, nway] = cache->size();
    // TODO: pause monitors.
    // cache->monitors->pause_monitor();
    std::vector<uint64_t> seeds(P);
    for(auto &s:seeds) s = cm_get_random_uint64();
    cache->seed(seeds);
    remapped.clear();
    for(uint32_t ai = 0; ai < P; ai++){
      for(uint32_t idx = 0; idx < nset; idx++){
        for(uint32_t way = 0; way < nway; way++){
          relocation_chain(ai, idx, way);
        }
      }
    }
    // TODO: resume monitors
    // cache->monitors->resume_monitor();
  }
  
  virtual void finish_resp(uint64_t addr, coh_cmd_t outer_cmd){
    // TODO: If the monitor identifies a remap, do it.
    // if(cache->monitors->remap_monitor()) remap();
    InnerT::finish_resp(addr, outer_cmd);
  }

protected:
  void relocation(CMMetadataBase* c_meta, CMDataBase* c_data, uint64_t& c_addr, uint32_t ai) {
    uint32_t new_idx, new_way;
    cache->replace(c_addr, &ai, &new_idx, &new_way, 0);
    auto[m_meta, m_data] = cache->access_line(ai, new_idx, new_way);
    uint64_t m_addr = m_meta->addr(new_idx); 
    auto c_m_meta = cache->meta_copy_buffer();
    auto c_m_data = m_data ? cache->data_copy_buffer() : nullptr;

    copy(m_meta, m_data, c_m_meta, c_m_data, m_addr);

    if (c_m_meta->is_valid()) {
      if (remapped.count(m_addr)) this->evict(m_meta, m_data, ai, new_idx, new_way, nullptr);
      else cache->hook_manage(m_addr, ai, new_idx, new_way, true, true, false, c_m_meta, c_m_data, nullptr);
    }

    copy(c_meta, c_data, m_meta, m_data, c_addr);
    cache->hook_read(c_addr, ai, new_idx, new_way, true, m_meta, m_data, nullptr);

    remapped.insert(c_addr);
    c_addr = m_addr;
    copy(c_m_meta, c_m_data, c_meta, c_data, m_addr);

    cache->meta_return_buffer(c_m_meta);
    cache->data_return_buffer(c_m_data);
  }

  void relocation_chain(uint32_t ai, uint32_t idx, uint32_t way) {
    auto[meta, data] = cache->access_line(ai, idx, way);
    uint64_t c_addr = meta->addr(idx);
    if (!meta->is_valid() || remapped.count(c_addr)) return;
    auto c_meta = cache->meta_copy_buffer();
    auto c_data = data ? cache->data_copy_buffer() : nullptr;
    copy(meta, data, c_meta, c_data, c_addr);

    meta->to_invalid();
    cache->hook_manage(c_addr, ai, idx, way, true, true, false, c_meta, c_data, nullptr);

    while(c_meta->is_valid() && !remapped.count(c_addr)){
      relocation(c_meta, c_data, c_addr, ai);
    }
    cache->meta_return_buffer(c_meta);
    cache->data_return_buffer(c_data);
  }
};

template<bool EnMT = false>
using InnerCohPort = InnerCohPortT<InnerCohPortUncached<EnMT>, EnMT>;

// base class for CoreInterface
class CoreInterfaceBase
{
public:
  virtual const CMDataBase *read(uint64_t addr, uint64_t *delay) = 0;
  
  virtual void write(uint64_t addr, const CMDataBase *m_data, uint64_t *delay) = 0;
  // flush a cache block from the whole cache hierarchy, (clflush in x86-64)
  virtual void flush(uint64_t addr, uint64_t *delay) = 0;
  // if the block is dirty, write it back to memory, while leave the block cache in shared state (clwb in x86-64)
  virtual void writeback(uint64_t addr, uint64_t *delay) = 0;
  // writeback and invalidate all dirty cache blocks, sync with NVM (wbinvd in x86-64)
  virtual void writeback_invalidate(uint64_t *delay) = 0;

  // flush the whole cache
  virtual void flush_cache(uint64_t *delay) = 0;

  virtual void query_loc(uint64_t addr, std::list<LocInfo> *locs) = 0;

  __always_inline uint64_t normalize(uint64_t addr) const { return addr & ~0x3full; }
};

// interface with the processing core is a special InnerCohPort
template<bool EnMT = false>
class CoreInterface : public InnerCohPortUncached<EnMT>, public CoreInterfaceBase {
  typedef InnerCohPortUncached<EnMT> BaseT;
  using BaseT::policy;
  using BaseT::cache;
  using BaseT::outer;

public:
  CoreInterface(policy_ptr policy) : InnerCohPortUncached<EnMT>(policy) {}
  virtual ~CoreInterface() override {}

  virtual const CMDataBase *read(uint64_t addr, uint64_t *delay) override {
    addr = normalize(addr);
    auto cmd = policy->cmd_for_read();
    auto [meta, data, ai, s, w, hit] = this->access_line(addr, cmd, XactPrio::acquire, delay);
    cache->hook_read(addr, ai, s, w, hit, meta, data, delay);
    if constexpr (EnMT) { meta->unlock(); cache->reset_mt_state(ai, s, XactPrio::acquire);}
    if(!hit) outer->finish_req(addr);
#ifdef CHECK_MULTI
    if constexpr (EnMT) { global_lock_checker->check(); }
#endif
    return data; // potentially dangerous and the data pointer is returned without lock
  }

  virtual void write(uint64_t addr, const CMDataBase *m_data, uint64_t *delay) override {
    addr = normalize(addr);
    auto cmd = policy->cmd_for_write();
    auto [meta, data, ai, s, w, hit] = this->access_line(addr, cmd, XactPrio::acquire, delay);
    meta->to_dirty();
    if(data) data->copy(m_data);
    cache->hook_write(addr, ai, s, w, hit, true, meta, data, delay);
    if constexpr (EnMT) { meta->unlock(); cache->reset_mt_state(ai, s, XactPrio::acquire);}
    if(!hit) outer->finish_req(addr);
#ifdef CHECK_MULTI
    if constexpr (EnMT) { global_lock_checker->check(); }
#endif
  }

  virtual void flush(uint64_t addr, uint64_t *delay) override     { addr = normalize(addr); this->flush_line(addr, policy->cmd_for_flush(), delay); }
  virtual void writeback(uint64_t addr, uint64_t *delay) override { addr = normalize(addr); this->flush_line(addr, policy->cmd_for_writeback(), delay); }
  virtual void writeback_invalidate(uint64_t *delay) override     { assert(nullptr == "Error: L1.writeback_invalidate() is not implemented yet!"); }

  virtual void flush_cache(uint64_t *delay) override {
    auto [npar, nset, nway] = cache->size();
    for(int ipar=0; ipar<npar; ipar++)
      for(int iset=0; iset < nset; iset++)
        for(int iway=0; iway < nway; iway++) {
          auto [meta, data] = cache->access_line(ipar, iset, iway);
          if constexpr (EnMT) meta->lock();
          if(meta->is_valid()) {
            auto addr = meta->addr(iset);
            if constexpr (EnMT) meta->unlock();
            this->flush_line(addr, policy->cmd_for_flush(), delay);
          } else {
            if constexpr (EnMT) meta->unlock();
          }
        }
  }

  virtual void query_loc(uint64_t addr, std::list<LocInfo> *locs) override {
    addr = normalize(addr);
    outer->query_loc_req(addr, locs);
    locs->push_front(cache->query_loc(addr));
  }

private:
  // hide and prohibit calling these functions
  virtual std::pair<uint32_t, policy_ptr> connect(CohClientBase *, bool) override { return std::make_pair(-1, policy); }
  virtual void acquire_resp(uint64_t, CMDataBase *, CMMetadataBase *, coh_cmd_t, uint64_t *) override {}
  virtual void writeback_resp(uint64_t, CMDataBase *, CMMetadataBase *, coh_cmd_t, uint64_t *) override {}
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

  CoherentCacheBase(CacheBase *cache, OuterCohPortBase *outer, InnerCohPortBase *inner, policy_ptr policy, std::string name)
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
template<typename CacheT, typename OuterT, class InnerT>
  requires C_DERIVE<CacheT, CacheBase> && C_DERIVE<OuterT, OuterCohPortBase> && C_DERIVE<InnerT, InnerCohPortBase>
class CoherentCacheNorm : public CoherentCacheBase
{
public:
  CoherentCacheNorm(policy_ptr policy, std::string name = "")
    : CoherentCacheBase(new CacheT(name), new OuterT(policy), new InnerT(policy), policy, name) {}
  virtual ~CoherentCacheNorm() override {}
};

/////////////////////////////////
// Slice dispatcher needed normally needed for sliced LLC

// generic dispatcher
// HT: hasher type
template<typename HT> requires C_DERIVE<HT, SliceHashBase>
class SliceDispatcher : public CohMasterBase
{
protected:
  const std::string name;
  std::vector<CohMasterBase*> cohm;
  HT hasher;
public:
  SliceDispatcher(const std::string &n, int slice) : CohMasterBase(nullptr), name(n), hasher(slice) {}
  virtual ~SliceDispatcher() override {}
  void connect(CohMasterBase *c) { cohm.push_back(c); }
  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) override {
    cohm[hasher(addr)]->acquire_resp(addr, data_inner, meta_inner, cmd, delay);
  }
  virtual void writeback_resp(uint64_t addr, CMDataBase *data, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) override {
    cohm[hasher(addr)]->writeback_resp(addr, data, meta_inner, cmd, delay);
  }
  virtual void query_loc_resp(uint64_t addr, std::list<LocInfo> *locs) override {
    cohm[hasher(addr)]->query_loc_resp(addr, locs);
  }
  virtual void finish_resp(uint64_t addr, coh_cmd_t cmd) override {
    cohm[hasher(addr)]->finish_resp(addr, cmd);
  }
};

#endif
