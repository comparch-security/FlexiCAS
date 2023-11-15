#ifndef CM_REPLACE_HPP_
#define CM_REPLACE_HPP_

#include <list>
#include <vector>
#include <set>
#include <algorithm>
#include <iterator>
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
  virtual uint32_t replace(uint32_t s, uint32_t *w, uint32_t op = 0) = 0; // return the number of free places
  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) = 0;
  virtual void invalid(uint32_t s, uint32_t w) = 0;
  virtual ~ReplaceFuncBase() {}
};

/////////////////////////////////
// FIFO replacement
// IW: index width, NW: number of ways, EF: empty first, DUO: demand update only (do not update state for release)
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceFIFO : public ReplaceFuncBase
{
protected:
  std::vector<std::list<uint32_t> > used_map;
  std::vector<std::set<uint32_t> > free_map;

public:
  ReplaceFIFO() : ReplaceFuncBase(1ul<<IW), used_map(1ul<<IW), free_map(1ul << IW) {
    for (auto &s: free_map) for(uint32_t i=0; i<NW; i++) s.insert(i);
    if constexpr (!EF)
      for (auto &s: used_map) for(uint32_t i=0; i<NW; i++) s.push_back(i);
  }
  virtual ~ReplaceFIFO() {}
  virtual uint32_t replace(uint32_t s, uint32_t *w, uint32_t op = 0){
    if constexpr (EF)
      *w = free_map[s].empty() ? used_map[s].front() : *(free_map[s].cbegin());
    else
      *w = used_map[s].front();

    return free_map[s].size();
  }

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) {
    if(free_map[s].count(w)) {
      free_map[s].erase(w);
      used_map[s].remove(w);
      used_map[s].push_back(w);
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
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceLRU : public ReplaceFIFO<IW, NW, EF>
{
protected:
  using ReplaceFIFO<IW,NW,EF>::free_map;
  using ReplaceFIFO<IW,NW,EF>::used_map;

public:
  ReplaceLRU() : ReplaceFIFO<IW,NW,EF>() {}
  ~ReplaceLRU() {}

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) {
    if constexpr (EF) {
      if(free_map[s].count(w)) {
        free_map[s].erase(w);
        used_map[s].push_back(w);
      } else if(!DUO || !release) {
        used_map[s].remove(w);
        used_map[s].push_back(w);
      }
    } else {
      if(free_map[s].count(w)) free_map[s].erase(w);
      if(!DUO || !release) {
        used_map[s].remove(w);
        used_map[s].push_back(w);
      }
    }
  }
};


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
