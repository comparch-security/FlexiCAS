#ifndef CM_REPLACE_HPP_
#define CM_REPLACE_HPP_

#include <cstdint>
#include <cstdio>
#include <list>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <cassert>
#include "util/random.hpp"

///////////////////////////////////
// Base class

class ReplaceFuncBase
{
protected:
  const uint32_t nset;
  std::mutex mtx;
public:
  ReplaceFuncBase(uint32_t nset) : nset(nset) {};
  virtual uint32_t replace(uint32_t s, uint32_t *w) = 0; // return the number of free places
  virtual void access(uint32_t s, uint32_t w) = 0;
  virtual void invalid(uint32_t s, uint32_t w) = 0;
  virtual void withdraw_replace(uint32_t s, uint32_t w) {}
  virtual ~ReplaceFuncBase() {}
};

/////////////////////////////////
// FIFO replacement
// IW: index width, NW: number of ways
template<int IW, int NW, bool EF = true>
class ReplaceFIFO : public ReplaceFuncBase
{
protected:
  std::vector<std::list<uint32_t> > used_map;
  std::vector<std::unordered_map<uint32_t, bool> > using_map; // Write in the way that the thread needs to use during runtime
  std::vector<std::unordered_set<uint32_t> > free_map; 

public:
  ReplaceFIFO() : ReplaceFuncBase(1ul<<IW), used_map(1ul<<IW), using_map(1ul<<IW), free_map(1ul << IW) {
    for (auto &s : free_map) for(uint32_t i=0; i<NW; i++) s.insert(i);
  }
  virtual ~ReplaceFIFO() {}
  virtual void withdraw_replace(uint32_t s, uint32_t w) {
    std::unique_lock lk(mtx);
    assert(using_map[s].count(w));
    if(using_map[s][w]){
      used_map[s].push_front(w);
    }else{
      free_map[s].insert(w);
    }
    using_map[s].erase(w);
    lk.unlock();
  }

  virtual uint32_t replace(uint32_t s, uint32_t *w){
    std::unique_lock lk(mtx);
    if(!free_map[s].empty()){
      *w = *(free_map[s].begin());
      free_map[s].erase(*w);
      using_map[s][*w] = false;
    }
    else{
      assert(used_map[s].size() >= 1);
      *w = used_map[s].front();
      used_map[s].remove(*w);
      using_map[s][*w] = true;
    }
    lk.unlock();
    return free_map[s].size();
  }
  virtual void access(uint32_t s, uint32_t w) {
    std::unique_lock lk(mtx);
    assert(!free_map[s].count(w)); 
    assert(using_map[s].count(w));
    if(using_map[s].count(w)){
      using_map[s].erase(w);
      used_map[s].push_back(w);
    }
    lk.unlock();
  }
  virtual void invalid(uint32_t s, uint32_t w){
  }
};

/////////////////////////////////
// LRU replacement
// IW: index width, NW: number of ways
template<int IW, int NW, bool EF = true>
class ReplaceLRU : public ReplaceFIFO<IW, NW>
{
public:
  using ReplaceFIFO<IW,NW>::free_map;
  using ReplaceFIFO<IW,NW>::used_map;
  using ReplaceFIFO<IW,NW>::using_map;
  using ReplaceFIFO<IW,NW>::mtx;
  
  ReplaceLRU() : ReplaceFIFO<IW,NW,EF>() {}
  ~ReplaceLRU() {}

  virtual void access(uint32_t s, uint32_t w) {
    std::unique_lock lk(mtx);
    assert(!free_map[s].count(w)); 
    if(using_map[s].count(w)){
      using_map[s].erase(w);
      used_map[s].push_back(w);
    }else{
      assert(std::find(used_map[s].begin(), used_map[s].end(), w) != used_map[s].end());
      used_map[s].remove(w);
      used_map[s].push_back(w);
    }
    lk.unlock();
  }
};

/////////////////////////////////
// Random replacement
// IW: index width, NW: number of ways
template<int IW, int NW>
class ReplaceRandom : public ReplaceFuncBase
{
protected:
  std::unordered_map<uint32_t, std::unordered_set<uint32_t> > free_map;
public:
  ReplaceRandom() : ReplaceFuncBase(1ul<<IW) {}
  virtual ~ReplaceRandom() {}

  virtual uint32_t replace(uint32_t s, uint32_t *w){
    if(!free_map.count(s))
      for(uint32_t i=0; i<NW; i++) free_map[s].insert(i);
    if(free_map[s].size() > 0)
      *w = *(free_map[s].begin());
    else
      *w = cm_get_random_uint32() % NW;
    return free_map[s].size();
  }

  virtual void access(uint32_t s, uint32_t w){
    if(free_map[s].count(w))
      free_map[s].erase(w);
  }

  virtual void invalid(uint32_t s, uint32_t w) {
    free_map[s].insert(w);
  }
};

/////////////////////////////////
// Mirage Random replacement
// IW: index width, NW: number of ways
template<int IW, int NW>
class ReplaceCompleteRandom : public ReplaceFuncBase
{
  std::vector<uint8_t> free_blocks;
public:
  ReplaceCompleteRandom() : ReplaceFuncBase(1ul<<IW), free_blocks(this->nset,NW) {}
  virtual ~ReplaceCompleteRandom() {}

  virtual uint32_t replace(uint32_t s, uint32_t *w){
    *w = cm_get_random_uint32() % NW;
    return 0;
  }

  virtual void access(uint32_t s, uint32_t w) { free_blocks[s]--; assert(free_blocks[s]>=0); }
  virtual void invalid(uint32_t s, uint32_t w) { free_blocks[s]++; assert(free_blocks[s]<=NW); }
};
#endif
