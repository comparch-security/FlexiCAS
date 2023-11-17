#ifndef CM_CACHE_EXCLUSIVE_HPP
#define CM_CACHE_EXCLUSIVE_HPP

#include "cache/coherence.hpp"
#include "cache/msi.hpp"

class ExclusivePolicySupportBase
{
public:
  virtual void meta_after_release(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase* directory_meta, uint64_t addr, bool dirty) = 0;
  virtual std::pair<bool, coh_cmd_t> release_need_probe(coh_cmd_t cmd, CMMetadataBase* meta) = 0;
};


template<typename MT, bool isLLC> requires C_DERIVE(MT, MetadataBroadcastBase)
class ExclusiveMSIPolicy : public MSIPolicy<MT, false, isLLC>, public ExclusivePolicySupportBase    // always not L1
{
public:
  virtual void meta_after_release(coh_cmd_t cmd, CMMetadataBase *mmeta, CMMetadataBase* meta, uint64_t addr, bool dirty){
    // meta transfer from directory (if use directory coherence protocol) to cache
    mmeta->init(addr);
    mmeta->to_shared(-1);
    if(meta) { 
      if constexpr (C_DERIVE(MT, MetadataDirectoryBase))
        static_cast<MT*>(mmeta)->set_sharer(static_cast<MT*>(meta)->get_sharer());
      assert(!meta->is_dirty());
      meta->to_invalid();
    }
    if(dirty) mmeta->to_dirty();
  }

  virtual std::pair<bool, coh_cmd_t> release_need_probe(coh_cmd_t cmd, CMMetadataBase* meta) {
    assert(this->is_release(cmd));
    return std::make_pair(true, coh_cmd_t{cmd.id, this->probe_msg, this->evict_act});
  }

  virtual bool need_writeback(const CMMetadataBase* meta) { return true; }

  virtual std::pair<bool, coh_cmd_t> inner_need_release(){
    return std::make_pair(true, this->cmd_for_release());
  }

  virtual void meta_after_probe_ack(coh_cmd_t cmd, CMMetadataBase *meta, int32_t inner_id) const{
    assert(this->is_probe(cmd));
    if(this->is_evict(cmd)) meta->sync(inner_id);
  }
};

// Skewed Exclusive Cache
// IW: index width, NW: number of ways, DW: Directory Ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type, DRPC: directory replacer type(if use direcotry)
// EnMon: whether to enable monitoring
// EnDir: whether to enable use directory
template<int IW, int NW, int DW, int P, typename MT, typename DT, typename IDX, typename RPC, typename DRPC, typename DLY, bool EnMon, bool EnDir>
class CacheSkewedExclusive : public CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon>
{
  typedef CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon> CacheSkewedT;
protected:
  DRPC ext_replacer[P];
public:
  CacheSkewedExclusive(std::string name = "") : CacheSkewedT(name, 0, (EnDir ? DW : 0)) {}

  virtual bool replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, unsigned int genre = 0){
    if(0 == genre)
      return CacheSkewedT::replace(addr, ai, s, w);

    if constexpr (!EnDir) return false;
    else {
      if constexpr (P==1) *ai = 0;
      else                *ai = (cm_get_random_uint32() % P);
      *s = this->indexer.index(addr, *ai);
      this->ext_replacer[*ai].replace(*s, w);
      *w += NW;
      return true;
    }
  }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) {
    if(w >= NW) {
      ext_replacer[ai].access(s, w-NW, false);
      /* ToDo: We need to use hook_manage to record this, potentially outer fetch without local store, snoopying exclusive */
    } else
      CacheSkewedT::hook_read(addr, ai, s, w, hit, delay);
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool is_release, uint64_t *delay) {
    if(w >= NW) {
      ext_replacer[ai].access(s, w-NW, is_release);
      /* ToDo: We need to use hook_manage to record this, potentially outer fetch without local store, snoopying exclusive */
    } else
      CacheSkewedT::hook_write(addr, ai, s, w, hit, is_release, delay);
  }

  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, uint64_t *delay) {
    if(w >= NW) {
      if(hit && evict) ext_replacer[ai].invalid(s, w-NW);
      /* ToDo: We need to use hook_manage to record this, potentially outer fetch without local store, snoopying exclusive */
    } else
      CacheSkewedT::hook_manage(addr, ai, s, w, hit, evict, writeback, delay);
  }

  virtual ~CacheSkewedExclusive(){}
};

// Norm Exclusive Cache with Extened Directory
// IW: index width, NW: number of ways, DW: Extended Directory Ways
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type, DRPC: directory replacer type(if use direcotry)
// EnMon: whether to enable monitoring
template<int IW, int NW, int DW, typename MT, typename DT, typename IDX, typename RPC, typename DRPC, typename DLY, bool EnMon>
using CacheNormExclusiveWithExtendedDir = CacheSkewedExclusive<IW, NW, DW, 1, MT, DT, IDX, RPC, DRPC, DLY, EnMon, true>;

// Norm Exclusive Cache
// IW: index width, NW: number of ways
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type,
// EnMon: whether to enable monitoring
// EnDir: whether to enable use directory
template<int IW, int NW, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon, bool EnDir=false>
using CacheNormExclusive = CacheSkewedExclusive<IW, NW, 0, 1, MT, DT, IDX, RPC, ReplaceRandom<1,1>, DLY, EnMon, EnDir>;

class ExclusiveInnerPortUncached : public InnerCohPortUncached
{
protected:
  using InnerCohPortBase::cache;
public:
  ExclusiveInnerPortUncached(CohPolicyBase *policy) : InnerCohPortUncached(policy) {}
  virtual ~ExclusiveInnerPortUncached() {}

protected:
  virtual void evict(CMMetadataBase *meta, CMDataBase *data, int32_t ai, uint32_t s, uint32_t w, uint64_t *delay) {
    InnerCohPortUncached::evict(meta, meta->is_extend() ? cache->copy_buffer() : data, ai, s, w, delay);
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, bool>
  access_line(uint64_t addr, CMDataBase* data_inner, coh_cmd_t cmd, uint64_t *delay) { // common function for access a line in the cache
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    bool hit = cache->hit(addr, &ai, &s, &w);
    if(hit) {
      meta = cache->access(ai, s, w);
      if(!meta->is_extend()) { // hit on cache
        data = cache->get_data(ai, s, w);
        auto promote = policy->acquire_need_promote(cmd, meta);
        if(promote.first) { outer->acquire_req(addr, meta, data, promote.second, delay); hit = 0; } // promote permission if needed
        // fetch to higher cache and invalid here
        evict(meta, data, ai, s, w, delay);
        meta = nullptr;
        return std::make_tuple(meta, data, ai, s, w, hit);
      } else { // hit on extend directory meta
        auto sync = policy->acquire_need_sync(cmd, meta);
        data = data_inner;
        if(sync.first) probe_req(addr, meta, data, sync.second, delay);
      }
    } else {
      // if use snooping coherence protocol, we also need probe other inner cache here
      bool probe_hit = false;
      data = data_inner;
      auto sync = policy->acquire_need_sync(cmd, meta);
      if(sync.first) probe_hit = probe_req(addr, meta, data, sync.second, delay);
      if(!probe_hit) {
        bool replace = cache->replace(addr, &ai, &s, &w, true);
        if(replace){
          // replace is true means use directory coherence protocol
          meta = cache->access(ai, s, w);
          if(meta->is_valid()) {
            assert(meta->is_extend());
            evict(meta, data, ai, s, w, delay);
          }
        }
        outer->acquire_req(addr, meta, data, cmd, delay);
      }
    }
    return std::make_tuple(meta, data, ai, s, w, hit);
  }
  virtual void write_line(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay, bool dirty = true) {
    uint32_t ai, s, w;
    uint32_t dai, ds, dw;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    CMMetadataBase* mmeta = nullptr;
    if(policy->is_writeback(cmd)) {
      outer->writeback_req(addr, nullptr, data_inner, cmd, delay);  // exclusive cache don't handle release_writeback
    } else {
      bool writeback = false; bool hit = this->cache->hit(addr, &dai, &ds, &dw);
      if(hit){
        meta = this->cache->access(dai, ds, dw);
        assert(meta->is_extend()); // must use directory coherence protocol 
        this->cache->hook_manage(addr, dai, ds, dw, true, true, false, delay);
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
      this->cache->hook_write(addr, ai, s, w, hit, true, delay);
    }
  }

  virtual void flush_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay){
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    bool hit = false;
    auto flush = policy->flush_need_sync(cmd, meta);
    if(flush.first) {
      hit = this->cache->hit(addr, &ai, &s, &w);
      if(hit){
        meta = this->cache->access(ai, s, w);
        if(!meta->is_extend()){
          // hit on cache
          data = this->cache->get_data(ai, s, w);
          auto writeback = policy->writeback_need_writeback(meta);
          if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay);
          this->cache->hook_manage(addr, ai, s, w, true, policy->is_evict(cmd), writeback.first, delay);
        }else{
          // hit on extend meta  
          probe_req(addr, meta, data, flush.second, delay);
          // probe data don't writeback(if dirty) to exclusive cache, then we just need invalid the extend meta
         this->cache->hook_manage(addr, ai, s, w, true, policy->is_evict(cmd), false, delay);
        }
      }else{
        probe_req(addr, meta, data, flush.second, delay);
      }
      policy->meta_after_flush(cmd, meta);
    } else outer->writeback_req(addr, nullptr, nullptr, policy->cmd_for_outer_flush(cmd), delay);
  }

};


template<class IPUC> requires C_DERIVE(IPUC, ExclusiveInnerPortUncached)
class ExclusiveInnerCohPortT : public IPUC
{
public:
  ExclusiveInnerCohPortT(CohPolicyBase *policy) : IPUC(policy) {}
  virtual ~ExclusiveInnerCohPortT() {}

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

template<class OPUC> requires C_DERIVE(OPUC, OuterCohPortUncached)
class ExclusiveOuterCohPortT : public OPUC
{
public:
  ExclusiveOuterCohPortT(CohPolicyBase *policy) : OPUC(policy) {}
  virtual ~ExclusiveOuterCohPortT() {}

  virtual bool probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, coh_cmd_t outer_cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    bool extend = false;
    bool hit = this->cache->hit(addr, &ai, &s, &w);
    CMMetadataBase* meta = nullptr;
    CMDataBase* data = nullptr;

    if(hit){
      meta = this->cache->access(ai, s, w);
      extend = meta->is_extend();
      if(!extend){
        // hit on cache
        data = this->cache->get_data(ai, s, w);
        if(data_outer) data_outer->copy(data);
      }else{
        // sync if necessary
        data = data_outer;
        auto sync = OPUC::policy->probe_need_sync(outer_cmd, meta);
        if(sync.first) this->inner->probe_req(addr, meta, data, sync.second, delay);
      }
    }else{
      data = data_outer;
      // sync if necessary
      auto sync = OPUC::policy->probe_need_sync(outer_cmd, meta);
      if(sync.first) this->inner->probe_req(addr, meta, data, sync.second, delay);
    }
    auto writeback = OPUC::policy->probe_need_writeback(outer_cmd, meta);
    if(writeback.first) this->writeback_req(addr, meta, data, writeback.second, delay);

    OPUC::policy->meta_after_probe(outer_cmd, meta, meta_outer, OPUC::coh_id);

    this->cache->hook_manage(addr, ai, s, w, hit, OPUC::policy->is_outer_evict(outer_cmd), writeback.first, delay, extend);

    return hit;
  }
};

typedef ExclusiveOuterCohPortT<OuterCohPortUncached> ExclusiveOuterCohPort;
typedef ExclusiveInnerCohPortT<ExclusiveInnerPortUncached> ExclusiveInnerCohPort;

#endif
