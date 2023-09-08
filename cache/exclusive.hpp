#ifndef CM_CACHE_EXCLUSIVE_HPP
#define CM_CACHE_EXCLUSIVE_HPP

#include "cache/msi.hpp"

class ExclusiveCacheSupportBase
{
protected:
  std::vector<CacheArrayBase *> directory_arrays; // extended meta arrays  
public:
  virtual CMMetadataBase* access_directory_meta(uint32_t ai, uint32_t s, uint32_t w) { return nullptr;}
  virtual void directory_replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w) {}
  virtual void hook_directory_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, int32_t hit, uint64_t *delay) {}
  virtual void hook_directory_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, int32_t hit, bool evict, bool writeback, uint64_t *delay) {} 
  virtual ~ExclusiveCacheSupportBase(){
    for(auto d : directory_arrays) delete d;
  }
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
    if constexpr (EnDir){
      this->directory_arrays.resize(P);
      for(auto &a : this->directory_arrays) a = new CacheArrayNorm<IW,DW,MT,void>();
    }    
  }
  virtual CMMetadataBase* access_directory_meta(uint32_t ai, uint32_t s, uint32_t w){
    if constexpr (EnDir){
      return this->directory_arrays[ai]->get_meta(s, w);
    }
    return nullptr;
  }

  virtual int32_t hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w){
    for(*ai=0; *ai<P; (*ai)++) {
      *s = this->indexer.index(addr, *ai);
      for(*w=0; *w<NW; (*w)++){
        if(this->access(*ai, *s, *w)->match(addr)) return 1;
      }
    }
    if constexpr (EnDir){
      for(*ai=0; *ai<P; (*ai)++) {
        *s = this->indexer.index(addr, *ai);
        for(*w=0; *w<DW; (*w)++){
          if(this->access_directory_meta(*ai, *s, *w)->match(addr)) return 2;
        }
      }
      return 0;
    }else{
      return 2;
    }
  }
 
  virtual void directory_replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w){
    if constexpr (EnDir){
      if constexpr (P==1) *ai = 0;
      else                *ai = (cm_get_random_uint32() % P);
      *s = this->indexer.index(addr, *ai);
      this->d_replacer[*ai].replace(*s, w);
    }
  }
  virtual void hook_directory_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, int32_t hit, bool evict, bool writeback, uint64_t *delay) {
    if(EnDir){
      if(hit && evict) this->d_replacer[ai].invalid(s, w);
    //   if constexpr (EnMon || !std::is_void<DLY>::value) monitors->hook_manage(addr, ai, s, w, hit, evict, writeback, delay);
    }
  }
  virtual void hook_directory_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, int32_t hit, uint64_t *delay){
    if(EnDir){
      this->d_replacer[ai].access(s, w);
    //   if constexpr (EnMon || !std::is_void<DLY>::value) monitors->hook_read(addr, ai, s, w, hit, delay);
    }
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
    if(hit != 1){ // miss on cache
      policy->meta_after_grant(cmd, meta);
      dynamic_cast<ExclusiveCacheSupportBase*>(this->cache)->hook_directory_read(addr, ai, s, w, hit, delay);
      delete data;
    } 
  }

protected:
  virtual void evict(CMMetadataBase *meta, CMDataBase *data, int32_t ai, uint32_t s, uint32_t w, uint64_t *delay) {
    // evict a block due to conflict
    auto addr = meta->addr(s);
    int32_t hit = this->cache->hit(addr);
    if(hit == 2){ // hit on directory or using snooping coherence protocol
      auto sync = policy->writeback_need_sync(meta);
      if(sync.first) probe_req(addr, meta, data, sync.second, delay); 
    }
    auto writeback = policy->writeback_need_writeback(meta);
    if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay, writeback.first);
    policy->meta_after_evict(meta); 
    if(hit == 2){ 
      dynamic_cast<ExclusiveCacheSupportBase *>(this->cache)->hook_directory_manage(addr, ai, s, w, true, true, true, delay);
    }else{
      this->cache->hook_manage(addr, ai, s, w, true, true, true, delay);  
    }
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, int32_t>
  access_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) { // common function for access a line in the cache
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data;
    int32_t hit = this->cache->hit(addr, &ai, &s, &w);
    if(hit == 1) {
      std::tie(meta, data) = this->cache->access_line(ai, s, w);
      auto promote = policy->acquire_need_promote(cmd, meta);
      if(promote.first) { outer->acquire_req(addr, meta, data, promote.second, delay); hit = 0; } // promote permission if needed
      // fetch to higher cache and invalid here
      evict(meta, data, ai, s, w, delay);
      return std::make_tuple(meta, data, ai, s, w, hit);
    }else{
      bool create = false;
      if(hit == 2){
        meta = dynamic_cast<ExclusiveCacheSupportBase *>(this->cache)->access_directory_meta(ai, s, w);
        std::tie(meta, data, create) = dynamic_cast<ExclusivePolicySupportBase *>(policy)->acquire_need_create(meta);
        auto sync = policy->acquire_need_sync(cmd, meta);
        if(sync.first) probe_req(addr, meta, data, sync.second, delay);
        if(!meta->is_valid()) outer->acquire_req(addr, meta, data, cmd, delay);
      }else{
        dynamic_cast<ExclusiveCacheSupportBase *>(this->cache)->directory_replace(addr, &ai, &s, &w);
        meta = dynamic_cast<ExclusiveCacheSupportBase *>(this->cache)->access_directory_meta(ai, s, w);
        std::tie(meta, data, create) = dynamic_cast<ExclusivePolicySupportBase *>(policy)->acquire_need_create(meta);
        if(meta && meta->is_valid()){
          evict(meta, data, ai, s, w, delay);
        }
        outer->acquire_req(addr, meta, data, cmd, delay);
      }
      if(create) {
        delete meta;
        meta = nullptr;
      }
      return std::make_tuple(meta, data, ai, s, w, hit);
    }
  }
  virtual void write_line(uint64_t addr, CMDataBase *data_inner, coh_cmd_t cmd, uint64_t *delay, bool dirty = true) {
    uint32_t ai, s, w;
    uint32_t dai, ds, dw;
    CMMetadataBase *meta = nullptr; CMMetadataBase *directory_meta = nullptr;
    CMDataBase *data = nullptr;
    bool writeback = false; int32_t hit = this->cache->hit(addr, &dai, &ds, &dw); 
    assert(hit == 2); // must miss on cache and hit on directory(if use directory coherence protocol)
    directory_meta = dynamic_cast<ExclusiveCacheSupportBase *>(this->cache)->access_directory_meta(dai, ds, dw);
    this->cache->replace(addr, &ai, &s, &w);
    std::tie(meta, data) = this->cache->access_line(ai, s, w);
    // evict the block for addr
    if(meta->is_valid()) evict(meta, data, ai, s, w, delay);
    if(data_inner) data->copy(data_inner);
    // meta needs to be inited
    dynamic_cast<ExclusivePolicySupportBase *>(policy)->meta_after_release(cmd, meta, directory_meta, addr, dirty);
    // invalid other inner cache who holds the addr
    auto sync = dynamic_cast<ExclusivePolicySupportBase *>(policy)->release_need_probe(cmd, meta);
    if(sync.first) probe_req(addr, meta, data, sync.second, delay);
    this->cache->hook_write(addr, ai, s, w, hit, delay);
    // invalid directory meta
    dynamic_cast<ExclusiveCacheSupportBase *>(this->cache)->hook_directory_manage(addr, dai, ds, dw, true, true, true, delay);
  }

  virtual void flush_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay){
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    int32_t hit;

    auto flush = policy->flush_need_sync(cmd, meta);
    if(flush.first) {
      hit = this->cache->hit(addr, &ai, &s, &w);
      if(hit == 1) {
        std::tie(meta, data) = this->cache->access_line(ai, s, w);
        auto writeback = policy->writeback_need_writeback(meta);
        if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); 
        policy->meta_after_flush(cmd, meta);
        this->cache->hook_manage(addr, ai, s, w, true, policy->is_evict(cmd), writeback.first, delay);
      }else{
        if(hit == 2){
          bool create = false;
          meta = dynamic_cast<ExclusiveCacheSupportBase *>(this->cache)->access_directory_meta(ai, s, w);
          std::tie(meta, data, create) = dynamic_cast<ExclusivePolicySupportBase *>(policy)->acquire_need_create(meta);
          probe_req(addr, meta, data, flush.second, delay);
          auto writeback = policy->writeback_need_writeback(meta);
          if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); 
          policy->meta_after_flush(cmd, meta);
          dynamic_cast<ExclusiveCacheSupportBase *>(this->cache)->hook_directory_manage(addr, ai, s, w, true, true, writeback.first, delay);
          if(create){
            delete meta;
            meta = nullptr;
          }
          delete data;
        }
      }
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
    int32_t hit = this->cache->hit(addr, &ai, &s, &w);
    CMMetadataBase* meta = nullptr;
    CMDataBase* data = nullptr;
    bool create = false;

    if(hit != 0){
      if(hit == 1) {
        std::tie(meta, data) = this->cache->access_line(ai, s, w); // need c++17 for auto type infer
      }else if (hit == 2){
        meta = dynamic_cast<ExclusiveCacheSupportBase *>(this->cache)->access_directory_meta(ai, s, w);
        std::tie(meta, data, create) = dynamic_cast<ExclusivePolicySupportBase *>(this->policy)->acquire_need_create(meta);
        // sync if necessary
        auto sync = OPUC::policy->probe_need_sync(outer_cmd, meta);
        if(sync.first) this->inner->probe_req(addr, meta, data, sync.second, delay);
      }
      bool writeback = OPUC::policy->probe_need_writeback(outer_cmd, meta, meta_outer, this->coh_id);
      if(writeback) { 
        if(meta->is_dirty()){
          meta_outer->to_dirty();
          meta->to_clean();
        }
        if(data_outer) data_outer->copy(data);
      }
      // update meta
      OPUC::policy->meta_after_probe(outer_cmd, meta);
      if(hit == 2){
        delete data;
        data = nullptr;
        if(create){
          delete meta;
          meta = nullptr;
        }
      }
    }
    this->cache->hook_manage(addr, ai, s, w, hit, OPUC::policy->is_outer_evict(outer_cmd), writeback, delay);
  }
};

typedef ExclusiveOuterCohPortT<OuterCohPortUncached> ExclusiveOuterCohPort;

typedef ExclusiveInnerCohPortT<ExclusiveInnerPortUncached> ExclusiveInnerCohPort;

#endif
