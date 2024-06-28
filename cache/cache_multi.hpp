#ifndef CM_CACHE_CACHE_MULTI_HPP
#define CM_CACHE_CACHE_MULTI_HPP

#include "cache/cache.hpp"

// Multi-thread Skewed Cache 
// IW: index width, NW: number of ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
// EF: empty first in replacer
template<int IW, int NW, int P, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon, bool EF = true>
  requires C_DERIVE<MT, CMMetadataBase> && C_DERIVE_OR_VOID<DT, CMDataBase> && C_DERIVE<IDX, IndexFuncBase> &&
           C_DERIVE_OR_VOID<DLY, DelayBase>
class CacheSkewedMultiThread : public CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon, EF, true>
{
  typedef CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon, EF, true> CacheT;
  typedef CacheArrayNorm<IW, NW, MT, DT, true> CacheAT;

protected:
  using CacheT::arrays;
  using CacheT::indexer;
  using CacheT::access;
  using CacheT::replace;
public:
  CacheSkewedMultiThread(std::string name = "", unsigned int extra_par = 0, unsigned int extra_way = 0) 
    : CacheT(name, extra_par, extra_way) {}

  virtual bool hit_replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, 
                   uint16_t priority, bool need_replace = false)
  {
    /**
     * When using multi-threaded cache, determining hit for an address depends on what behavior it is performing 
     * (acquire, probe, or release) and the priority of that behavior. When a high priority behavior (thread) 
     * is working, a low priority behavior (thread) needs to be blocked until the high priority behavior ends
     */
    bool hit = false;
    for(*ai = 0; *ai < P; (*ai)++){
      *s = indexer.index(addr, *ai);
      this->set_mt_state(*ai, *s, priority);

      for(*w = 0; *w < NW; (*w)++){
        if(access(*ai, *s, *w)->match(addr)) { hit = true; break;}
      }
      if(hit) break;
    }

    if(need_replace && !hit) replace(addr, ai, s, w);

    /** if don't replace, then *ai=P, else if replace occurs, then 0<=(*ai)< P */
    for(uint32_t i = 0; i < P; i++){
      if(i != *ai){
        this->reset_mt_state(i, indexer.index(addr, i), priority);
      }
    }
    return hit;
  }

  virtual ~CacheSkewedMultiThread() {}

};

// Multi-thread normal set-associative cache
// IW: index width, NW: number of ways
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon>
using CacheNormMultiThread = CacheSkewedMultiThread<IW, NW, 1, MT, DT, IDX, RPC, DLY, EnMon>;

#endif
