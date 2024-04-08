#ifndef CM_REPLACE_HPP_
#define CM_REPLACE_HPP_

// #define SET

#include <list>
#include <vector>
#include <set>
#include <algorithm>
#include <iterator>
#include <unordered_set>
#include <cassert>
#include "util/random.hpp"

///////////////////////////////////
// Base class

class ReplaceFuncBase
{
protected:
  const uint32_t nset;
  std::vector<std::mutex*> mtxs;
public:
  ReplaceFuncBase(uint32_t nset) : nset(nset) { for (uint32_t i=0; i<nset; i++) mtxs.emplace_back(new std::mutex());};
  virtual uint32_t replace(uint32_t s, uint32_t *w, uint32_t op = 0) = 0; // return the number of free places
  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) = 0;
  virtual void invalid(uint32_t s, uint32_t w) = 0;
  virtual ~ReplaceFuncBase() {
    for(auto m : mtxs) delete m;
  }
};


#ifdef SET
/////////////////////////////////
// FIFO replacement
// IW: index width, NW: number of ways, EF: empty first, DUO: demand update only (do not update state for release)
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceFIFO : public ReplaceFuncBase
{
protected:
  std::vector<std::list<uint32_t> > used_map;
  std::vector<std::vector<bool> > using_map; // Write in the way that the thread needs to use during runtime
  std::vector<std::unordered_set<uint32_t> > free_map; 

public:
  ReplaceFIFO() : ReplaceFuncBase(1ul<<IW), used_map(1ul<<IW), using_map(1ul<<IW), free_map(1ul << IW) {
    for (auto &s: free_map) for(uint32_t i=0; i<NW; i++) s.insert(i);
    for (auto &s:using_map) for(uint32_t i=0; i<NW; i++) s.emplace_back(false);
    if constexpr (!EF)
      for (auto &s: used_map) for(uint32_t i=0; i<NW; i++) s.push_back(i);
  }
  virtual ~ReplaceFIFO() {}
  virtual uint32_t replace(uint32_t s, uint32_t *w, uint32_t op = 0){
    if constexpr (EF){
      std::unique_lock lk(*mtxs[s]);
      if(!free_map[s].empty()){
        *w = *(free_map[s].begin());
        free_map[s].erase(*w);
        using_map[s][*w] = false;
      }
      else{
#ifndef NDEBUG 
        assert(used_map[s].size() >= 1);
#endif
        *w = used_map[s].front();
        used_map[s].remove(*w);
        using_map[s][*w] = true;
      }
      lk.unlock();
    }
    else
      *w = used_map[s].front();
    return free_map[s].size();
  }

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) {
    std::unique_lock lk(*mtxs[s]);
#ifndef NDEBUG 
    assert(!free_map[s].count(w)); 
#endif
    if(using_map[s][w]){
      using_map[s][w] = false;
      used_map[s].push_back(w);
    }
    lk.unlock();
  }
  virtual void invalid(uint32_t s, uint32_t w){
    // if constexpr (EF) used_map[s].remove(w);
    // free_map[s].insert(w);
    std::unique_lock lk(*mtxs[s]);
    if(!using_map[s][w]){
      used_map[s].remove(w);
      free_map[s].insert(w);
    }
    lk.unlock();
  }
};

/////////////////////////////////
// LRU replacement
// IW: index width, NW: number of ways, EF: empty first
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceLRU : public ReplaceFIFO<IW, NW, EF>
{
protected:
  using ReplaceFIFO<IW,NW>::free_map;
  using ReplaceFIFO<IW,NW>::used_map;
  using ReplaceFIFO<IW,NW>::using_map;
  using ReplaceFIFO<IW,NW>::mtxs;

public:
  ReplaceLRU() : ReplaceFIFO<IW,NW,EF>() {}
  ~ReplaceLRU() {}

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) {
    if constexpr (EF) {
      std::unique_lock lk(*mtxs[s]);
#ifndef NDEBUG 
      assert(!free_map[s].count(w)); 
#endif
      if(using_map[s][w]){
        using_map[s][w] = false;
        used_map[s].push_back(w);
      }else{
#ifndef NDEBUG 
        assert(std::find(used_map[s].begin(), used_map[s].end(), w) != used_map[s].end());
#endif
        used_map[s].remove(w);
        used_map[s].push_back(w);
      }
      lk.unlock();
    } else {
      if(free_map[s].count(w)) free_map[s].erase(w);
      if(!DUO || !release) {
        used_map[s].remove(w);
        used_map[s].push_back(w);
      }
    }
  }
};
#else
/////////////////////////////////
// FIFO replacement
// IW: index width, NW: number of ways, EF: empty first, DUO: demand update only (do not update state for release)
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceFIFO : public ReplaceFuncBase
{
protected:
  std::vector<std::vector<bool> > using_map; // Write in the way that the thread needs to use during runtime
  std::vector<std::vector<uint32_t> > count_map; 
public:
  ReplaceFIFO() : ReplaceFuncBase(1ul<<IW), using_map(1ul<<IW), count_map(1ul << IW){
    for (auto &s: count_map) for(uint32_t i=0; i<NW; i++) s.emplace_back(0);
    for (auto &s:using_map) for(uint32_t i=0; i<NW; i++) s.emplace_back(false);
  }
  virtual ~ReplaceFIFO() {}
  virtual uint32_t replace(uint32_t s, uint32_t *w, uint32_t op = 0){
    std::unique_lock lk(*mtxs[s]);
    uint32_t min_index = NW;
    int max_count = -1;
    for(int i = 0; i < NW; i++){
      if(!using_map[s][i]){
        if(count_map[s][i] == 0){
          *w = i;
          break;
        }else if((int)count_map[s][i] > max_count){
          *w = i;
          max_count = count_map[s][i];
        }
      }
    }
    using_map[s][*w] = true;
    lk.unlock();
    return count_map[s].size();
  }

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) {
    std::unique_lock lk(*mtxs[s]);
    if(using_map[s][w]){
      using_map[s][w] = false;
      count_map[s][w] = 1;
      for(int i = 0; i < NW; i++){
        if(count_map[s][i] != 0 && i != w && using_map[s][i] == false) count_map[s][i] += 1;
      }
    }
    lk.unlock();
  }
  virtual void invalid(uint32_t s, uint32_t w){
    // if constexpr (EF) used_map[s].remove(w);
    // free_map[s].insert(w);
    std::unique_lock lk(*mtxs[s]);
    if(!using_map[s][w]){
      count_map[s][w] = 0;
    }
    lk.unlock();
  }
};

/////////////////////////////////
// LRU replacement
// IW: index width, NW: number of ways, EF: empty first
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceLRU : public ReplaceFIFO<IW, NW, EF>
{
protected:
  using ReplaceFIFO<IW,NW>::count_map;
  using ReplaceFIFO<IW,NW>::using_map;
  using ReplaceFIFO<IW,NW>::mtxs;

public:
  ReplaceLRU() : ReplaceFIFO<IW,NW,EF>() {}
  ~ReplaceLRU() {}

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) {
    std::unique_lock lk(*mtxs[s]);
    if(using_map[s][w]) using_map[s][w] = false;
    count_map[s][w] = 1;
    for(int i = 0; i < NW; i++){
      if(count_map[s][i] != 0 && i != w && using_map[s][i] == false) count_map[s][i] += 1;
    }
    lk.unlock();
  }
};

#endif


/////////////////////////////////
// Static RRIP replacement
// IW: index width, NW: number of ways, EF: empty first
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceSRRIP : public ReplaceFuncBase
{
protected:
  std::vector<std::vector<uint8_t> > used_map;
  std::vector<std::set<uint32_t> > free_map;

  uint32_t select(std::vector<uint8_t>& rrpv) {
    auto start = rrpv.begin();
    auto it = std::max_element(start, rrpv.end());
    uint8_t gap = 3 - *it;
    if(gap > 0) for(auto &w: rrpv) w += gap;
    return std::distance(start, it);
  }

public:
  ReplaceSRRIP() : ReplaceFuncBase(1ul<<IW), used_map(1ul<<IW, std::vector<uint8_t>(NW, 3)), free_map(1ul << IW) {
    for (auto &s: free_map) for(uint32_t i=0; i<NW; i++) s.insert(i);
  }
  virtual ~ReplaceSRRIP() {}

  virtual uint32_t replace(uint32_t s, uint32_t *w, uint32_t op = 0) {
    if constexpr (EF)
      *w = free_map[s].empty() ? select(used_map[s]) : *(free_map[s].cbegin());
    else
      *w = select(used_map[s]);
    return free_map[s].size();
  }

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0){
    if(free_map[s].count(w)) {
      free_map[s].erase(w);
      used_map[s][w] = 2;
    } else if(!DUO || !release)
      used_map[s][w] = 0;
  }

  virtual void invalid(uint32_t s, uint32_t w){
    free_map[s].insert(w);
  }
};


/////////////////////////////////
// Random replacement
// IW: index width, NW: number of ways, EF: empty first
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceRandom : public ReplaceFuncBase
{
protected:
  std::vector<std::set<uint32_t> > free_map;
public:
  ReplaceRandom() : ReplaceFuncBase(1ul<<IW), free_map(1ul<<IW) {
    for (auto &s: free_map) for(uint32_t i=0; i<NW; i++) s.insert(i);
  }
  virtual ~ReplaceRandom() {}

  virtual uint32_t replace(uint32_t s, uint32_t *w, uint32_t op = 0){
    if constexpr (EF)
      *w = free_map[s].empty() ? (cm_get_random_uint32() % NW) : *(free_map[s].cbegin());
    else
      *w = cm_get_random_uint32() % NW;

    return free_map[s].size();
  }

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0){
    if(free_map[s].count(w))
      free_map[s].erase(w);
  }

  virtual void invalid(uint32_t s, uint32_t w) {
    free_map[s].insert(w);
  }
};

#endif
