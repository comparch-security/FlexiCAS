#ifndef CM_REPLACE_HPP_
#define CM_REPLACE_HPP_

#include <vector>
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
  virtual uint32_t get_free_num(uint32_t s) const = 0; // return the number of free places
  virtual void replace(uint32_t s, uint32_t *w, uint32_t op = 0) = 0;
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
  std::vector<std::vector<uint32_t> > used_map; // at the size of 16, vector is actually faster than list and do not require alloc
  std::vector<std::vector<bool> > free_map, alloc_map;
  std::vector<uint32_t> free_num;

  uint32_t alloc_from_free(uint32_t s) {
    free_num[s]--;
    for(uint32_t i=0; i<NW; i++)
      if(free_map[s][i]) {
        free_map[s][i] = false;
        alloc_map[s][i] = true;
        return i;
      }

    assert(0 == "replacer free_map corrupted!");
    return -1;
  }

public:
  ReplaceFIFO() : ReplaceFuncBase(1ul<<IW), used_map(1ul<<IW), free_map(1ul << IW), alloc_map(1ul << IW), free_num(1ul << IW, NW) {
    for (auto &s: used_map) {
      s.resize(NW);
      for(uint32_t i=0; i<NW; i++) s[i] = i;
    }
    for (auto &s: free_map) s.resize(NW, true);
    for (auto &s: alloc_map) s.resize(NW, false);
  }
  virtual ~ReplaceFIFO() {}

  virtual uint32_t get_free_num(uint32_t s) const {return free_num[s]; }

  virtual void replace(uint32_t s, uint32_t *w, uint32_t op = 0){
    uint32_t i = 0;
    if constexpr (EF) {
      if(free_num[s] > 0) i = alloc_from_free(s);
      else
        for(; i<NW; i++) if(used_map[s][i] == 0) break;
    } else {
        for(; i<NW; i++) if(used_map[s][i] == 0) break;
        if(free_map[s][i]) { free_num[s]--; free_map[s][i] = false; }
    }
    assert(i < NW || 0 == "relacer used_map corrupted!");
    alloc_map[s][i] = true;
    *w = i;
  }

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) {
    if(alloc_map[s][w] && !release) {
      alloc_map[s][w] = false;
      auto prio = used_map[s][w];
      for(uint32_t i=0; i<NW; i++) if(used_map[s][i] > prio) used_map[s][i]--;
      used_map[s][w] = NW-1;
    }
  }

  virtual void invalid(uint32_t s, uint32_t w){
    //used_map[s][w] = 0;
    if(!alloc_map[s][w]) {
      free_map[s][w] = true;
      free_num[s]++;
    }
  }
};

/////////////////////////////////
// LRU replacement
// IW: index width, NW: number of ways, EF: empty first
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceLRU : public ReplaceFIFO<IW, NW, EF>
{
protected:
  using ReplaceFIFO<IW,NW,EF>::alloc_map;
  using ReplaceFIFO<IW,NW,EF>::used_map;

public:
  ReplaceLRU() : ReplaceFIFO<IW,NW,EF>() {}
  ~ReplaceLRU() {}

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) {
    if(alloc_map[s][w] || !DUO || !release) {
      auto prio = used_map[s][w];
      for(uint32_t i=0; i<NW; i++) if(used_map[s][i] > prio) used_map[s][i]--;
      used_map[s][w] = NW-1;
    }
    if(alloc_map[s][w] && !release) alloc_map[s][w] = false;
  }
};


/////////////////////////////////
// Static RRIP replacement
// IW: index width, NW: number of ways, EF: empty first
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceSRRIP : public ReplaceFIFO<IW, NW, EF>
{
protected:
  using ReplaceFIFO<IW,NW,EF>::alloc_from_free;
  using ReplaceFIFO<IW,NW,EF>::used_map;
  using ReplaceFIFO<IW,NW,EF>::alloc_map;
  using ReplaceFIFO<IW,NW,EF>::free_map;
  using ReplaceFIFO<IW,NW,EF>::free_num;

  uint32_t select(uint32_t s) {
    uint32_t max_prio = used_map[s][0];
    uint32_t max_i    = 0;
    for(uint32_t i=1; i<NW; i++) if(used_map[s][i] > max_prio) {max_prio = used_map[s][i]; max_i = i;}
    uint32_t gap = 3 - max_prio;
    if(gap > 0) for(uint32_t i=0; i<NW; i++) used_map[s][i] += gap;
    return max_i;
  }

public:
  ReplaceSRRIP() : ReplaceFIFO<IW,NW,EF>() {
    for (auto &s: used_map) for(uint32_t i=0; i<NW; i++) s[i] = 3;
  }
  virtual ~ReplaceSRRIP() {}

  virtual void replace(uint32_t s, uint32_t *w, uint32_t op = 0) {
    uint32_t i;
    if constexpr (EF) {
      if(free_num[s] > 0)
        i = alloc_from_free(s);
      else
        i = select(s);
    } else {
      i = select(s);
      if(free_map[s][i]) { free_num[s]--; free_map[s][i] = false; }
    }
    assert(i < NW || 0 == "relacer used_map corrupted!");
    alloc_map[s][i] = true;
    *w = i;
  }

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0){
    if(alloc_map[s][w] || !DUO || !release)
      used_map[s][w] = (alloc_map[s][w]) ? 2 : 0;
    if(alloc_map[s][w] && !release) alloc_map[s][w] = false;
  }

  virtual void invalid(uint32_t s, uint32_t w){
    used_map[s][w] = 3;
    if(!alloc_map[s][w]) {
      free_map[s][w] = true;
      free_num[s]++;
    }
  }
};


/////////////////////////////////
// Random replacement
// IW: index width, NW: number of ways, EF: empty first
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceRandom  : public ReplaceFIFO<IW, NW, EF>
{
protected:
  using ReplaceFIFO<IW,NW,EF>::alloc_from_free;
  using ReplaceFIFO<IW,NW,EF>::free_map;
  using ReplaceFIFO<IW,NW,EF>::free_num;
  using ReplaceFIFO<IW,NW,EF>::alloc_map;

  RandomGen<uint32_t> * loc_random; // a local randomizer for better thread parallelism

public:
  ReplaceRandom() : ReplaceFIFO<IW,NW,EF>(), loc_random(cm_alloc_rand32()) {}

  virtual ~ReplaceRandom() {
    delete loc_random;
  }

  virtual void replace(uint32_t s, uint32_t *w, uint32_t op = 0){
    uint32_t i;
    if constexpr (EF) {
      if(free_num[s] > 0)
        i = alloc_from_free(s);
      else
        i = (*loc_random)() % NW;
    } else {
      i = (*loc_random)() % NW;
      if(free_map[s][i]) { free_num[s]--; free_map[s][i] = false; }
    }
    assert(i < NW || 0 == "relacer used_map corrupted!");
    alloc_map[s][i] = true;
    *w = i;
  }

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0){
    if(alloc_map[s][w] && !release) alloc_map[s][w] = false;
  }

  virtual void invalid(uint32_t s, uint32_t w) {
    if(!alloc_map[s][w]) {
      free_map[s][w] = true;
      free_num[s]++;
    }
  }
};

#endif
