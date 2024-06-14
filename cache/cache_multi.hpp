#ifndef CM_CACHE_CACHE_MULTI_HPP
#define CM_CACHE_CACHE_MULTI_HPP

#include "cache/cache.hpp"
#include <mutex>
#include <condition_variable>
#include <tuple>

// Multi-thread support for Cache Array
class CacheArrayMultiThreadSupport
{
public:
  virtual std::vector<uint32_t> *get_status() = 0; 
  virtual std::mutex* get_mutex(uint32_t s) = 0; // get set mutex
  virtual std::mutex* get_cacheline_mutex(uint32_t s, uint32_t w) = 0; // get cacheline mutex
  virtual std::condition_variable* get_cv(uint32_t s) = 0; // get set cv
};

// Multi-thread Cache Array
// IW: index width, NW: number of ways, MT: metadata type, DT: data type (void if not in use)
template<int IW, int NW, typename MT, typename DT>
  requires C_DERIVE<MT, CMMetadataCommon> 
        && C_DERIVE_OR_VOID(DT, CMDataBase)
class CacheArrayMultiThread : public CacheArrayNorm<IW, NW, MT, DT>, 
                              public CacheArrayMultiThreadSupport
{

  typedef CacheArrayNorm<IW, NW, MT, DT> CacheAT;
protected:
  std::vector<uint32_t> status; // record every set status
  std::vector<std::mutex *> status_mtxs; // mutex for status
  std::vector<std::mutex *> mutexs;  // mutex array for meta
  std::vector<std::condition_variable *> cvs; // cv array, used in conjunction with mutexes

public:
  using CacheAT::nset;
  using CacheAT::way_num;
  CacheArrayMultiThread(unsigned int extra_way = 0, std::string name = "") : CacheAT(extra_way, name){
    size_t meta_num = nset * way_num;
    status.resize(nset);
    for(uint32_t i = 0; i < nset; i++) status[i] = 0;

    mutexs.resize(meta_num);
    for(auto &t:mutexs) t = new std::mutex();

    status_mtxs.resize(nset);
    for(auto &s : status_mtxs) s = new std::mutex();

    cvs.resize(nset);
    for(auto &c : cvs) c = new std::condition_variable();
  }

  virtual ~CacheArrayMultiThread(){
    for(auto t: mutexs) delete t;
    for(auto s : status_mtxs) delete s;
    for(auto c : cvs) delete c;
  }

  virtual std::mutex* get_cacheline_mutex(uint32_t s, uint32_t w) { return mutexs[s*(way_num) + w]; }

  virtual std::vector<uint32_t> *get_status(){ return &status; }
  virtual std::mutex* get_mutex(uint32_t s) { return status_mtxs[s]; }
  virtual std::condition_variable* get_cv(uint32_t s) { return cvs[s]; }
};


// Multi-thread support for CacheBase
class CacheBaseMultiThreadSupport
{
public:
  virtual std::vector<uint32_t> *get_status(uint32_t ai) = 0;
  virtual std::mutex* get_mutex(uint32_t ai, uint32_t s) = 0;
  virtual std::condition_variable* get_cv(uint32_t ai, uint32_t s) = 0;
  virtual std::mutex* get_cacheline_mutex(uint32_t ai, uint32_t s, uint32_t w) = 0;

  // get set's status, mutex and cv in one function call
  virtual std::tuple<std::vector<uint32_t> *, std::mutex*, std::condition_variable*> 
          get_set_control(uint32_t ai, uint32_t s) = 0; 

  virtual bool hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, 
                   uint16_t priority, bool need_replace = false) = 0;
};


// Multi-thread Skewed Cache 
// IW: index width, NW: number of ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
// EF: empty first in replacer
template<int IW, int NW, int P, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon, bool EF = true>
  requires C_DERIVE<MT, CMMetadataBase> 
        && C_DERIVE_OR_VOID(DT, CMDataBase)
        && C_DERIVE<IDX, IndexFuncBase> 
        && C_DERIVE<RPC, ReplaceFuncBaseMT<EF>>
        && C_DERIVE_OR_VOID(DLY, DelayBase)
class CacheSkewedMultiThread : public CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon>, 
                               public CacheBaseMultiThreadSupport
{
  typedef CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon> CacheT;
  typedef CacheArrayMultiThread<IW, NW, MT, DT> CacheAT;

protected:
  using CacheT::arrays;
  using CacheT::indexer;
  using CacheT::access;
  using CacheT::replace;
public:
  CacheSkewedMultiThread(std::string name = "", unsigned int extra_par = 0, unsigned int extra_way = 0) 
  : CacheT(name, extra_par, extra_way)
  {
    for(int i=0; i<P; i++) {
      delete arrays[i];
      arrays[i] = new CacheAT(extra_way);
    }
  }

  virtual std::vector<uint32_t> *get_status(uint32_t ai){
    return (static_cast<CacheAT*>(arrays[ai]))->get_status();
  }
  virtual std::mutex* get_mutex(uint32_t ai, uint32_t s){
    return (static_cast<CacheAT*>(arrays[ai]))->get_mutex(s);
  }
  virtual std::condition_variable* get_cv(uint32_t ai, uint32_t s) {
    return (static_cast<CacheAT*>(arrays[ai]))->get_cv(s);
  }

  virtual std::tuple<std::vector<uint32_t> *, std::mutex*, std::condition_variable*> 
          get_set_control(uint32_t ai, uint32_t s)
  {
    return std::make_tuple(get_status(ai), get_mutex(ai, s), get_cv(ai, s));
  }

  virtual std::mutex* get_cacheline_mutex(uint32_t ai, uint32_t s, uint32_t w){
    return (static_cast<CacheAT*>(arrays[ai]))->get_cacheline_mutex(s, w);
  }
  

  virtual bool hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, 
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
      uint32_t idx = *s;
      auto [status, mtx, cv] = get_set_control(*ai, *s);
      std::unique_lock lk(*mtx, std::defer_lock);
      lk.lock();
      /** Wait until the high priority thread ends (lower the priority of the set)  */
      cv->wait(lk, [idx, status, priority] { return ((*status)[idx] < priority);} );
      (*status)[*s] |= priority;
      lk.unlock();

      for(*w = 0; *w < NW; (*w)++){
        if(access(*ai, *s, *w)->match(addr)) { hit = true; break;}
      }
      if(hit) break;
    }

    if(need_replace && !hit) replace(addr, ai, s, w);

    /** if don't replace, then *ai=P, else if replace occurs, then 0<=(*ai)< P */
    for(uint32_t i = 0; i < P; i++){
      if(i != *ai){
        uint32_t s = indexer.index(addr, i);
        auto [status, mtx, cv] = get_set_control(i, s);
        std::unique_lock lk(*mtx, std::defer_lock);
        lk.lock();
        (*status)[s] &= ~(priority);
        lk.unlock();
        cv->notify_all();
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
