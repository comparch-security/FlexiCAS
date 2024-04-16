#ifndef CM_CACHE_CACHE_MULTI_HPP
#define CM_CACHE_CACHE_MULTI_HPP

#include "cache/cache.hpp"
#include "cache/replace_multi.hpp"
#include <mutex>
#include <condition_variable>

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
  requires C_DERIVE(MT, CMMetadataCommon) && C_DERIVE_OR_VOID(DT, CMDataBase)
class CacheArrayMultiThread : public CacheArrayNorm<IW, NW, MT, DT>, public CacheArrayMultiThreadSupport
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
};


// Multi-thread Skewed Cache 
// IW: index width, NW: number of ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, int P, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon>
  requires C_DERIVE(MT, CMMetadataBase) && C_DERIVE_OR_VOID(DT, CMDataBase) &&
           C_DERIVE(IDX, IndexFuncBase) && C_DERIVE2(RPC, ReplaceFuncBase, ReplaceMultiThreadSupport) && C_DERIVE_OR_VOID(DLY, DelayBase)
class CacheSkewedMultiThread : public CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon>, public CacheBaseMultiThreadSupport
{
  typedef CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon> CacheT;
  typedef CacheArrayMultiThread<IW, NW, MT, DT> CacheAT;

protected:
  using CacheT::arrays;

public:
  CacheSkewedMultiThread(std::string name = "", unsigned int extra_par = 0, unsigned int extra_way = 0) : CacheT(name, extra_par, extra_way)
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

  virtual std::mutex* get_cacheline_mutex(uint32_t ai, uint32_t s, uint32_t w){
    return (static_cast<CacheAT*>(arrays[ai]))->get_cacheline_mutex(s, w);
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