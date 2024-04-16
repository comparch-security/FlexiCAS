#ifndef CM_REPLACE_MULTI_HPP_
#define CM_REPLACE_MULTI_HPP_

#include "cache/replace.hpp"
#include <mutex>

///////////////////////////////////
// Multi-thread support for Replacer
class ReplaceMultiThreadSupport
{
protected:
  std::vector<std::mutex *> mtxs;
public:
  const uint32_t nset;
  ReplaceMultiThreadSupport(uint32_t nset) : nset(nset) { for (uint32_t i = 0; i < nset; i++) mtxs.emplace_back(new std::mutex());}

  virtual ~ReplaceMultiThreadSupport(){
    for(auto m : mtxs) delete m;
  }
};

/////////////////////////////////
// multi-thread FIFO replacement
// IW: index width, NW: number of ways, EF: empty first, DUO: demand update only (do not update state for release)
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceFIFOMultiThread : public ReplaceFIFO<IW, NW, EF, DUO>, public ReplaceMultiThreadSupport
{
  typedef ReplaceFIFO<IW, NW, EF, DUO> ReT;
protected:
  using ReT::used_map;
  using ReT::free_map;

  std::vector<std::vector<bool> > using_map; // set the way being used by the thread to true
public:
  ReplaceFIFOMultiThread() : ReT(), ReplaceMultiThreadSupport(1ul << IW), using_map(1ul << IW){
    for(auto &s : using_map) for(uint32_t i = 0; i < NW; i++) s.emplace_back(false);
  }
  virtual ~ReplaceFIFOMultiThread() {}

  virtual uint32_t replace(uint32_t s, uint32_t *w, uint32_t op = 0){
    std::unique_lock lk(*mtxs[s]);
    if constexpr(EF){
      if(!free_map[s].empty()){
        *w = *(free_map[s].cbegin());
        free_map[s].erase(*w);
      }else{
        *w = used_map[s].front();
        used_map[s].remove(*w);
      }
    }
    else{
      *w = used_map[s].front();
      used_map[s].remove(*w);
    }
    using_map[s][*w] = true;
    return free_map[s].size();
  }

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) {
    std::unique_lock lk(*mtxs[s]);
    if(using_map[s][w]){
      using_map[s][w] = false;
      used_map[s].push_back(w);
    }
  }

  virtual void invalid(uint32_t s, uint32_t w){
    std::unique_lock lk(*mtxs[s]);
    if(!using_map[s][w]){
      if constexpr (EF) used_map[s].remove(w);
      free_map[s].insert(w);
    }
  }
};

/////////////////////////////////
// multi-thread LRU replacement
// IW: index width, NW: number of ways, EF: empty first, DUO: demand update only (do not update state for release)
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceLRUMultiThread : public ReplaceFIFOMultiThread<IW, NW, EF, DUO>
{
  typedef ReplaceFIFOMultiThread<IW, NW, EF, DUO> RefT;

protected:
  using RefT::free_map;
  using RefT::used_map;
  using RefT::using_map;
  using RefT::mtxs;

public:
  ReplaceLRUMultiThread() : RefT() {}
  ~ReplaceLRUMultiThread() {}

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0){
    std::unique_lock lk(*mtxs[s]);
    if constexpr (EF){
      if(using_map[s][w]){
        using_map[s][w] = false;
        used_map[s].push_back(w);
      } else if(!DUO || !release){
        used_map[s].remove(w);
        used_map[s].push_back(w);
      }
    }else{
      if(using_map[s][w]) using_map[w] = false;
      if(!DUO || !release) {
        used_map[s].remove(w);
        used_map[s].push_back(w);
      }
    }
  }
};

#endif
