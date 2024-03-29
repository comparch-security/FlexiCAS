#ifndef CM_CACHE_COHERENCE_HPP
#define CM_CACHE_COHERENCE_HPP

#include "cache/cache.hpp"
#include "cache/coh_policy.hpp"
#include "cache/slicehash.hpp"
#include "util/common.hpp"
#include "util/util.hpp"
#include <boost/format.hpp>
#include <cassert>
#include <tuple>

static boost::format access_format("addr: 0x%16x, ai:%2d, set:%2d, way:%2d");

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
  int32_t ack_id;          // ack id
  CohPolicyBase *policy;   // the coherence policy

public:
  OuterCohPortBase(CohPolicyBase *policy) : policy(policy) {}
  virtual ~OuterCohPortBase() {}

  void connect(CohMasterBase *h, std::tuple<int32_t, int32_t, CohPolicyBase *> info) { coh = h; ack_id = std::get<0>(info); coh_id = std::get<1>(info); policy->connect(std::get<2>(info)); }

  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay, uint32_t ai, uint32_t s, uint32_t w, int32_t inner_id = 0) = 0;
  virtual void writeback_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay, uint32_t ai = 0, uint32_t s = 0) = 0;
  virtual void acquire_ack_req(uint64_t addr, uint64_t* delay, int32_t inner_inner_id = 0) {}
  virtual std::pair<bool, bool> probe_resp(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) { return std::make_pair(false,false); } // may not implement if not supported

  bool is_uncached() const { return coh_id == -1; }
  virtual void query_loc_req(uint64_t addr, std::list<LocInfo> *locs) = 0;

  friend CoherentCacheBase; // deferred assignment for cache
  friend InnerCohPortBase;
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
  InnerAcquireRecord record; // record the address info being acquired
  uint32_t ack_num;
public:
  InnerCohPortBase(CohPolicyBase *policy) : policy(policy), ack_num(0) {}
  virtual ~InnerCohPortBase() { delete policy; }

  std::tuple<int32_t, int32_t, CohPolicyBase *> connect(CohClientBase *c, bool uncached = false) {
    record.add_size(c->inner->inner_size(), uncached);
    if(uncached) {
      return std::make_tuple(ack_num++, -1, policy);
    } else {
      coh.push_back(c);
      return std::make_tuple(ack_num++, coh.size()-1, policy);
    }
  }

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t outer_cmd, uint64_t *delay, int32_t inner_inner_id = -1) = 0;
  virtual void writeback_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) = 0;
  virtual std::pair<bool,bool> probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) { return std::make_pair(false,false); } // may not implement if not supported
  virtual void acquire_ack_resp(uint64_t addr, coh_cmd_t cmd, uint64_t *delay, int32_t inner_inner_id) {}
  virtual void query_loc_resp(uint64_t addr, std::list<LocInfo> *locs) = 0;

  uint32_t inner_size() { return ack_num;}
  
  friend CoherentCacheBase; // deferred assignment for cache
};

// common behvior for uncached outer ports
class OuterCohPortUncached : public OuterCohPortBase
{
public:
  OuterCohPortUncached(CohPolicyBase *policy) : OuterCohPortBase(policy) {}
  virtual ~OuterCohPortUncached() {}

  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t outer_cmd, uint64_t *delay, uint32_t ai, uint32_t s, uint32_t w, int32_t inner_id = -1) {
    outer_cmd.id = coh_id;
    auto cmtx = this->cache->get_cacheline_mutex(ai, s, w);
    UNSET_LOCK_PTR(cmtx, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d , w:%d \
    mutex: %p, acquire to outer cache, unset cacheline lock\n", get_time(), database.get_id(get_thread_id), addr, \
    this->cache->get_name().c_str(), ai, s, w, cmtx);

    coh->acquire_resp(addr, data, meta->get_outer_meta(), outer_cmd, delay, policy->get_ack_id(inner_id, ack_id));

    SET_LOCK_PTR(cmtx, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d , w:%d \
    mutex: %p, acquire to outer cache end, set cacheline lock\n", get_time(), database.get_id(get_thread_id), addr, \
    this->cache->get_name().c_str(), ai, s, w, cmtx);
    policy->meta_after_fetch(outer_cmd, meta, addr);
  }

  virtual void writeback_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t outer_cmd, uint64_t *delay, uint32_t ai, uint32_t s) {
    outer_cmd.id = coh_id;
    CMMetadataBase *outer_meta = meta ? meta->get_outer_meta() : nullptr;
    coh->writeback_resp(addr, data, outer_meta, outer_cmd, delay);
    if(meta){
       policy->meta_after_writeback(outer_cmd, meta);
    }
  }

  virtual void query_loc_req(uint64_t addr, std::list<LocInfo> *locs){
    coh->query_loc_resp(addr, locs);
  }

  virtual void acquire_ack_req(uint64_t addr, uint64_t* delay, int32_t inner_inner_id = 0){
    if(!is_uncached()) coh->acquire_ack_resp(addr, policy->cmd_for_acquire_ack(coh_id), delay, policy->get_ack_id(inner_inner_id, ack_id));
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
    bool hit = cache->hit_t(addr, &ai, &s, &w, 0x10);
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    if(hit) {
      auto status = cache->get_status(ai);
      auto mtx = cache->get_mutex(ai, s);
      auto cv = cache->get_cv(ai, s);
      auto cmtx = cache->get_cacheline_mutex(ai, s, w);

      SET_LOCK_PTR(cmtx, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d , w:%d \
      mutex: %p, probe resp, set cacheline lock\n", get_time(), database.get_id(get_thread_id), addr, \
      cache->get_name().c_str(), ai, s, w, cmtx);
      
      std::unique_lock lk(*mtx, std::defer_lock);

      std::tie(meta, data) = cache->access_line(ai, s, w); // need c++17 for auto type infer

      // check again
      if(!meta->is_valid() || meta->addr(s) != addr){
        UNSET_LOCK_PTR(cmtx, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d , w:%d \
        mutex: %p, probe resp, unset cacheline lock\n", get_time(), database.get_id(get_thread_id), addr, \
        cache->get_name().c_str(), ai, s, w, cmtx);
        hit = false;
      }else{
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
        OPUC::policy->meta_after_probe(outer_cmd, meta, meta_outer, coh_id, writeback); // alway update meta

        UNSET_LOCK_PTR(cmtx, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d , w:%d \
        mutex: %p, probe resp, unset cacheline lock\n", get_time(), database.get_id(get_thread_id), addr, \
        cache->get_name().c_str(), ai, s, w, cmtx);
      }

      SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d, w:%d \
      mutex: %p, probe_resp(h,set status lock,B)\n", get_time(), database.get_id(get_thread_id), addr, \
      cache->get_name().c_str(), ai, s, w, mtx);
      (*status)[s] &= (~0x10);
      UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      probe_resp(h,unset status lock,B)\n", get_time(), database.get_id(get_thread_id), addr, cache->get_name().c_str(), mtx);
      cv->notify_all();
    }
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

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay, int32_t inner_inner_id = 0) {
    auto [meta, data, ai, s, w, mtx, cv, status, hit, cmtx] = access_line(addr, cmd, delay, inner_inner_id);
    if(meta->is_valid() && meta->addr(s) == addr){
      policy->meta_after_grant(cmd, meta, meta_inner);
      if (data_inner) data_inner->copy(this->cache->get_data(ai, s, w));
      cache->hook_read(addr, ai, s, w, hit, meta, data, delay);
    }

    outer->acquire_ack_req(addr, delay, inner_inner_id);

    auto unset = policy->acquire_unset_lock(cmd);
    if(unset){
      UNSET_LOCK_PTR(cmtx, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      unset cacheline lock\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), cmtx);
      std::unique_lock lk(*mtx, std::defer_lock);
      SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      set status lock\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
      (*status)[s] = (*status)[s] & (~0x01);
      UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      unset status lock\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
      cv->notify_all();
    }else{
      record.add(inner_inner_id, cmd.id, addr, addr_info{ai, s, w, mtx, cmtx, cv, status});
    }
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

  virtual void acquire_ack_resp(uint64_t addr, coh_cmd_t cmd, uint64_t* delay, int32_t inner_inner_id){
    auto info = record.query(inner_inner_id, cmd.id, addr);
    if(info.first){

      auto mtx = info.second.mtx;
      auto status = info.second.status;
      auto s = info.second.s;
      auto cv = info.second.cv;
      auto cmtx = info.second.cmtx;

      UNSET_LOCK_PTR(cmtx, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      acquire resp ack(unset cacheline lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), cmtx);

      std::unique_lock lk(*mtx, std::defer_lock);
      SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      acquire resp ack(set status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
      (*status)[s] = (*status)[s] & (~0x01);
      record.erase(inner_inner_id, cmd.id, addr);
      UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      acquire resp ack(unset status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
      cv->notify_all();
    }
  }

protected:
  virtual void evict(CMMetadataBase *meta, CMDataBase *data, uint32_t ai, uint32_t s, uint32_t w, uint64_t *delay) {
    // evict a block due to conflict
    auto addr = meta->addr(s);
    // assert(cache->hit(addr));
    auto sync = policy->writeback_need_sync(meta);
    if(sync.first) {
      auto [phit, pwb] = probe_req(addr, meta, data, sync.second, delay); // sync if necessary
      if(pwb) cache->hook_write(addr, ai, s, w, true, true, meta, data, delay); // a write occurred during the probe
    }
    auto writeback = policy->writeback_need_writeback(meta, outer->is_uncached());
    if(writeback.first){
      auto status = this->cache->get_status(ai);
      auto mtx = this->cache->get_mutex(ai, s);
      auto cv = this->cache->get_cv(ai, s);
      uint32_t wait_value = 0x100;
      std::unique_lock lk(*mtx, std::defer_lock);
      SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
      mutex: %p, evict line begin(set lock)\n", get_time(), database.get_id(get_thread_id), addr, \
      this->cache->get_name().c_str(), ai, s, mtx);
      WAIT_CV(cv, lk, s, status, wait_value, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      evict line begin(set lock), get cv\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
      (*status)[s] |= 0x10;
      UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
      mutex: %p, evict line begin(unset lock)\n", get_time(), database.get_id(get_thread_id), addr, \
      this->cache->get_name().c_str(), ai, s, mtx);
      // check again
      auto writeback_r = policy->writeback_need_writeback(meta, outer->is_uncached());
      if(writeback_r.first) outer->writeback_req(addr, meta, data, writeback.second, delay, ai); // writeback if dirty

      SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
      mutex: %p, evict line over(set lock)\n", get_time(), database.get_id(get_thread_id), addr, \
      this->cache->get_name().c_str(), ai, s, mtx);
      (*status)[s] &= ~(0x10);
      UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
      mutex: %p, evict line over(unset lock)\n",get_time(), database.get_id(get_thread_id), addr, \
      this->cache->get_name().c_str(), ai, s, mtx);
      cv->notify_all();
    }
    policy->meta_after_evict(meta);
    cache->hook_manage(addr, ai, s, w, true, true, writeback.first, meta, data, delay);;
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

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, std::mutex*, std::condition_variable*, std::vector<uint32_t>*, bool, std::mutex*>
  access_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay, int32_t inner_id = 0) { // common function for access a line in the cache
    uint32_t ai, s, w;
    CMMetadataBase *meta;
    CMDataBase *data;
    std::mutex* mtx;
    std::mutex* cmtx;
    std::condition_variable* cv;
    std::vector<uint32_t>* status = nullptr;
    bool hit = cache->hit_t(addr, &ai, &s, &w, 0x1, true);
    if(hit) {
      status = this->cache->get_status(ai);
      mtx = this->cache->get_mutex(ai, s);
      cv = this->cache->get_cv(ai, s);
      cmtx = this->cache->get_cacheline_mutex(ai, s, w);
      std::tie(meta, data) = cache->access_line(ai, s, w);
      SET_LOCK_PTR(cmtx, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d w : %d\
      mutex: %p, access line,set cache line mtx(hit)\n", get_time(), database.get_id(get_thread_id), addr, \
      this->cache->get_name().c_str(), ai, s, w, cmtx);

      auto sync = policy->access_need_sync(cmd, meta);
      if(sync.first) {
        auto [phit, pwb] = probe_req(addr, meta, data, sync.second, delay); // sync if necessary
        if(pwb) cache->hook_write(addr, ai, s, w, true, true, meta, data, delay); // a write occurred during the probe
      }
      auto [promote, promote_local, promote_cmd] = policy->access_need_promote(cmd, meta);
      if(promote) { outer->acquire_req(addr, meta, data, promote_cmd, delay, ai, s, w, inner_id); hit = false; } // promote permission if needed
      else if(promote_local) {
        meta->to_modified(-1);
      }
    } else { // miss
      status = this->cache->get_status(ai);
      mtx = this->cache->get_mutex(ai, s);
      cv = this->cache->get_cv(ai, s);
      cmtx = this->cache->get_cacheline_mutex(ai, s, w);
      std::tie(meta, data) = this->cache->access_line(ai, s, w);
      SET_LOCK_PTR(cmtx, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d w : %d\
      mutex: %p, access line,set cache line mtx(not hit)\n", get_time(), database.get_id(get_thread_id), addr, \
      this->cache->get_name().c_str(), ai, s, w, cmtx);
      if(meta->is_valid()) evict(meta, data, ai, s, w, delay);
      outer->acquire_req(addr, meta, data, policy->cmd_for_outer_acquire(cmd), delay, ai, s, w, inner_id); // fetch the missing block
    }
    return std::make_tuple(meta, data, ai, s, w, mtx, cv, status, hit, cmtx);
  }

  virtual void write_line(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    std::mutex* mtx;
    std::mutex* wmtx;
    std::condition_variable* cv;
    std::vector<uint32_t>* status;
    bool hit = this->cache->hit_t(addr, &ai, &s, &w, 0x100);
    if(hit){
      status = this->cache->get_status(ai);
      mtx = this->cache->get_mutex(ai, s);
      cv = this->cache->get_cv(ai, s);
      std::tie(meta, data) = this->cache->access_line(ai, s, w);
      if(meta->is_valid() && meta->addr(s) == addr){
        if(data_inner) data->copy(data_inner);
        policy->meta_after_release(cmd, meta, meta_inner);
#ifndef NDEBUG        
        assert(meta_inner); // assume meta_inner is valid for all writebacks
#endif  
        cache->hook_write(addr, ai, s, w, hit, true, meta, data, delay);
      }

      std::unique_lock lk(*mtx, std::defer_lock);
      SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      write_line(set status lock B)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
      (*status)[s] = (*status)[s] & (~0x100);
      UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
      write_line(unset status lock B)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
      cv->notify_all();
    }
  }

  virtual void flush_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    std::mutex* mtx;
    std::mutex* wmtx;
    std::condition_variable* cv;
    std::vector<uint32_t>* status;
    bool hit = cache->hit_t(addr, &ai, &s, &w, 0x1);
    if(hit) std::tie(meta, data) = cache->access_line(ai, s, w);

    auto [flush, probe, probe_cmd] = policy->flush_need_sync(cmd, meta, outer->is_uncached());
    if(!flush) {
      if(hit){
        status = this->cache->get_status(ai);
        mtx = this->cache->get_mutex(ai, s);
        cv = this->cache->get_cv(ai, s);
        std::unique_lock lk(*mtx, std::defer_lock);
        SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
        flush_line(set status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
        (*status)[s] = (*status)[s] & (~0x1);
        UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
        flush_line(unset status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
        cv->notify_all();
      }
      // do not handle flush at this level, and send it to the outer cache
      outer->writeback_req(addr, nullptr, nullptr, policy->cmd_for_flush(), delay, ai);
      return;
    }

    if(!hit) return;

    status = this->cache->get_status(ai);
    mtx = this->cache->get_mutex(ai, s);
    cv = this->cache->get_cv(ai, s);
    if(probe) {
      auto [phit, pwb] = probe_req(addr, meta, data, probe_cmd, delay); // sync if necessary
      if(pwb) cache->hook_write(addr, ai, s, w, true, true, meta, data, delay); // a write occurred during the probe
    }

    auto writeback = policy->writeback_need_writeback(meta, outer->is_uncached());
    if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay, ai); // writeback if dirty
    
    policy->meta_after_flush(cmd, meta);
    cache->hook_manage(addr, ai, s, w, hit, policy->is_evict(cmd), writeback.first, meta, data, delay);

    std::unique_lock lk(*mtx, std::defer_lock);
    SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    flush_line(set status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
    (*status)[s] = (*status)[s] & (~0x1);
    UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    flush_line(unset status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
    cv->notify_all();
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
    auto [meta, data, ai, s, w, mtx, cv, status, hit, cmtx] = access_line(addr, cmd, delay);

    cache->hook_read(addr, ai, s, w, hit, meta, data, delay);

    outer->acquire_ack_req(addr, delay);

    UNSET_LOCK_PTR(cmtx, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    read(unset cacheline lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);

    std::unique_lock lk(*mtx, std::defer_lock);
    SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    read(set status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
    (*status)[s] = (*status)[s] & (~0x01);
    UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    read(unset status lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
    cv->notify_all();

    return data;
  }

  virtual void write(uint64_t addr, const CMDataBase *m_data, uint64_t *delay) {
    addr = normalize(addr);
    auto cmd = policy->cmd_for_write();
    auto [meta, data, ai, s, w, mtx, cv, status, hit, cmtx] = access_line(addr, cmd, delay);

    if(meta->addr(s) == addr){
      meta->to_dirty();
      if(data) data->copy(m_data);
      cache->hook_write(addr, ai, s, w, hit, false, meta, data, delay);
    }else{
      assert(0);
    }

    outer->acquire_ack_req(addr, delay);

    UNSET_LOCK_PTR(cmtx, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    write(unset cacheline lock)\n", get_time(), database.get_id(get_thread_id), addr, this->cache->get_name().c_str(), mtx);
    
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

  // flush the whole cache
  virtual void flush_cache(uint64_t *delay) {
    auto [npar, nset, nway] = cache->size();
    for(int ipar=0; ipar<npar; ipar++)
      for(int iset=0; iset < nset; iset++)
        for(int iway=0; iway < nway; iway++) {
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
  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay, int32_t inner_inner_id = 0) {}
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
  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay, int32_t inner_inner_id = 0){
    cohm[hasher(addr)]->acquire_resp(addr, data_inner, meta_inner, cmd, delay, inner_inner_id);
  }
  virtual void writeback_resp(uint64_t addr, CMDataBase *data, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay){
    cohm[hasher(addr)]->writeback_resp(addr, data, meta_inner, cmd, delay);
  }
  virtual void query_loc_resp(uint64_t addr, std::list<LocInfo> *locs){
    cohm[hasher(addr)]->query_loc_resp(addr, locs);
  }
};

#endif
