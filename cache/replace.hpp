#ifndef CM_REPLACE_HPP_
#define CM_REPLACE_HPP_

#include <list>
#include <vector>
#include <unordered_set>
#include <cassert>
#include "util/random.hpp"

///////////////////////////////////
// Base class

class ReplaceFuncBase
{
protected:
  const uint32_t nset;
public:
  ReplaceFuncBase(uint32_t nset) : nset(nset) {};
  virtual uint32_t replace(uint32_t s, uint32_t *w) = 0; // return the number of free places
  virtual void access(uint32_t s, uint32_t w) = 0;
  virtual void invalid(uint32_t s, uint32_t w) = 0;
  virtual ~ReplaceFuncBase() {}
};

/////////////////////////////////
// FIFO replacement
// IW: index width, NW: number of ways, EF: empty first
template<int IW, int NW, bool EF = true>
class ReplaceFIFO : public ReplaceFuncBase
{
protected:
  std::vector<std::list<uint32_t> > used_map;
  std::vector<std::unordered_set<uint32_t> > free_map;

public:
  ReplaceFIFO() : ReplaceFuncBase(1ul<<IW), used_map(1ul<<IW), free_map(1ul << IW) {
    for (auto &s: free_map) for(uint32_t i=0; i<NW; i++) s.insert(i);
  }
  virtual ~ReplaceFIFO() {}
  virtual uint32_t replace(uint32_t s, uint32_t *w){
    if constexpr (EF)
      *w = free_map[s].empty() ? used_map[s].front() : *(free_map[s].begin());
    else
      *w = used_map[s].front();

    return free_map[s].size();
  }

  virtual void access(uint32_t s, uint32_t w) {
    if constexpr (EF) {
      if(free_map[s].count(w)) {
        free_map[s].erase(w);
        used_map[s].push_back(w);
      }
    } else {
      if(free_map[s].count(w)) free_map[s].erase(w);
      if(w == used_map[s].front()) {
        used_map[s].pop_front();
        used_map[s].push_back(w);
      }
    }
  }
  virtual void invalid(uint32_t s, uint32_t w){
    if constexpr (EF) used_map[s].remove(w);
    free_map[s].insert(w);
  }
};

/////////////////////////////////
// LRU replacement
// IW: index width, NW: number of ways, EF: empty first
template<int IW, int NW, bool EF = true>
class ReplaceLRU : public ReplaceFIFO<IW, NW, EF>
{
protected:
  using ReplaceFIFO<IW,NW,EF>::free_map;
  using ReplaceFIFO<IW,NW,EF>::used_map;

public:
  ReplaceLRU() : ReplaceFIFO<IW,NW,EF>() {}
  ~ReplaceLRU() {}

  virtual void access(uint32_t s, uint32_t w) {
    if constexpr (EF) {
      if(free_map[s].count(w)) {
        free_map[s].erase(w);
        used_map[s].push_back(w);
      } else{
        used_map[s].remove(w);
        used_map[s].push_back(w);
      }
    } else {
      if(free_map[s].count(w)) free_map[s].erase(w);
      used_map[s].remove(w);
      used_map[s].push_back(w);
    }
  }
};


/////////////////////////////////
// Random replacement
// IW: index width, NW: number of ways, EF: empty first
template<int IW, int NW, bool EF = true>
class ReplaceRandom : public ReplaceFuncBase
{
protected:
  std::vector<std::unordered_set<uint32_t> > free_map;
public:
  ReplaceRandom() : ReplaceFuncBase(1ul<<IW), free_map(1ul<<IW) {
    for (auto &s: free_map) for(uint32_t i=0; i<NW; i++) s.insert(i);
  }
  virtual ~ReplaceRandom() {}

  virtual uint32_t replace(uint32_t s, uint32_t *w){
    if constexpr (EF)
      *w = free_map[s].empty() ? (cm_get_random_uint32() % NW) : *(free_map[s].begin());
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

#endif
