#ifndef CM_CACHE_COHERENCE_HPP
#define CM_CACHE_COHERENCE_HPP

#include <cassert>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <type_traits>
#include <tuple>
#include <vector>
#include "cache/cache.hpp"
#include "cache/slicehash.hpp"
#include "cache/coh_policy.hpp"
#include "util/log.hpp"
#include "util/common.hpp"

class OuterCohPortBase;
class InnerCohPortBase;
class CoherentCacheBase;

// proactive support for future parallel simulation using multi-thread
//   When multi-thread is supported,
//   actual coherence client and master helper classes will implement the cross-thread FIFOs
typedef OuterCohPortBase CohClientBase;
typedef InnerCohPortBase CohMasterBase;

std::mutex Mlock;

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
  virtual bool probe_resp(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) { return false; } // may not implement if not supported

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

  virtual void acquire_resp(uint64_t addr, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) = 0;
  virtual void writeback_resp(uint64_t addr, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay, bool dirty = true) = 0;
  virtual bool probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) { return false; } // may not implement if not supported

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
    std::unique_lock lkm(Mlock, std::defer_lock);
    SET_LOCK(lkm, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    acquire_line(set Mlock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), &Mlock);
    policy->meta_after_fetch(outer_cmd, meta, addr);
    UNSET_LOCK(lkm, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    acuqire_line(unset Mlock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), &Mlock);
  }
  virtual void writeback_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t outer_cmd, uint64_t *delay) {
    outer_cmd.id = this->coh_id;
    coh->writeback_resp(addr, data, outer_cmd, delay, meta ? meta->is_dirty() : false);
    std::unique_lock lkm(Mlock, std::defer_lock);
    SET_LOCK(lkm, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    writeback_line(set Mlock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), &Mlock);
    policy->meta_after_writeback(outer_cmd, meta);
    UNSET_LOCK(lkm, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    writeback_line(unset Mlock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), &Mlock);
  }
};

// common behavior for cached outer ports
template<class OPUC, typename = typename std::enable_if<std::is_base_of<OuterCohPortUncached, OPUC>::value>::type>
class OuterCohPortT : public OPUC
{
public:
  OuterCohPortT(CohPolicyBase *policy) : OPUC(policy) {}
  virtual ~OuterCohPortT() {}

  virtual bool probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, coh_cmd_t outer_cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    bool writeback = false;
    bool hit = this->cache->hit(addr, &ai, &s, &w);
    if(hit) {
      auto status = this->cache->get_status(ai);
      auto mtx = this->cache->get_mutex(ai);
      auto cv = this->cache->get_cv(ai);
      std::unique_lock lk(*mtx, std::defer_lock);
      SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d, w:%d \
      mutex: %p, probe_resp(h,set status lock)\n", get_time(), database.get_id(get_thread_id), addr, \
      this->cache->get_name().c_str(), ai, s, w, mtx);
      WAIT_CV(cv, lk, s, status, 0x100, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      probe_resp(h,get cv)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
      (*status)[s] |= 0x100;
      UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      probe_resp(h,unset status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);

      auto [meta, data] = this->cache->access_line(ai, s, w); // need c++17 for auto type infer

      // sync if necessary
      auto sync = OPUC::policy->probe_need_sync(outer_cmd, meta);
      if(sync.first) this->inner->probe_req(addr, meta, data, sync.second, delay);

      /*
        For the first time, we will only consider the case of inclusive, and here we have 
        removed the case of sending down writeback requests
      */
      // writeback if dirty
      // auto writeback = OPUC::policy->probe_need_writeback(outer_cmd, meta);
      // if(writeback.first) this->writeback_req(addr, meta, data, writeback.second, delay);
      std::unique_lock lkm(Mlock, std::defer_lock);
      SET_LOCK(lkm, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      probe_resp(set Mlock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), &Mlock);
      if(meta->is_valid()){ // check meta again 
        if(writeback = meta->is_dirty()){
          meta_outer->to_dirty();
          if(data_outer) data_outer->copy(data);
          meta->to_clean();
        }
        OPUC::policy->meta_outer_after_probe(outer_cmd, meta_outer, this->coh_id);
        // update meta
        OPUC::policy->meta_after_probe(outer_cmd, meta, this->coh_id);
      }else{
        hit = false;
      }
      UNSET_LOCK(lkm, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      probe_resp(unset Mlock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), &Mlock);

      SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d, w:%d \
      mutex: %p, probe_resp(h,set status lock,B)\n", get_time(), database.get_id(get_thread_id), addr, \
      this->cache->get_name().c_str(), ai, s, w, mtx);
      (*status)[s] &= (~0x100);
      UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      probe_resp(h,unset status lock,B)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
      cv->notify_all();
    }
    this->cache->hook_manage(addr, ai, s, w, hit, OPUC::policy->is_outer_evict(outer_cmd), writeback, delay);
    return hit;
  }
};

typedef OuterCohPortT<OuterCohPortUncached> OuterCohPort;

class InnerCohPortUncached : public InnerCohPortBase
{
public:
  InnerCohPortUncached(CohPolicyBase *policy) : InnerCohPortBase(policy) {}
  virtual ~InnerCohPortUncached() {}

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, coh_cmd_t cmd, uint64_t *delay) {
    auto [meta, data, ai, s, w, mtx, cv, status, hit] = access_line(addr, data_inner, cmd, delay);

    std::unique_lock lkm(Mlock, std::defer_lock);
    SET_LOCK(lkm, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    acquire_resp(set Mlock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), &Mlock);
    if (data_inner) data_inner->copy(this->cache->get_data(ai, s, w));
    if(meta->addr(s) == addr){
      policy->meta_after_grant(cmd, meta);
      this->cache->hook_read(addr, ai, s, w, hit, delay);
    }
    UNSET_LOCK(lkm, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    acquire_resp(unset Mlock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), &Mlock);

    std::unique_lock lk(*mtx, std::defer_lock);
    SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    acquire_resp(set status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
    (*status)[s] = (*status)[s] & (~0x01);
    UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    acquire_resp(unset status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
    cv->notify_all();
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
    // assert(this->cache->hit(addr));
    auto sync = policy->writeback_need_sync(meta);
    if(sync.first) probe_req(addr, meta, data, sync.second, delay); // sync if necessary
    auto writeback = policy->writeback_need_writeback(meta);
    if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty
    std::unique_lock lkm(Mlock, std::defer_lock);
    SET_LOCK(lkm, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    evict_line(set Mlock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), &Mlock);
    policy->meta_after_evict(meta);
    this->cache->hook_manage(addr, ai, s, w, true, true, writeback.first, delay);
    UNSET_LOCK(lkm, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    evict_line(unset Mlock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), &Mlock);
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, std::mutex*, std::condition_variable*, std::vector<uint32_t>*, bool>
  access_line(uint64_t addr, CMDataBase* data_inner, coh_cmd_t cmd, uint64_t *delay) { // common function for access a line in the cache
    uint32_t ai, s, w;
    CMMetadataBase *meta;
    CMDataBase *data;
    std::mutex* mtx;
    std::condition_variable* cv;
    std::vector<uint32_t>* status;
    bool hit = this->cache->hit(addr, &ai, &s, &w);
    if(hit) {
      status = this->cache->get_status(ai);
      mtx = this->cache->get_mutex(ai);
      cv = this->cache->get_cv(ai);
      std::unique_lock lk(*mtx, std::defer_lock);
      SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d, w:%d \
      mutex: %p, access_line(h,set status lock)\n", get_time(), database.get_id(get_thread_id), addr, \
      this->cache->get_name().c_str(), ai, s, w, mtx);
      WAIT_CV(cv, lk, s, status, 1, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      access_line(h,get cv)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
      (*status)[s] = 0x1;
      UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      access_line(h,unset status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
      std::tie(meta, data) = this->cache->access_line(ai, s, w);
      if(meta->addr(s) == addr){
        auto sync = policy->acquire_need_sync(cmd, meta);
        if(sync.first) probe_req(addr, meta, data, sync.second, delay); // sync if necessary
        auto promote = policy->acquire_need_promote(cmd, meta);
        if(promote.first) { outer->acquire_req(addr, meta, data, promote.second, delay); hit = false; } // promote permission if needed
      }else{
        uint32_t aai, as, aw;
        hit = false;
        this->cache->replace(addr, &aai, &as, &aw);
        // For simplicity, only consider remapping to the same set again
        assert(aai == ai);
        assert(as == s);
        std::tie(meta, data) = this->cache->access_line(ai, s, w);
        if(meta->is_valid()) evict(meta, data, ai, s, w, delay);
        outer->acquire_req(addr, meta, data, policy->cmd_for_outer_acquire(cmd), delay); // fetch the missing block
      }
    } else { // miss
      // get the way to be replaced
      this->cache->replace(addr, &ai, &s, &w);
      status = this->cache->get_status(ai);
      mtx = this->cache->get_mutex(ai);
      cv = this->cache->get_cv(ai);
      std::unique_lock lk(*mtx, std::defer_lock);
      SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d, w:%d \
      mutex: %p, access_line(uh,set status lock)\n", get_time(), database.get_id(get_thread_id), addr, \
      this->cache->get_name().c_str(), ai, s, w, mtx);
      WAIT_CV(cv, lk, s, status, 1, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      access_line(uh, get cv)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
      (*status)[s] = 0x1;
      UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      access_line(uh,unset status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
      std::tie(meta, data) = this->cache->access_line(ai, s, w);
      if(meta->is_valid()) evict(meta, data, ai, s, w, delay);
      outer->acquire_req(addr, meta, data, policy->cmd_for_outer_acquire(cmd), delay); // fetch the missing block
    }
    return std::make_tuple(meta, data, ai, s, w, mtx, cv, status, hit);
  }

  virtual void write_line(uint64_t addr, CMDataBase *data_inner, coh_cmd_t cmd, uint64_t *delay, bool dirty = true) {
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    std::mutex* mtx;
    std::condition_variable* cv;
    std::vector<uint32_t>* status;
    bool hit = this->cache->hit(addr, &ai, &s, &w);
    if(hit){
      status = this->cache->get_status(ai);
      mtx = this->cache->get_mutex(ai);
      cv = this->cache->get_cv(ai);
      std::unique_lock lk(*mtx, std::defer_lock);
      SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d, w:%d \
      mutex: %p, write_line(h,set status lock)\n", get_time(), database.get_id(get_thread_id), addr, \
      this->cache->get_name().c_str(), ai, s, w, mtx);
      WAIT_CV(cv, lk, s, status, 1, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, status[s]: %d,\
      write_line(h,get cv)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx, (*status)[s]);
      (*status)[s] = 0x1;
      UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      write_line(h,unset status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
      std::tie(meta, data) = this->cache->access_line(ai, s, w);
      std::unique_lock lkm(Mlock, std::defer_lock);
      SET_LOCK(lkm, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      write_line(set Mlock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), &Mlock);
      if(meta->addr(s) == addr){
        if(data_inner) data->copy(data_inner);
        policy->meta_after_release(meta);
        this->cache->hook_write(addr, ai, s, w, hit, delay);
      }
      UNSET_LOCK(lkm, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      write_line(unset Mlock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), &Mlock);

      SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      write_line(set status lock B)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
      (*status)[s] = (*status)[s] & (~0x01);
      UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      write_line(unset status lock B)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
      cv->notify_all();
    }else{
      printf("thread : %d , name : %s, addr : 0x%-7lx, fail\n", database.get_id(get_thread_id), this->cache->get_name().c_str(), addr);
      assert(0);
    }
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

  virtual bool probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) {
    bool hit = false;
    for(uint32_t i=0; i<this->coh.size(); i++) {
      auto probe = IPUC::policy->probe_need_probe(cmd, meta, i);
      if(probe.first) 
        if(this->coh[i]->probe_resp(addr, meta, data, probe.second, delay))
          hit = true;
    }
    return hit;
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
    auto [meta, data, ai, s, w, mtx, cv, status, hit] = access_line(addr, nullptr, cmd, delay);
    
    std::unique_lock lkm(Mlock, std::defer_lock);
    SET_LOCK(lkm, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    read(set Mlock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), &Mlock);
    if(meta->addr(s) == addr){
      policy->meta_after_grant(cmd, meta);
      this->cache->hook_read(addr, ai, s, w, hit, delay);
    }
    UNSET_LOCK(lkm, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    read(unset Mlock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), &Mlock);

    std::unique_lock lk(*mtx, std::defer_lock);
    SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    read(set status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
    (*status)[s] = (*status)[s] & (~0x01);
    UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    read(unset status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
    cv->notify_all();

    return data;
  }

  virtual void write(uint64_t addr, const CMDataBase *data, uint64_t *delay) {
    addr = normalize(addr);
    auto cmd = policy->cmd_for_write();
    auto [meta,m_data, ai, s, w, mtx, cv, status, hit] = access_line(addr, nullptr, cmd, delay);

    std::unique_lock lkm(Mlock, std::defer_lock);
    SET_LOCK(lkm, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    write(set Mlock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), &Mlock);
    if(meta->addr(s) == addr){
      if(m_data) m_data->copy(data);
      meta->to_dirty();
      this->cache->hook_write(addr, ai, s, w, hit, delay);
    }else{
      assert(0);
    }
    UNSET_LOCK(lkm, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    write(unset Mlock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), &Mlock);

    std::unique_lock lk(*mtx, std::defer_lock);
    SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    write(set status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
    (*status)[s] = (*status)[s] & (~0x01);
    UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    write(unset status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
    cv->notify_all();

  }

  // flush a cache block from the whole cache hierarchy, (clflush in x86-64)
  virtual void flush(uint64_t addr, uint64_t *delay)     { addr = normalize(addr); flush_line(addr, policy->cmd_for_flush(), delay); }

  // if the block is dirty, write it back to memory, while leave the block cache in shared state (clwb in x86-64)
  virtual void writeback(uint64_t addr, uint64_t *delay) { addr = normalize(addr); flush_line(addr, policy->cmd_for_writeback(), delay); }

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
  CoherentCacheNorm(CohPolicyBase *policy, std::string name = "") : CoherentCacheBase(new CacheT(name), new OuterT(policy), new InnerT(policy), policy, name) {}
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
  SliceDispatcher(const std::string &n) : CohMasterBase(nullptr), name(n) {}
  virtual ~SliceDispatcher() {}
  void connect(CohMasterBase *c) { cohm.push_back(c); }
  virtual void acquire_resp(uint64_t addr, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay){
    this->cohm[hasher(addr)]->acquire_resp(addr, data, cmd, delay);
  }
  virtual void writeback_resp(uint64_t addr, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay, bool dirty = true){
    this->cohm[hasher(addr)]->writeback_resp(addr, data, cmd, delay, dirty);
  }
};

#endif
