#ifndef CM_CACHE_DYNAMIC_RANDOM_HPP
#define CM_CACHE_DYNAMIC_RANDOM_HPP

#include "cache/coherence.hpp"
#include "util/monitor.hpp"
#include <numeric>

#define MAGIC_ID_REMAP_ASK  2024091300ul
#define MAGIC_ID_REMAP_END  2024102700ul
#define EvictDistRelocEvent 2024081000ull

struct RemapHelper {
  static const unsigned int replace_for_relocate = 2408200ul;
  static const unsigned int replace_during_remap = 2408201ul;
};

// Dynamic-Randomized Skewed Cache 
// IW: index width, NW: number of ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, int P, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon>
  requires C_DERIVE<MT, CMMetadataBase> 
        && C_DERIVE_OR_VOID<DT, CMDataBase>
        && C_DERIVE<IDX, IndexSkewed<IW, 6, P>>
        && C_DERIVE_OR_VOID<DLY, DelayBase>
class CacheRemap : public CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon>, protected RemapHelper
{
  typedef CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon> CacheT;

protected:
  using CacheT::indexer;
  using CacheT::arrays;
  using CacheT::replacer;
  using CacheT::loc_random;

  IDX indexer_next;
  std::vector<uint64_t> indexer_seed_next;
  std::vector<uint64_t> remap_pointer;
  bool remap;

  virtual void replace_choose_set(uint64_t addr, uint32_t *ai, uint32_t *s, unsigned int genre) override {
    if constexpr (P==1) *ai = 0;
    else                *ai = ((*loc_random)() % P);
    if(0 == genre) *s = indexer.index(addr, *ai);
    else if(replace_for_relocate == genre) *s = indexer_next.index(addr, *ai);
    else {
      assert(replace_during_remap == genre);
      assert(0 == "remap in multithread simulation is not supported yet!");
      *ai = -1; //force a segment error in release mode
    }
  }

public:
  CacheRemap(std::string name = "", unsigned int extra_par = 0, unsigned int extra_way = 0) 
  : CacheT(name, extra_par, extra_way), remap(false) {
    remap_pointer.resize(P, 0);
    indexer_seed_next.resize(P);
    std::generate(indexer_seed_next.begin(), indexer_seed_next.end(), cm_get_random_uint64);
    indexer_next.seed(indexer_seed_next);
  }
  virtual ~CacheRemap() {}

  void rotate_indexer() {
    indexer.seed(indexer_seed_next);
    std::generate(indexer_seed_next.begin(), indexer_seed_next.end(), cm_get_random_uint64);
    indexer_next.seed(indexer_seed_next);
  }

  virtual bool hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, uint16_t prio, bool check_and_set) override {
    if(!remap) return CacheT::hit(addr, ai, s, w, prio, check_and_set);
    for(*ai=0; *ai<P; (*ai)++) {
      *s = indexer.index(addr, *ai);
      if(*s >= remap_pointer[*ai]){
        if (arrays[*ai]->hit(addr, *s, w)) return true;
      }
      *s = indexer_next.index(addr, *ai);
      if (arrays[*ai]->hit(addr, *s, w)) return true;
    }
    return false;
  }

  void move_remap_pointer(uint32_t ai) { remap_pointer[ai]++; }

  void remap_start() { remap = true; }
  void remap_end() {
    remap = false;
    std::fill(remap_pointer.begin(), remap_pointer.end(), 0);
    rotate_indexer();
    for(uint32_t ai = 0; ai < P; ai++){
      for(uint32_t idx = 0; idx < 1ul<<IW; idx++){
        for(uint32_t way = 0; way < NW; way++){
          static_cast<MT *>(this->access(ai, idx, way))->to_unrelocated();
        }
      }
    }
  }

};

template <typename CT, typename MT, typename Policy, bool EnMT = false> 
class InnerCohPortRemapT : public InnerCohPortT<InnerCohPortUncached, Policy, EnMT>, protected RemapHelper
{
  typedef InnerCohPortT<InnerCohPortUncached, Policy, EnMT> InnerT;


protected:
  using InnerT::cache;
  bool remap_flag;

public:
  InnerCohPortRemapT() : remap_flag(false){}
  void remap(){
    auto [p, s, w] = cache->size();
    uint32_t P, nset, nway;
    std::tie(P, nset, nway) = std::make_tuple(static_cast<uint32_t>(p), static_cast<uint32_t>(s), static_cast<uint32_t>(w));
    // cache->monitors->pause();
    static_cast<CT *>(cache)->remap_start();
    for(uint32_t ai = 0; ai < P; ai++){
      for(uint32_t idx = 0; idx < nset; idx++){
        for(uint32_t way = 0; way < nway; way++){
          relocation_chain(ai, idx, way);
        }
        static_cast<CT *>(cache)->move_remap_pointer(ai);
      }
    }
    static_cast<CT *>(cache)->remap_end();
    // cache->monitors->resume();
  }
  
  virtual void finish_resp(uint64_t addr, coh_cmd_t outer_cmd){
    cache->monitors->magic_func(addr, MAGIC_ID_REMAP_ASK, &remap_flag);
    if(remap_flag){
      remap();
      cache->monitors->magic_func(addr, MAGIC_ID_REMAP_END, nullptr);
      remap_flag = false;
    }
    InnerT::finish_resp(addr, outer_cmd);
  }

protected:
  void relocation(CMMetadataBase* c_meta, CMDataBase* c_data, uint64_t& c_addr) {
    uint32_t new_ai, new_idx, new_way;
    cache->replace(c_addr, &new_ai, &new_idx, &new_way, XactPrio::acquire, replace_for_relocate);
    auto[m_meta, m_data] = cache->access_line(new_ai, new_idx, new_way);
    uint64_t m_addr = m_meta->addr(new_idx); 
    if (m_meta->is_valid()) {
      if (static_cast<MT *>(m_meta)->is_relocated())
        this->evict(m_meta, m_data, new_ai, new_idx, new_way, nullptr);
      else
        cache->replace_manage(new_ai, new_idx, new_way, true, 1);
    }
    static_cast<CT *>(cache)->swap(m_addr, c_addr, m_meta, c_meta, m_data, c_data);
    cache->replace_read(new_ai, new_idx, new_way, false);
    cache->monitor_magic_func(c_addr, EvictDistRelocEvent, &new_idx);
    static_cast<MT *>(m_meta)->to_relocated();
    c_addr = m_addr;
  }

  void relocation_chain(uint32_t ai, uint32_t idx, uint32_t way) {
    auto[meta, data] = cache->access_line(ai, idx, way);
    uint64_t c_addr = meta->addr(idx);
    if (!meta->is_valid() || static_cast<MT *>(meta)->is_relocated()) return;
    auto c_meta = cache->meta_copy_buffer();
    auto c_data = data ? cache->data_copy_buffer() : nullptr;
    static_cast<CT *>(cache)->relocate(c_addr, meta, c_meta, data, c_data);
    static_cast<MT *>(meta)->to_relocated();
    cache->replace_manage(ai, idx, way, true, 1);

    while(c_meta->is_valid()) relocation(c_meta, c_data, c_addr);

    cache->meta_return_buffer(c_meta);
    cache->data_return_buffer(c_data);
  }
};

// Remap Monitor Base
class RemapperBase : public SimpleAccMonitor
{
protected:
  bool remap = false;
  bool remap_enable;

public:
  RemapperBase(bool remap_enable = true) : SimpleAccMonitor(true), remap_enable(remap_enable) {}

  virtual bool magic_func(uint64_t cache_id, uint64_t addr, uint64_t magic_id, void *magic_data) override {
    if (magic_id == MAGIC_ID_REMAP_ASK) {
      if(remap_enable){
        *static_cast<bool*>(magic_data) |= remap;
        if(remap) active = false;
      }
      return true;
    }
    if (magic_id == MAGIC_ID_REMAP_END) {
      remap = false;
      active = true;
      return true;
    }
    return false;
  } 
};

// Simple Remap Monitor
class SimpleEVRemapper : public RemapperBase
{
protected:
  uint64_t period;

public:
  SimpleEVRemapper(uint64_t period) : period(period) {}

  virtual void invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, const CMMetadataBase *meta, const CMDataBase *data) override {
    if(!active) return;
    cnt_invalid++;
    if(cnt_invalid !=0 && (cnt_invalid % period) == 0) {
      remap = true;
    }
  }
};

// ACC Remap Monitor
class ACCRemapper : public RemapperBase
{
protected:
  uint64_t period;

public:
  ACCRemapper(uint64_t period) : period(period) {}

  virtual void read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data)  override {
    if(!active) return;
    cnt_access++;
    if(!hit) cnt_miss++;
    if(cnt_access !=0 && (cnt_access % period) == 0) {
      remap = true;
    }
  }
};

template<int IW>
class ZSEVRemapper : public RemapperBase
{
protected:
  static constexpr uint32_t nset = 1ul << IW;
  const double factor;
  const double threshold;
  const uint64_t access_period, evict_period;
  std::array<uint64_t, nset> evicts{};
  std::array<double, nset> set_evict_history{};

  bool Z_Score_detect(){
    double qrm  = sqrt(std::accumulate(evicts.begin(), evicts.end(), 0.0,
                                      [](double q, const uint64_t& d){return q + d * d;})
                      / (nset-1.0)
                      );
    double mu   = std::accumulate(evicts.begin(), evicts.end(), 0.0) / (double)(nset);
    for(size_t i=0; i<nset; i++) {
      double delta = qrm == 0.0 ? 0.0 : (evicts[i] - mu) * evicts[i] / qrm;
      set_evict_history[i] = (1 - factor) * set_evict_history[i] + factor * (evicts[i] > mu ? delta : -delta);
    }

    for(size_t i=0; i<nset; i++){
      if(set_evict_history[i] >= threshold) {
        // std::cerr << "found set " << i << std::endl;
        return true;
      }
    }
    return false;
  }

public:
  ZSEVRemapper(const double factor, uint64_t access_period, uint64_t evict_period, double th, bool remap_enable = true)
    : RemapperBase(remap_enable), factor(factor), threshold(th),
      access_period(access_period), evict_period(evict_period){}

  virtual void read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data)  override {
    if(!active) return;
    cnt_access++;
    if(!hit) cnt_miss++;
    if(access_period != 0 && (cnt_access % access_period) == 0) {
      if(Z_Score_detect()) {
        remap = true;
      }
      evicts.fill(0);
    }
  }
  virtual void write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data) override {
    if(!active) return;
    cnt_write++;
    if(!hit) {
      cnt_write_miss++;
    }
  }

  virtual void invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, const CMMetadataBase *meta, const CMDataBase *data) override {
    if(!active) return;
    cnt_invalid++;
    evicts[s]++;
    if(evict_period != 0 && (cnt_invalid % evict_period) == 0) {
      remap = true;
      // std::cerr << " remapped by eviction limit @" << cnt_invalid  << std::endl;
    }
  }

  virtual bool magic_func(uint64_t cache_id, uint64_t addr, uint64_t magic_id, void *magic_data) override {
    if (magic_id == MAGIC_ID_REMAP_ASK) {
      if(remap_enable){
        *static_cast<bool*>(magic_data) |= remap;
        if(remap) active = false;
      }
      return true;
    }
    if (magic_id == MAGIC_ID_REMAP_END) {
      remap = false;
      active = true;
      cnt_access = 0; cnt_miss = 0; cnt_write = 0;  cnt_write_miss = 0; cnt_invalid = 0;
      evicts.fill(0); set_evict_history.fill(0.0);
      return true;
    }
    return false;
  }
};

#endif
