#ifndef CM_CACHE_COHERENCE_HPP
#define CM_CACHE_COHERENCE_HPP

#include "cache/coh_policy.hpp"
#include "cache/cache.hpp"
#include "cache/slicehash.hpp"
#include <tuple>
#include <memory>

/////////////////////////////////
// Priority of transactions (only useful for multithread simulation):
// transactions with higher priority can pre-empt transactions with lower priority on the same cache set
struct XactPrio{
  static const uint16_t acquire       = 0x0001;
  static const uint16_t flush         = 0x0001;
  static const uint16_t probe         = 0x0010; // acquire miss, requiring lower cahce which back-probe this cache
  static const uint16_t sync          = 0x0100; 
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

/////////////////////////////////
// Base interface for outer ports

class OuterCohPortBase
{
protected:
  CacheBase *cache;        // reverse pointer for the cache parent
  InnerCohPortBase *inner; // inner port for probe when sync
  CohMasterBase *coh;      // hook up with the coherence hub
  int32_t coh_id;          // the identifier used in locating this cache client by the coherence master

public:
  virtual ~OuterCohPortBase() = default;

  virtual void connect(CohMasterBase *h) = 0;
  virtual void connect_by_dispatch(CohMasterBase *dispatcher, CohMasterBase *h) = 0;

  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) = 0;
  virtual void writeback_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) = 0;

  // may not implement probe_resp() and finish_req() if the port is uncached
  virtual std::pair<bool, bool> probe_resp(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) { return std::make_pair(false,false); }
  virtual void finish_req(uint64_t addr) {}

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
public:
  virtual ~InnerCohPortBase() = default;

  virtual uint32_t connect(CohClientBase *c) {
    coh.push_back(c);
    assert(coh.size() <= 63 || 0 ==
           "Only 63 coherent inner caches are supported for now as the directory in class MetadataDirectoryBase is implemented as a 64-bit bitmap.");
    return coh.size()-1;
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
template<class Policy, bool EnMT> requires C_DERIVE<Policy, CohPolicyBase>
class OuterCohPortUncached : public OuterCohPortBase
{
public:
  virtual void connect(CohMasterBase *h) override { // auto detection of uncached cache
    OuterCohPortBase::coh = h;
    if constexpr (Policy::is_uncached())
      OuterCohPortBase::coh_id = -1;
    else
      OuterCohPortBase::coh_id = h->connect(this);
  }

  virtual void connect_by_dispatch(CohMasterBase *dispatcher, CohMasterBase *h) override {
    OuterCohPortBase::coh = dispatcher;
    if constexpr (Policy::is_uncached())
      OuterCohPortBase::coh_id = -1;
    else
      OuterCohPortBase::coh_id = h->connect(this);
  }

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

    Policy::meta_after_fetch(outer_cmd, meta, addr);
  }

  virtual void writeback_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t outer_cmd, uint64_t *delay) override {
    outer_cmd.id = coh_id;
    CMMetadataBase *outer_meta = meta ? meta->get_outer_meta() : nullptr;
    coh->writeback_resp(addr, data, outer_meta, outer_cmd, delay);
    Policy::meta_after_writeback(outer_cmd, meta);
  }

  virtual void query_loc_req(uint64_t addr, std::list<LocInfo> *locs) override {
    coh->query_loc_resp(addr, locs);
  }
};

// common behavior for cached outer ports
template<template <typename, bool, typename...> class OPUC, typename Policy, bool EnMT, typename... Extra> requires C_DERIVE<OPUC<Policy, EnMT>, OuterCohPortBase>
class OuterCohPortT : public OPUC<Policy, EnMT, Extra...>
{
protected:
  using OuterCohPortBase::cache;
  using OuterCohPortBase::coh_id;
public:
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
          if(!cache->check_mt_state(ai, s, XactPrio::probe) || !meta->match(addr)) { // cache line is invalidated potentially by a simultaneous acquire
            meta->unlock(); meta = nullptr; data = nullptr;
            cache->reset_mt_state(ai, s, XactPrio::probe); continue; // redo the hit check
          }
        }
        break;
      }
    } else {
      hit = cache->hit(addr, &ai, &s, &w, 0, false);
      if(hit) std::tie(meta, data) = cache->access_line(ai, s, w);
    }

    if(hit) {
      // sync if necessary
      auto sync = Policy::probe_need_sync(outer_cmd, meta);
      if(sync.first) {
        auto [phit, pwb] = OuterCohPortBase::inner->probe_req(addr, meta, data, sync.second, delay);
        if(pwb) {
          cache->replace_write(ai, s, w, false);
          cache->hook_write(addr, ai, s, w, true, meta, data, delay);
        }
      }

      // now we should be able to safely operate on the cache line
      if constexpr (EnMT) {assert(meta->match(addr)); meta_outer->lock(); }
      if((writeback = Policy::probe_need_writeback(outer_cmd, meta))) { if(data_outer) data_outer->copy(data); } // writeback if dirty
      Policy::meta_after_probe(outer_cmd, meta, meta_outer, coh_id, writeback); // alway update meta
      cache->replace_manage(ai, s, w, hit, (coh::is_evict(outer_cmd) ? 1 : 0));
      cache->hook_manage(addr, ai, s, w, hit, (coh::is_evict(outer_cmd) ? 1 : 0), writeback, meta, data, delay);
      if constexpr (EnMT) { meta_outer->unlock(); meta->unlock(); cache->reset_mt_state(ai, s, XactPrio::probe); }
    } else {
      if constexpr (EnMT) meta_outer->lock();
      Policy::meta_after_probe(outer_cmd, meta, meta_outer, coh_id, writeback); // alway update meta
      cache->replace_manage(ai, s, w, hit, (coh::is_evict(outer_cmd) ? 1 : 0));
      cache->hook_manage(addr, ai, s, w, hit, (coh::is_evict(outer_cmd) ? 1 : 0), writeback, meta, data, delay);
      if constexpr (EnMT) meta_outer->unlock();
    }
    return std::make_pair(hit, writeback);
  }

  virtual void finish_req(uint64_t addr) override {
    assert(!Policy::is_uncached());
    OuterCohPortBase::coh->finish_resp(addr, coh::cmd_for_finish(coh_id));
  }

};

template<typename Policy, bool EnMT = false>
using OuterCohPort = OuterCohPortT<OuterCohPortUncached, Policy, EnMT>;

template<typename Policy, bool EnMT> requires C_DERIVE<Policy, CohPolicyBase>
class InnerCohPortUncached : public InnerCohPortBase
{
public:
  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) override {
    auto [meta, data, ai, s, w, hit] = access_line(addr, cmd, XactPrio::acquire, delay);
    bool act_as_prefetch = coh::is_prefetch(cmd) && Policy::is_uncached(); // only tweak replace priority at the LLC accoridng to [Guo2022-MICRO]

    if (data_inner && data) data_inner->copy(data);
    Policy::meta_after_grant(cmd, meta, meta_inner);
    if(!act_as_prefetch || !hit) cache->replace_read(ai, s, w, act_as_prefetch);
    cache->hook_read(addr, ai, s, w, hit, meta, data, delay);
    extra_operations(delay);
    finish_record(addr, coh::cmd_for_finish(cmd.id), !hit, meta, ai, s);
    if(cmd.id == -1) finish_resp(addr, coh::cmd_for_finish(cmd.id));
  }

  virtual void writeback_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) override {
    if(coh::is_flush(cmd))
      flush_line(addr, cmd, delay);
    else
      write_line(addr, data_inner, meta_inner, cmd, delay);
  }

  virtual void query_loc_resp(uint64_t addr, std::list<LocInfo> *locs) override {
    outer->query_loc_req(addr, locs);
    locs->push_front(cache->query_loc(addr));
  }

protected:
  virtual void extra_operations(uint64_t *delay) {} // possible operations to do after meta updated, currently is not compatible with MT

  virtual void evict(CMMetadataBase *meta, CMDataBase *data, int32_t ai, uint32_t s, uint32_t w, uint64_t *delay) {
    // evict a block due to conflict
    auto addr = meta->addr(s);
    assert(cache->hit(addr));
    auto sync = Policy::writeback_need_sync(meta);
    if(sync.first) {
      if constexpr (EnMT && Policy::sync_need_lock()) cache->set_mt_state(ai, s, XactPrio::sync);
      auto [phit, pwb] = probe_req(addr, meta, data, sync.second, delay); // sync if necessary
      if(pwb){
        cache->replace_write(ai, s, w, false);
        cache->hook_write(addr, ai, s, w, true, meta, data, delay); // a write occurred during the probe
      }
      if constexpr (EnMT && Policy::sync_need_lock()) cache->reset_mt_state(ai, s, XactPrio::sync);
    }
    auto writeback = Policy::writeback_need_writeback(meta);
    if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty
    Policy::meta_after_evict(meta);
    cache->replace_manage(ai, s, w, true, 1);
    cache->hook_manage(addr, ai, s, w, true, 1, writeback.first, meta, data, delay);
  }

  virtual std::tuple<bool, CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t>
  check_hit_or_replace(uint64_t addr, uint16_t prio, bool do_replace, uint64_t *delay) { // check hit or get a replacement target
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    bool hit;
    if constexpr (EnMT) {
      while(true) {
        hit = cache->hit(addr, &ai, &s, &w, prio, true);
        if(hit) {
          std::tie(meta, data) = cache->access_line(ai, s, w);
          meta->lock();
          if(!cache->check_mt_state(ai, s, prio) || !meta->match(addr)) { // acquire is intercepted by a probe and even invalidated
            meta->unlock(); meta = nullptr; data = nullptr;
            cache->reset_mt_state(ai, s, prio);
            continue; // redo the hit check
          }
        } else if(do_replace) { // miss
          if(cache->replace(addr, &ai, &s, &w, prio)) { // lock the cache set and get a replacement candidate
            std::tie(meta, data) = cache->access_line(ai, s, w);
            meta->lock();
            while(!cache->check_mt_state(ai, s, prio)) { // yield to an active probe
              meta->unlock();
              cache->wait_mt_state(ai, s, prio);
              meta->lock();
            }
          } else
            continue; // redo the hit check
        }
        break;
      }
    } else {
      hit = cache->hit(addr, &ai, &s, &w, 0, false);
      if(!hit && do_replace) cache->replace(addr, &ai, &s, &w, prio);
      if(hit || do_replace)  std::tie(meta, data) = cache->access_line(ai, s, w);
    }
    return std::make_tuple(hit, meta, data, ai, s, w);
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, bool>
  access_line(uint64_t addr, coh_cmd_t cmd, uint16_t prio, uint64_t *delay) { // common function for access a line in the cache
    auto [hit, meta, data, ai, s, w] = check_hit_or_replace(addr, prio, true, delay);
    if(hit) {
      auto sync = Policy::access_need_sync(cmd, meta);
      if(sync.first) {
        if constexpr (EnMT && Policy::sync_need_lock()) { assert(prio < XactPrio::sync); cache->set_mt_state(ai, s, XactPrio::sync);}
        auto [phit, pwb] = probe_req(addr, meta, data, sync.second, delay); // sync if necessary
        if(pwb){
          cache->replace_write(ai, s, w, false);
          cache->hook_write(addr, ai, s, w, true, meta, data, delay); // a write occurred during the probe
        }
        if constexpr (EnMT && Policy::sync_need_lock()) cache->reset_mt_state(ai, s, XactPrio::sync);
      }
      auto [promote, promote_local, promote_cmd] = Policy::access_need_promote(cmd, meta);
      if(promote) { outer->acquire_req(addr, meta, data, promote_cmd, delay); hit = false; } // promote permission if needed
      else if(promote_local) meta->to_modified(-1);
    } else { // miss
      if(meta->is_valid()) evict(meta, data, ai, s, w, delay);
      outer->acquire_req(addr, meta, data, coh::is_prefetch(cmd) ? cmd : Policy::cmd_for_outer_acquire(cmd), delay); // fetch the missing block
    }
    return std::make_tuple(meta, data, ai, s, w, hit);
  }

  virtual void write_line(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) {
    auto [meta, data, ai, s, w, hit] = access_line(addr, cmd, XactPrio::release, delay);
    assert(hit || cmd.id == -1); // must hit if the inner is cached
    if(data_inner) data->copy(data_inner);
    Policy::meta_after_release(cmd, meta, meta_inner);
    assert(meta_inner); // assume meta_inner is valid for all writebacks
    cache->replace_write(ai, s, w, false);
    cache->hook_write(addr, ai, s, w, hit, meta, data, delay);
    if constexpr (EnMT) { meta->unlock(); cache->reset_mt_state(ai, s, XactPrio::release); }
  }

  virtual void flush_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) {
    if constexpr (!Policy::is_uncached())
      outer->writeback_req(addr, nullptr, nullptr, coh::cmd_for_flush(), delay);
    else {
      auto [hit, meta, data, ai, s, w] = check_hit_or_replace(addr, XactPrio::flush, false, delay);
      auto [probe, probe_cmd] = Policy::flush_need_sync(cmd, meta);
      if(!hit) return;

      if(probe) {
        if constexpr (EnMT && Policy::sync_need_lock()) cache->set_mt_state(ai, s, XactPrio::sync);
        auto [phit, pwb] = probe_req(addr, meta, data, probe_cmd, delay); // sync if necessary
        if(pwb){
          cache->replace_write(ai, s, w, false); // a write occurred during the probe
          cache->hook_write(addr, ai, s, w, true, meta, data, delay); // a write occurred during the probe
        }
        if constexpr (EnMT && Policy::sync_need_lock()) cache->reset_mt_state(ai, s, XactPrio::sync);
      }

      auto writeback = Policy::writeback_need_writeback(meta);
      if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty

      Policy::meta_after_flush(cmd, meta, cache);
      cache->replace_manage(ai, s, w, hit, (coh::is_evict(cmd) ? 2 : 0)); // identify flush to hook_manager
      cache->hook_manage(addr, ai, s, w, hit, (coh::is_evict(cmd) ? 2 : 0), writeback.first, meta, data, delay); // identify flush to hook_manager

      if constexpr (EnMT) { meta->unlock(); cache->reset_mt_state(ai, s, XactPrio::flush); }
    }
  }

};

template<template <typename, bool, typename...> class IPUC, typename Policy, bool EnMT, typename... Extra> requires C_DERIVE<IPUC<Policy, EnMT, Extra...>, InnerCohPortBase>
class InnerCohPortT : public IPUC<Policy, EnMT, Extra...>
{
private:
  PendingXact<EnMT> pending_xact; // record the pending finish message from inner caches
  typedef IPUC<Policy, EnMT, Extra...> IPUC_T;
protected:
  using InnerCohPortBase::cache;
  using InnerCohPortBase::coh;
  using InnerCohPortBase::outer;
public:
  virtual std::pair<bool, bool> probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) override {
    bool hit = false, writeback = false;
    if constexpr (EnMT) meta->unlock();
    for(uint32_t i=0; i<coh.size(); i++) {
      auto probe = Policy::probe_need_probe(cmd, meta, i);
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
      // avoid probe to the same cache line happens between a grant and a finish,
      // do not unlock the cache line until a finish is received (only needed for coherent inner cache)
      if constexpr (EnMT) { meta->unlock(); cache->reset_mt_state(ai, s, XactPrio::acquire); }
      pending_xact.remove(addr, outer_cmd.id);
      IPUC_T::finish_resp(addr, outer_cmd); // call the embedded uncahced port in case there are house keeping work
      if(forward) outer->finish_req(addr);
    }
  }
};

template<typename Policy, bool EnMT = false>
using InnerCohPort = InnerCohPortT<InnerCohPortUncached, Policy, EnMT>;

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
  // prefetch a block
  virtual const CMDataBase *prefetch(uint64_t addr, uint64_t *delay) = 0;

  // flush the whole cache
  virtual void flush_cache(uint64_t *delay) = 0;

  virtual void query_loc(uint64_t addr, std::list<LocInfo> *locs) = 0;

  __always_inline uint64_t normalize(uint64_t addr) const { return addr & ~0x3full; }
};

// interface with the processing core is a special InnerCohPort
template<typename Policy, bool EnMT = false>
class CoreInterface : public InnerCohPortUncached<Policy, EnMT>, public CoreInterfaceBase {
  typedef InnerCohPortUncached<Policy, EnMT> BaseT;
  using BaseT::cache;
  using BaseT::outer;

  virtual const CMDataBase *read_write_access(uint64_t addr, const CMDataBase *m_data, const coh_cmd_t cmd, uint64_t *delay) {
    addr = normalize(addr);
    auto [meta, data, ai, s, w, hit] = this->access_line(addr, cmd, XactPrio::acquire, delay);
    if(coh::is_write(cmd)) {
      meta->to_dirty();
      if(data) data->copy(m_data);
      cache->replace_write(ai, s, w, true);
      cache->hook_write(addr, ai, s, w, hit, meta, data, delay);
    } else {
      bool act_as_prefetch = coh::is_prefetch(cmd) && Policy::is_uncached(); // only tweak replace priority at the LLC accoridng to [Guo2022-MICRO]
      if(!act_as_prefetch || !hit) cache->replace_read(ai, s, w, act_as_prefetch);
      cache->hook_read(addr, ai, s, w, hit, meta, data, delay);
    }
    if constexpr (EnMT) { meta->unlock(); cache->reset_mt_state(ai, s, XactPrio::acquire);}
    if(!hit) outer->finish_req(addr);
#ifdef CHECK_MULTI
    if constexpr (EnMT) { global_lock_checker->check(); }
#endif
    return data; // potentially dangerous and the data pointer is returned without lock
  }

public:
  using BaseT::BaseT;

  virtual const CMDataBase *read(uint64_t addr, uint64_t *delay) override { return read_write_access(addr, nullptr, coh::cmd_for_read(), delay); }
  virtual void write(uint64_t addr, const CMDataBase *m_data, uint64_t *delay) override { read_write_access(addr, m_data, coh::cmd_for_write(), delay); }
  virtual void flush(uint64_t addr, uint64_t *delay) override     { this->flush_line(normalize(addr), coh::cmd_for_flush(), delay); }
  virtual void writeback(uint64_t addr, uint64_t *delay) override { this->flush_line(normalize(addr), coh::cmd_for_writeback(), delay); }
  virtual void writeback_invalidate(uint64_t *delay) override     { assert(nullptr == "Error: L1.writeback_invalidate() is not implemented yet!"); }
  virtual const CMDataBase *prefetch(uint64_t addr, uint64_t *delay) override { return read_write_access(addr, nullptr, coh::cmd_for_prefetch(), delay); }

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
            this->flush_line(addr, coh::cmd_for_flush(), delay);
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
  virtual uint32_t connect(CohClientBase *) override { return -1; }
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

  CoherentCacheBase(CacheBase *cache, OuterCohPortBase *outer, InnerCohPortBase *inner, std::string name)
    : name(name), cache(cache), outer(outer), inner(inner)
  {
    // deferred assignment for the reverse pointer to cache
    outer->cache = cache; outer->inner = inner;
    inner->cache = cache; inner->outer = outer;
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
  CoherentCacheNorm(std::string name = "")
    : CoherentCacheBase(new CacheT(name), new OuterT, new InnerT, name) {}
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
  SliceDispatcher(const std::string &n, int slice) : name(n), hasher(slice) {}
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
