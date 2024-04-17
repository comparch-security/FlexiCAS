#ifndef CM_CACHE_COHERENCE_MULTI_HPP
#define CM_CACHE_COHERENCE_MULTI_HPP

#include "cache/cache.hpp"
#include "cache/coherence.hpp"
#include "cache/cache_multi.hpp"
#include "util/concept_macro.hpp"


/////////////////////////////////
// Priority of acuqire、probe、release
// the higher the value, the higher the priority.
class Priority{
public:
  static const uint16_t acquire       = 0x001;
  static const uint16_t flush         = 0x001;
  static const uint16_t read          = 0x001;
  static const uint16_t write         = 0x001;
  static const uint16_t acquire_ack   = 0x001;
  static const uint16_t probe         = 0x010;
  static const uint16_t evict         = 0x010;
  static const uint16_t evict_cv_wait = 0x100;
  static const uint16_t release       = 0x100;
};

/////////////////////////////////
// Multi-thread support for outer ports
class OuterCohPortMultiThreadSupport
{
public:
  OuterCohPortMultiThreadSupport() {}
  virtual ~OuterCohPortMultiThreadSupport() {}

  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, 
                           uint64_t *delay, uint32_t ai, uint32_t s, uint32_t w) = 0;
  // Compared to single thread, multi-threaded adds an 'acquire ack' mechanism
  virtual void acquire_ack_req(uint64_t addr, uint64_t* delay) {} 
  
};

/////////////////////////////////
// Multi-thread support for inner ports
class InnerCohPortMultiThreadSupport
{
public:
  InnerCohPortMultiThreadSupport() {}
  virtual ~InnerCohPortMultiThreadSupport() {}

  // adding a cmd parameter in the function parameters might be better. 
  virtual void acquire_ack_resp(uint64_t addr, uint64_t *delay) = 0;
};

// common behvior for multi-thread uncached outer ports
template <typename CT>
  requires C_DERIVE2(CT, CacheBase, CacheBaseMultiThreadSupport)
class OuterCohPortMultiThreadUncached : public OuterCohPortUncached, public OuterCohPortMultiThreadSupport
{
public:
  OuterCohPortMultiThreadUncached(policy_ptr policy) : OuterCohPortUncached(policy), OuterCohPortMultiThreadSupport() {}
  virtual ~OuterCohPortMultiThreadUncached() {}

  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t outer_cmd, 
                           uint64_t *delay, uint32_t ai, uint32_t s, uint32_t w)
  {
    outer_cmd.id = coh_id;
    /** When issuing an acquire request to the lower-level cache, need to unlock the cacheline mutex. */
    auto cmtx = (static_cast<CT*>(cache))->get_cacheline_mutex(ai, s, w);
    cmtx->unlock();

    coh->acquire_resp(addr, data, meta->get_outer_meta(), outer_cmd, delay);
    
    /**  When receiving an acquire response from the lower-level cache, re-lock the cacheline mutex. */
    cmtx->lock();
    policy->meta_after_fetch(outer_cmd, meta, addr);
  }

};

// common behavior for cached outer ports
template <class OPUC, typename IT, typename CT> 
  requires C_DERIVE2(IT, InnerCohPortMultiThreadSupport, InnerCohPortBase)
        && C_DERIVE2(CT, CacheBase, CacheBaseMultiThreadSupport)
class OuterCohPortMultiThreadT : public OPUC
{
protected:
  using OuterCohPortBase::cache;
  using OuterCohPortBase::coh_id;
  using OuterCohPortBase::inner;
  using OuterCohPortBase::coh;
  using OPUC::writeback_req;

public:
  OuterCohPortMultiThreadT(policy_ptr policy) : OPUC(policy) {}
  virtual ~OuterCohPortMultiThreadT() {}

  virtual void acquire_ack_req(uint64_t addr, uint64_t* delay){
    (static_cast<IT*>(coh))->acquire_ack_resp(addr, delay);
  }
  
  virtual std::pair<bool,bool> probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, coh_cmd_t outer_cmd, uint64_t *delay){
    uint32_t ai, s, w;
    auto cache = static_cast<CT *>(OuterCohPortBase::cache);
    bool writeback = false;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    bool hit = cache->hit(addr, &ai, &s, &w, Priority::probe);
    if(hit){
      auto [status, mtx, cv] = cache->get_set_control(ai, s);
      auto cmtx = cache->get_cacheline_mutex(ai, s, w);
      cmtx->lock();
      std::unique_lock lk(*mtx, std::defer_lock);
      std::tie(meta, data) = cache->access_line(ai, s, w);
      /* It is possible that higher priority behaviors have caused the meta to change, so need check again */
      if(!meta->is_valid() || meta->addr(s) != addr){
        cmtx->unlock();
        hit = false;
      }else{
        auto sync = OPUC::policy->probe_need_sync(outer_cmd, meta);
        if(sync.first){
          auto [phit, pwb] = inner->probe_req(addr, meta, data, sync.second, delay);
          if(pwb) cache->hook_write(addr, ai, s, w, true, true, meta, data, delay);
        }
        // writeback if dirty
        if((writeback = OPUC::policy->probe_need_writeback(outer_cmd, meta))) {
          if(data_outer) data_outer->copy(data);
        }
        OPUC::policy->meta_after_probe(outer_cmd, meta, meta_outer, coh_id, writeback); // alway update meta

        cmtx->unlock();
      }

      lk.lock();
      (*status)[s] &= (~Priority::probe);
      lk.unlock();
      cv->notify_all();
    }
    cache->hook_manage(addr, ai, s, w, hit, OPUC::policy->is_outer_evict(outer_cmd), writeback, meta, data, delay);
    return std::make_pair(hit, writeback);
  }

};

template <typename IT, typename CT> 
  requires C_DERIVE2(IT, InnerCohPortMultiThreadSupport, InnerCohPortBase)
        && C_DERIVE2(CT, CacheBase, CacheBaseMultiThreadSupport) 
using OuterCohMultiThreadPort = OuterCohPortMultiThreadT<OuterCohPortMultiThreadUncached<CT>, IT, CT>;

class InnerCohPortMultiThreadUncached : public InnerCohPortUncached, public InnerCohPortMultiThreadSupport
{
public:
  virtual void acquire_ack_resp(uint64_t addr, uint64_t *delay){}
};


#endif