#ifndef CM_CHAMELEON_CACHE__HPP
#define CM_CHAMELEON_CACHE__HPP

#include "cache/coherence.hpp"
#include "chameleon/replace.hpp"
#include "rsa/monitor.hpp"

struct ChameleonHelper {
  static const unsigned int replace_for_victim = 2409190ul;
};

// Chameleon Cache 
// IW: index width, NW: number of ways, VW: number of victim ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, int VW, int P, typename MT, typename DT, typename IDX, typename RPC, typename VRPC, typename DLY, bool EnMon, bool EnMT = false>
  requires C_DERIVE<MT, CMMetadataBase> 
        && C_DERIVE_OR_VOID<DT, CMDataBase>
        && C_DERIVE<IDX, IndexSkewed<IW, 6, P>>
        && C_DERIVE_OR_VOID<DLY, DelayBase>
class ChameleonCache : public CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon>, protected ChameleonHelper
{
  typedef CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon> CacheT;

protected:
  using CacheT::arrays;
  using CacheT::replacer;
  using CacheT::loc_random;
  VRPC v_replacer; // victim replacer
  bool need_reinsert = false;

  virtual void replace_choose_set(uint64_t addr, uint32_t *ai, uint32_t *s, unsigned int genre) override {
    if(replace_for_victim == genre){ *ai = P; *s = 0;}
    else CacheT::replace_choose_set(addr, ai, s, genre);
  }

public:
  ChameleonCache(std::string name = "", unsigned int extra_par = 1, unsigned int extra_way = 0) 
  : CacheT(name, extra_par, extra_way) {
    arrays[P] =  new CacheArrayNorm<0,VW,MT,DT,EnMT>(extra_way);
  }

  bool check_victim_entry(uint32_t ai, uint32_t s, uint32_t w){
    return (ai == P) && (s == 0) && (w < VW);
  }

  bool need_auto_reinsert(){ return need_reinsert; }

  virtual std::pair<CMMetadataBase *, CMDataBase *> access_line(uint32_t ai, uint32_t s, uint32_t w) override {
    auto base_line = CacheT::access_line(ai, s, w);
    if constexpr (!C_VOID<DT>){
      if(ai == P && w >= NW) return std::make_pair(base_line.first, w < VW ? arrays[ai]->get_data(s, w) : nullptr);
    }
    return base_line;
  }

  virtual bool replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, uint16_t prio, unsigned int genre = 0) override {
    replace_choose_set(addr, ai, s, genre);
    if(*ai == P){
      v_replacer.replace(*s, w);
      if(*w == VW-1) need_reinsert = true;
    }
    else replacer[*ai].replace(*s, w);
    return true;
  }

  virtual bool hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, uint16_t prio, bool check_and_set) override {
    if(CacheT::hit(addr, ai, s, w, prio, check_and_set)) return true;
    *ai = P; *s = 0;
    if(arrays[*ai]->hit(addr, *s, w)) return true;
    return false;
  }

  virtual void replace_read(uint32_t ai, uint32_t s, uint32_t w, bool prefetch, bool genre = false) override {
    if(ai == P) v_replacer.access(s, w, true, false);
    else replacer[ai].access(s, w, true, prefetch);
  }

  virtual void replace_manage(uint32_t ai, uint32_t s, uint32_t w, bool hit, uint32_t evict, bool genre = false) override {
    if(ai == P && hit && evict) v_replacer.access(s, w, true, false);
    else CacheT::replace_manage(ai, s, w, hit, evict);
  }

  std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t>
  reinsert(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, CMMetadataBase * meta, CMDataBase *data, uint16_t prio, uint64_t *delay){
    if(!meta->is_valid()){
      return std::make_tuple(meta, data, ai, s, w);
    }
    uint32_t swap_ai, swap_s, swap_w;
    replace(addr, &swap_ai, &swap_s, &swap_w, prio);
    auto [swap_meta, swap_data] = this->access_line(swap_ai, swap_s, swap_w);
    replace_manage(ai, s, w, true, 1);
    if(swap_meta->is_valid()){
      uint64_t swap_addr = swap_meta->addr(swap_s);
      replace_manage(swap_ai, swap_s, swap_w, true, 1);
      this->swap(addr, swap_addr, meta, swap_meta, data, swap_data);
      replace_read(ai, s, w, false);
      this->monitor_magic_func(swap_addr, EvictDistRelocEvent, &s);
    } else{
      this->relocate(addr, meta, swap_meta, data, swap_data);
    }
    replace_read(swap_ai, swap_s, swap_w, false);
    this->monitor_magic_func(addr, EvictDistRelocEvent, &swap_s);
    return std::make_tuple(swap_meta, swap_data, swap_ai, swap_s, swap_w);
  }

  void auto_reinsert(uint16_t prio, uint64_t *delay){
    // auto reinsert_way = v_replacer.reinsert(0);
    // for(const auto& way : reinsert_way){
    for(uint32_t way = 0; way < VW; way++){
      auto [meta, data] = this->access_line(P, 0, way);
      uint64_t addr = meta->addr(0);
      reinsert(addr, P, 0, way, meta, data, prio, delay);
    }
    need_reinsert = false;
  }

};

template <typename CT, typename Policy, bool EnMT = false> 
class InnerCohPortChameleonT : public InnerCohPortT<InnerCohPortUncached, Policy, EnMT>, protected ChameleonHelper
{
  typedef InnerCohPortT<InnerCohPortUncached, Policy, EnMT> InnerT;

protected:
  using InnerT::cache;
  using InnerT::outer;

  virtual void write_line(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) {
    InnerT::write_line(addr, data_inner, meta_inner, cmd, delay);
    if(static_cast<CT *>(cache)->need_auto_reinsert()){
      static_cast<CT *>(cache)->auto_reinsert(XactPrio::acquire, delay);
    }
  }

  virtual void evict(CMMetadataBase *meta, CMDataBase *data, int32_t ai, uint32_t s, uint32_t w, uint64_t *delay) {
    // evict a block due to conflict
    auto addr = meta->addr(s);
    assert(cache->hit(addr));
    uint32_t victim_ai, victim_s, victim_w;
    cache->replace(addr, &victim_ai, &victim_s, &victim_w, XactPrio::acquire, replace_for_victim);
    auto [victim_meta, victim_data] = cache->access_line(victim_ai, victim_s, victim_w);

    if(victim_meta->is_valid()) InnerT::evict(victim_meta, victim_data, victim_ai, victim_s, victim_w, delay);

    cache->replace_manage(ai, s, w, true, 1);
    static_cast<CT *>(cache)->relocate(addr, meta, victim_meta, data, victim_data);
    cache->replace_read(victim_ai, victim_s, victim_w, false);
    cache->monitor_magic_func(addr, EvictDistRelocEvent, &victim_s);
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, bool>
  access_line(uint64_t addr, coh_cmd_t cmd, uint16_t prio, uint64_t *delay) { // common function for access a line in the cache
    auto [meta, data, ai, s, w, hit] = InnerT::access_line(addr, cmd, prio, delay);
    if(hit && static_cast<CT *>(cache)->check_victim_entry(ai, s, w)){
      return std::tuple_cat(static_cast<CT *>(cache)->reinsert(addr, ai, s, w, meta, data, prio, delay), std::make_tuple(hit));
    }
    return std::make_tuple(meta, data, ai, s, w, hit);
  }

public:
  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) override {
    InnerT::acquire_resp(addr, data_inner, meta_inner, cmd, delay);
    if(static_cast<CT *>(cache)->need_auto_reinsert()){
      static_cast<CT *>(cache)->auto_reinsert(XactPrio::acquire, delay);
    }
  }

};

#endif
