#ifndef CM_REPLACE_HPP_
#define CM_REPLACE_HPP_

#include <vector>
#include <cassert>
#include <mutex>
#include "util/random.hpp"

///////////////////////////////////
// Base class
// EF: empty first
template<bool EF>
class ReplaceFuncBase
{
  const uint32_t NW;
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

  virtual uint32_t select(uint32_t s) = 0;

public:
  ReplaceFuncBase(uint32_t nset, uint32_t nway)
    :NW(nway), used_map(nset), free_map(nset), alloc_map(nset), free_num(nset, nway) {
    for (auto &s: free_map) s.resize(NW, true);
    for (auto &s: alloc_map) s.resize(NW, false);
  }
  virtual ~ReplaceFuncBase() {}

  uint32_t get_free_num(uint32_t s) const { return free_num[s]; }

  virtual void replace(uint32_t s, uint32_t *w, uint32_t op = 0) {
    uint32_t i = 0;
    if constexpr (EF) {
      if(free_num[s] > 0) i = alloc_from_free(s);
      else                i = select(s);
    } else {
      i = select(s);
      if(free_map[s][i]) { free_num[s]--; free_map[s][i] = false; }
    }
    assert(i < NW || 0 == "replacer used_map corrupted!");
    alloc_map[s][i] = true;
    *w = i;
  }

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) = 0;

  virtual void invalid(uint32_t s, uint32_t w){
    if(!alloc_map[s][w]) {
      free_map[s][w] = true;
      free_num[s]++;
    }
  }
};

///////////////////////////////////
// Base Replacer class supporting multi-thread (MT)
// EF: empty first
template<bool EF>
class ReplaceFuncBaseMT : public ReplaceFuncBase<EF>
{
protected:
  std::vector<std::mutex *> mtxs;

  __always_inline void lock(uint32_t s)   { mtxs[s]->lock();   }
  __always_inline void unlock(uint32_t s) { mtxs[s]->unlock(); }

public:
  ReplaceFuncBaseMT(uint32_t nset, uint32_t nway)
    : ReplaceFuncBase<EF>(nset, nway), mtxs(nset, nullptr) {
    for(auto &m: mtxs) m = new std::mutex();
  }

  virtual ~ReplaceFuncBaseMT() { for(auto m : mtxs) delete m; }

  virtual void replace(uint32_t s, uint32_t *w, uint32_t op = 0) {
    lock(s);
    ReplaceFuncBase<EF>::replace(s, w, op);
    unlock(s);
  }

  virtual void invalid(uint32_t s, uint32_t w){
    lock(s);
    ReplaceFuncBase<EF>::invalid(s, w);
    unlock(s);
  }
};

/////////////////////////////////
// FIFO replacement
// RPT: base class, IW: index width, NW: number of ways, EF: empty first, DUO: demand update only (do not update state for release)
template<template <bool> class RPT, int IW, int NW, bool EF, bool DUO>
class ReplaceFIFO_G : public RPT<EF>
{
protected:
  using RPT<EF>::alloc_map;
  using RPT<EF>::used_map;

  virtual uint32_t select(uint32_t s) {
    for(uint32_t i=0; i<NW; i++)
      if(used_map[s][i] == 0)
        return i;
    assert(0 == "replacer used_map corrupted!");
    return -1;
  }

public:
  ReplaceFIFO_G() : RPT<EF>(1ul << IW, NW) {
    for (auto &s: used_map) {
      s.resize(NW);
      for(uint32_t i=0; i<NW; i++) s[i] = i;
    }
  }
  virtual ~ReplaceFIFO_G() {}

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) {
    if constexpr (C_DERIVE<RPT<EF>, ReplaceFuncBaseMT<EF>>) RPT<EF>::lock(s);
    if(alloc_map[s][w] && !release) {
      alloc_map[s][w] = false;
      auto prio = used_map[s][w];
      for(uint32_t i=0; i<NW; i++) if(used_map[s][i] > prio) used_map[s][i]--;
      used_map[s][w] = NW-1;
    }
    if constexpr (C_DERIVE<RPT<EF>, ReplaceFuncBaseMT<EF>>) RPT<EF>::unlock(s);
  }
};

template<int IW, int NW, bool EF = true, bool DUO = true>
using ReplaceFIFO = ReplaceFIFO_G<ReplaceFuncBase, IW, NW, EF, DUO>;

template<int IW, int NW, bool EF = true, bool DUO = true>
using ReplaceFIFOMultiThread = ReplaceFIFO_G<ReplaceFuncBaseMT, IW, NW, EF, DUO>;

/////////////////////////////////
// LRU replacement
// RPT: base class, IW: index width, NW: number of ways, EF: empty first
template<template <bool> class RPT, int IW, int NW, bool EF, bool DUO>
class ReplaceLRU_G : public ReplaceFIFO_G<RPT, IW, NW, EF, DUO>
{
protected:
  using RPT<EF>::alloc_map;
  using RPT<EF>::used_map;

public:
  ReplaceLRU_G() : ReplaceFIFO_G<RPT, IW, NW, EF, DUO>() {}
  ~ReplaceLRU_G() {}

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) {
    if constexpr (C_DERIVE<RPT<EF>, ReplaceFuncBaseMT<EF>>) RPT<EF>::lock(s);
    if(alloc_map[s][w] || !DUO || !release) {
      auto prio = used_map[s][w];
      for(uint32_t i=0; i<NW; i++) if(used_map[s][i] > prio) used_map[s][i]--;
      used_map[s][w] = NW-1;
    }
    if(alloc_map[s][w] && !release) alloc_map[s][w] = false;
    if constexpr (C_DERIVE<RPT<EF>, ReplaceFuncBaseMT<EF>>) RPT<EF>::unlock(s);
  }
};

template<int IW, int NW, bool EF = true, bool DUO = true>
using ReplaceLRU = ReplaceLRU_G<ReplaceFuncBase, IW, NW, EF, DUO>;

template<int IW, int NW, bool EF = true, bool DUO = true>
using ReplaceLRUMultiThread = ReplaceLRU_G<ReplaceFuncBaseMT, IW, NW, EF, DUO>;

/////////////////////////////////
// Static RRIP replacement
// RPT: base class, IW: index width, NW: number of ways, EF: empty first
template<template <bool> class RPT, int IW, int NW, bool EF, bool DUO>
class ReplaceSRRIP_G : public RPT<EF>
{
protected:
  using RPT<EF>::used_map;
  using RPT<EF>::alloc_map;

  virtual uint32_t select(uint32_t s) {
    uint32_t max_prio = used_map[s][0];
    uint32_t max_i    = 0;
    for(uint32_t i=1; i<NW; i++) if(used_map[s][i] > max_prio) {max_prio = used_map[s][i]; max_i = i;}
    uint32_t gap = 3 - max_prio;
    if(gap > 0) for(uint32_t i=0; i<NW; i++) used_map[s][i] += gap;
    return max_i;
  }

public:
  ReplaceSRRIP_G() : RPT<EF>(1ul << IW, NW) {
    for (auto &s: used_map) s.resize(NW, 3);
  }
  virtual ~ReplaceSRRIP_G() {}

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0){
    if constexpr (C_DERIVE<RPT<EF>, ReplaceFuncBaseMT<EF>>) RPT<EF>::lock(s);
    if(alloc_map[s][w] || !DUO || !release)
      used_map[s][w] = (alloc_map[s][w]) ? 2 : 0;
    if(alloc_map[s][w] && !release) alloc_map[s][w] = false;
    if constexpr (C_DERIVE<RPT<EF>, ReplaceFuncBaseMT<EF>>) RPT<EF>::unlock(s);
  }

  virtual void invalid(uint32_t s, uint32_t w){
    if constexpr (C_DERIVE<RPT<EF>, ReplaceFuncBaseMT<EF>>) RPT<EF>::lock(s);
    used_map[s][w] = 3;
    ReplaceFuncBase<EF>::invalid(s, w);
    if constexpr (C_DERIVE<RPT<EF>, ReplaceFuncBaseMT<EF>>) RPT<EF>::unlock(s);
  }
};

template<int IW, int NW, bool EF = true, bool DUO = true>
using ReplaceSRRIP = ReplaceSRRIP_G<ReplaceFuncBase, IW, NW, EF, DUO>;

template<int IW, int NW, bool EF = true, bool DUO = true>
using ReplaceSRRIPMultiThread = ReplaceSRRIP_G<ReplaceFuncBaseMT, IW, NW, EF, DUO>;

/////////////////////////////////
// Random replacement
// RPT: base class, IW: index width, NW: number of ways, EF: empty first
template<template <bool> class RPT, int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceRandom_G  : public RPT<EF>
{
protected:
  using RPT<EF>::alloc_map;

  RandomGen<uint32_t> * loc_random; // a local randomizer for better thread parallelism

  virtual uint32_t select(uint32_t s) {
    return (*loc_random)() % NW;
  }

public:
  ReplaceRandom_G() : RPT<EF>(1ul << IW, NW), loc_random(cm_alloc_rand32()) {}

  virtual ~ReplaceRandom_G() {
    delete loc_random;
  }

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0){
    if constexpr (C_DERIVE<RPT<EF>, ReplaceFuncBaseMT<EF>>) RPT<EF>::lock(s);
    if(alloc_map[s][w] && !release) alloc_map[s][w] = false;
    if constexpr (C_DERIVE<RPT<EF>, ReplaceFuncBaseMT<EF>>) RPT<EF>::unlock(s);
  }
};

template<int IW, int NW, bool EF = true, bool DUO = true>
using ReplaceRandom = ReplaceRandom_G<ReplaceFuncBase, IW, NW, EF, DUO>;

template<int IW, int NW, bool EF = true, bool DUO = true>
using ReplaceRandomMultiThread = ReplaceRandom_G<ReplaceFuncBaseMT, IW, NW, EF, DUO>;

#endif
