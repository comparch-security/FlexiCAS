#ifndef CM_CACHE_EXCLUSIVE_HPP
#define CM_CACHE_EXCLUSIVE_HPP

#include "cache/msi.hpp"

class ExclusiveCacheSupportBase
{
public:
  virtual bool directory_replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w) { return false; }
};

template<int P, typename DRPC>
class ExclusiveCacheSupport : public ExclusiveCacheSupportBase
{
protected:
  DRPC d_replacer[P];
};

// DW: Directory Ways
template<int IW, int NW, int DW, int P, typename MT, typename DT, typename IDX, typename RPC, typename DRPC, typename DLY, bool EnMon, bool EnDir>
class CacheSkewedExclusive : public CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon>, public ExclusiveCacheSupport<P, DRPC>
{
public:
  CacheSkewedExclusive(std::string name = "") : CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon>(name), ExclusiveCacheSupport<P, DRPC>() {
    for(int i = 0; i<P; i++){
      delete this->arrays[i];
      this->arrays[i] = new CacheArrayNorm<IW,NW,MT,DT>(DW);
    }
  }

  virtual bool hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w){
    for(*ai=0; *ai<P; (*ai)++) {
      *s = this->indexer.index(addr, *ai);
      for(*w=0; *w<NW; (*w)++){
        if(this->access(*ai, *s, *w)->match(addr)) return true;
      }
    }
    if constexpr (EnDir){
      for(*ai=0; *ai<P; (*ai)++) {
        *s = this->indexer.index(addr, *ai);
        for(*w=NW; *w<(NW+DW); (*w)++){
          if(this->access(*ai, *s, *w)->match(addr)) return true;
        }
      }
    }
    return false;
  }
 
  virtual bool directory_replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w){
    if constexpr (EnDir){
      if constexpr (P==1) *ai = 0;
      else                *ai = (cm_get_random_uint32() % P);
      *s = this->indexer.index(addr, *ai);
      this->d_replacer[*ai].replace(*s, w);
      *w = *w + NW;
      return true;
    }
    return false;
  }

  virtual ~CacheSkewedExclusive(){}
};

template<int IW, int NW, int DW, typename MT, typename DT, typename IDX, typename RPC, typename DRPC, typename DLY, bool EnMon, bool EnDir>
using CacheNormExclusive = CacheSkewedExclusive<IW, NW, DW, 1, MT, DT, IDX, RPC, DRPC, DLY, EnMon, EnDir>;

class ExclusiveInnerPortUncached : public InnerCohPortUncached
{
public:
  ExclusiveInnerPortUncached(CohPolicyBase *policy) : InnerCohPortUncached(policy) {}
  virtual ~ExclusiveInnerPortUncached() {}

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, coh_cmd_t cmd, uint64_t *delay) {
    auto [meta, data, ai, s, w, hit] = access_line(addr, cmd, delay);

    if (data_inner) data_inner->copy(data);
    if(meta){ 
      // using snooping protocol meta must be nullptr, only using directory protocol meta is valid
      policy->meta_after_grant(cmd, meta);
      this->cache->hook_read(addr, ai, s, w, hit, delay);
    }
    if(!hit || meta) delete data;
  }

protected:
  virtual void evict(CMMetadataBase *meta, CMDataBase *data, int32_t ai, uint32_t s, uint32_t w, uint64_t *delay) {
    // evict a block due to conflict
    auto addr = meta->addr(s);
    if(meta->is_directory()){ // evict directory meta
      auto sync = policy->writeback_need_sync(meta);
      if(sync.first) probe_req(addr, meta, data, sync.second, delay);
    }
    auto writeback = policy->writeback_need_writeback(meta);
    if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay);
    policy->meta_after_evict(meta);
    this->cache->hook_manage(addr, ai, s, w, true, true, writeback.first, delay);
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, int32_t>
  access_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) { // common function for access a line in the cache
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data;
    bool hit = this->cache->hit(addr, &ai, &s, &w);
    bool create = false;
    if(hit) {
      meta = this->cache->access(ai, s, w);
      if(!meta->is_directory()){ // hit on cache
        data = this->cache->get_data(ai, s, w);
        auto promote = policy->acquire_need_promote(cmd, meta);
        if(promote.first) { outer->acquire_req(addr, meta, data, promote.second, delay); hit = 0; } // promote permission if needed
        // fetch to higher cache and invalid here
        evict(meta, data, ai, s, w, delay);
        meta = nullptr;
        return std::make_tuple(meta, data, ai, s, w, hit);
      }else{ // hit on directory 
        std::tie(meta, data, create) = dynamic_cast<ExclusivePolicySupportBase *>(policy)->acquire_need_create(meta);
        auto sync = policy->acquire_need_sync(cmd, meta);
        if(sync.first) probe_req(addr, meta, data, sync.second, delay);
      }
    }else{
      // if use snooping coherence protocol , we also need probe other inner cache here
      std::tie(meta, data, create) = dynamic_cast<ExclusivePolicySupportBase *>(policy)->acquire_need_create(meta);
      auto sync = policy->acquire_need_sync(cmd, meta);
      if(sync.first) probe_req(addr, meta, data, sync.second, delay);
      if(!meta->is_valid()){
        bool replace = dynamic_cast<ExclusiveCacheSupportBase *>(this->cache)->directory_replace(addr, &ai, &s, &w);
        if(replace){
          delete meta; create = false; 
          // replace is true means using directory coherence protocol 
          meta = this->cache->access(ai, s, w);
          if(meta->is_valid()) evict(meta, data, ai, s, w, delay);
        }
        outer->acquire_req(addr, meta, data, cmd, delay);
      }
    }
    if(create) { delete meta; meta = nullptr;}
    return std::make_tuple(meta, data, ai, s, w, hit);
  }
  virtual void write_line(uint64_t addr, CMDataBase *data_inner, coh_cmd_t cmd, uint64_t *delay, bool dirty = true) {
    uint32_t ai, s, w;
    uint32_t dai, ds, dw;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    CMMetadataBase* mmeta = nullptr;
    bool writeback = false; bool hit = this->cache->hit(addr, &dai, &ds, &dw);
    if(hit){
      meta = this->cache->access(dai, ds, dw);
      assert(meta->is_directory()); // must use directory coherence directory
    }
    this->cache->replace(addr, &ai, &s, &w);
    mmeta = this->cache->access(ai, s, w);
    data = this->cache->get_data(ai, s, w);
    if(mmeta->is_valid()) evict(mmeta, data, ai, s, w, delay);
    if(data_inner) data->copy(data_inner);
    // mmeta needs to be inited
    dynamic_cast<ExclusivePolicySupportBase *>(policy)->meta_after_release(cmd, mmeta, meta, addr, dirty);
    // invalid other inner cache who holds the addr
    auto sync = dynamic_cast<ExclusivePolicySupportBase *>(policy)->release_need_probe(cmd, mmeta);
    if(sync.first) probe_req(addr, mmeta, data, sync.second, delay);
    this->cache->hook_write(addr, ai, s, w, hit, delay);
    this->cache->hook_manage(addr, ai, s, w, true, true, false, delay);
  }

  virtual void flush_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay){
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    bool hit = false;
    bool create = false;
    auto flush = policy->flush_need_sync(cmd, meta);
    if(flush.first) {
      hit = this->cache->hit(addr, &ai, &s, &w);
      if(hit){
        meta = this->cache->access(ai, s, w);
        if(!meta->is_directory()){
          // hit on cache
          data = this->cache->get_data(ai, s, w);
        }else{
          // hit on directory
          std::tie(meta, data, create) = dynamic_cast<ExclusivePolicySupportBase *>(policy)->acquire_need_create(meta);
          probe_req(addr, meta, data, flush.second, delay);
        }
      }else{
        std::tie(meta, data, create) = dynamic_cast<ExclusivePolicySupportBase *>(policy)->acquire_need_create(meta);
        probe_req(addr, meta, data, flush.second, delay);
      }
      auto writeback = policy->writeback_need_writeback(meta);
      if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay);
      policy->meta_after_flush(cmd, meta);
      this->cache->hook_manage(addr, ai, s, w, true, policy->is_evict(cmd), writeback.first, delay);
      if(create) { delete meta; meta = nullptr; }
    } else outer->writeback_req(addr, nullptr, nullptr, policy->cmd_for_outer_flush(cmd), delay);
  }

};


template<class IPUC, typename = typename std::enable_if<std::is_base_of<ExclusiveInnerPortUncached, IPUC>::value>::type> 
class ExclusiveInnerCohPortT : public IPUC
{
public:
  ExclusiveInnerCohPortT(CohPolicyBase *policy) : IPUC(policy) {}
  virtual ~ExclusiveInnerCohPortT() {}

  virtual void probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) {
    for(uint32_t i=0; i<this->coh.size(); i++) {
      auto probe = IPUC::policy->probe_need_probe(cmd, meta, i);
      if(probe.first) this->coh[i]->probe_resp(addr, meta, data, probe.second, delay);
    }
  }
};

template<class OPUC, typename = typename std::enable_if<std::is_base_of<OuterCohPortUncached, OPUC>::value>::type>
class ExclusiveOuterCohPortT : public OPUC
{
public:
  ExclusiveOuterCohPortT(CohPolicyBase *policy) : OPUC(policy) {}
  virtual ~ExclusiveOuterCohPortT() {}

  virtual void probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, coh_cmd_t outer_cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    bool writeback = false;
    bool hit = this->cache->hit(addr, &ai, &s, &w);
    CMMetadataBase* meta = nullptr;
    CMDataBase* data = nullptr;
    bool create = false;

    if(hit){
      meta = this->cache->access(ai, s, w);
      if(!meta->is_directory()){
        // hit on cache
        data = this->cache->get_data(ai, s, w);
      }else{
        std::tie(meta, data, create) = dynamic_cast<ExclusivePolicySupportBase *>(this->policy)->acquire_need_create(meta);
        // sync if necessary
        auto sync = OPUC::policy->probe_need_sync(outer_cmd, meta);
        if(sync.first) this->inner->probe_req(addr, meta, data, sync.second, delay);
      }
    }else{
      std::tie(meta, data, create) = dynamic_cast<ExclusivePolicySupportBase *>(this->policy)->acquire_need_create(meta);
      // sync if necessary
      auto sync = OPUC::policy->probe_need_sync(outer_cmd, meta);
      if(sync.first) this->inner->probe_req(addr, meta, data, sync.second, delay);
    }
    writeback = OPUC::policy->probe_need_writeback(outer_cmd, meta, meta_outer, this->coh_id);
    if(writeback) { 
      if(meta->is_dirty()){
        meta_outer->to_dirty();
        meta->to_clean();
      }
      if(data_outer) data_outer->copy(data);
    }
    this->cache->hook_manage(addr, ai, s, w, hit, OPUC::policy->is_outer_evict(outer_cmd), writeback, delay);
  }
};

typedef ExclusiveOuterCohPortT<OuterCohPortUncached> ExclusiveOuterCohPort;

typedef ExclusiveInnerCohPortT<ExclusiveInnerPortUncached> ExclusiveInnerCohPort;

#endif
