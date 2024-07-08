#ifndef CM_REPLACE_HPP_
#define CM_REPLACE_HPP_

#include <vector>
#include <cassert>
#include <mutex>
#include "util/random.hpp"

///////////////////////////////////
// Base class
// EF: empty first, EnMT: enable multithread support
template<bool EF, bool EnMT>
class ReplaceFuncBase
{
  const uint32_t NW;
protected:
  std::vector<std::vector<uint32_t> > used_map; // at the size of 16, vector is actually faster than list and do not require alloc
  std::vector<std::vector<bool> > free_map, alloc_map;
  std::vector<uint32_t> free_num, alloc_num;
  std::vector<std::mutex *> mtxs;

  __always_inline void lock(uint32_t s)   { mtxs[s]->lock();   }
  __always_inline void unlock(uint32_t s) { mtxs[s]->unlock(); }

  __always_inline uint32_t alloc_from_free(uint32_t s) {
    free_num[s]--;
    for(uint32_t i=0; i<NW; i++)
      if(free_map[s][i]) {
        free_map[s][i] = false;
        return i;
      }

    assert(0 == "replacer free_map corrupted!");
    return -1;
  }

  virtual uint32_t select(uint32_t s) = 0;

public:
  ReplaceFuncBase(uint32_t nset, uint32_t nway)
    :NW(nway), used_map(nset), free_map(nset), alloc_map(nset), free_num(nset, nway), alloc_num(nset, 0) {
    for (auto &s: free_map) s.resize(NW, true);
    for (auto &s: alloc_map) s.resize(NW, false);
    if constexpr (EnMT) {
      mtxs.resize(nset, nullptr);
      for(auto &m: mtxs) m = new std::mutex();
    }
  }
  virtual ~ReplaceFuncBase() {
    if constexpr (EnMT) {
      for(auto m : mtxs) delete m;
    }
  }

  __always_inline uint32_t get_free_num(uint32_t s) const { return free_num[s]; }

  virtual void replace(uint32_t s, uint32_t *w, uint32_t op = 0) {
    uint32_t i = 0;
    if constexpr (EnMT) lock(s);
    if constexpr (EF) {
      if(free_num[s] > 0) i = alloc_from_free(s);
      else                i = select(s);
    } else {
      i = select(s);
      if(free_map[s][i]) { free_num[s]--; free_map[s][i] = false; }
    }
    assert(i < NW || 0 == "replacer used_map corrupted!");
    alloc_map[s][i] = true; alloc_num[s]++;
    *w = i;
    if constexpr (EnMT) unlock(s);
  }

  // in multithread env, a replace selection might be deserted.
  // The selected element should be placed as the most likely chosen position.
  virtual void restore(uint32_t s, uint32_t w, uint32_t op = 0) {
    if constexpr (EnMT) lock(s);
    if constexpr (EF) {
      free_num[s]++; free_map[s][w] = true; // put it to free map (likely selected next time)
    }
    alloc_map[s][w] = false; alloc_num[s]--;
    if constexpr (EnMT) unlock(s);
  }

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) = 0;

  virtual void invalid(uint32_t s, uint32_t w) {
    if constexpr (EnMT) lock(s);
    if(!alloc_map[s][w]) {
      free_map[s][w] = true;
      free_num[s]++;
    }
    if constexpr (EnMT) unlock(s);
  }
};

/////////////////////////////////
// FIFO replacement
// IW: index width, NW: number of ways
// EF: empty first, DUO: demand update only (do not update state for release)
// EnMT: enable multithread support
template<int IW, int NW, bool EF = true, bool DUO = true, bool EnMT = false>
class ReplaceFIFO : public ReplaceFuncBase<EF, EnMT>
{
  typedef ReplaceFuncBase<EF, EnMT> RPT;
protected:
  using RPT::alloc_map;
  using RPT::alloc_num;
  using RPT::used_map;

  virtual uint32_t select(uint32_t s) {
    for(uint32_t i=0; i<NW; i++)
      if(used_map[s][i] <= alloc_num[s] && !alloc_map[s][i])
        return i;
    assert(0 == "replacer used_map corrupted!");
    return -1;
  }

public:
  ReplaceFIFO() : RPT(1ul << IW, NW) {
    for (auto &s: used_map) {
      s.resize(NW);
      for(uint32_t i=0; i<NW; i++) s[i] = i;
    }
  }
  virtual ~ReplaceFIFO() {}

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) {
    if constexpr (EnMT) RPT::lock(s);
    if(alloc_map[s][w] && !release) {
      alloc_map[s][w] = false; alloc_num[s]--;
      auto prio = used_map[s][w];
      for(uint32_t i=0; i<NW; i++) if(used_map[s][i] > prio) used_map[s][i]--;
      used_map[s][w] = NW-1;
    }
    if constexpr (EnMT) RPT::unlock(s);
  }
};

template<int IW, int NW, bool EF = true, bool DUO = true>
using ReplaceFIFO_MT = ReplaceFIFO<IW, NW, EF, DUO, true>;

/////////////////////////////////
// LRU replacement
// IW: index width, NW: number of ways
// EF: empty first, DUO: demand update only (do not update state for release)
// EnMT: enable multithread support
template<int IW, int NW, bool EF = true, bool DUO = true, bool EnMT = false>
class ReplaceLRU : public ReplaceFIFO<IW, NW, EF, DUO, EnMT>
{
  typedef ReplaceFuncBase<EF, EnMT> RPT;
protected:
  using RPT::alloc_map;
  using RPT::alloc_num;
  using RPT::used_map;

public:
  ReplaceLRU() : ReplaceFIFO<IW, NW, EF, DUO, EnMT>() {}
  virtual ~ReplaceLRU() {}

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0) {
    if constexpr (EnMT) RPT::lock(s);
    if(alloc_map[s][w] || !DUO || !release) {
      auto prio = used_map[s][w];
      for(uint32_t i=0; i<NW; i++) if(used_map[s][i] > prio) used_map[s][i]--;
      used_map[s][w] = NW-1;
    }
    if(alloc_map[s][w] && !release) { alloc_map[s][w] = false; alloc_num[s]--; }
    if constexpr (EnMT) RPT::unlock(s);
  }
};

template<int IW, int NW, bool EF = true, bool DUO = true>
using ReplaceLRU_MT = ReplaceLRU<IW, NW, EF, DUO, true>;

/////////////////////////////////
// Static RRIP replacement
// IW: index width, NW: number of ways
// EF: empty first, DUO: demand update only (do not update state for release)
// EnMT: enable multithread support
template<int IW, int NW, bool EF = true, bool DUO = true, bool EnMT = false>
class ReplaceSRRIP : public ReplaceFuncBase<EF, EnMT>
{
  typedef ReplaceFuncBase<EF, EnMT> RPT;
protected:
  using RPT::used_map;
  using RPT::alloc_map;
  using RPT::alloc_num;

  virtual uint32_t select(uint32_t s) {
    uint32_t max_prio = used_map[s][0];
    uint32_t max_i    = 0;
    for(uint32_t i=1; i<NW; i++) if(used_map[s][i] > max_prio && !alloc_map[s][i]) {max_prio = used_map[s][i]; max_i = i;}
    uint32_t gap = 3 - max_prio;
    if(gap > 0 && alloc_num[s] == 0) for(uint32_t i=0; i<NW; i++) used_map[s][i] += gap;
    return max_i;
  }

public:
  ReplaceSRRIP() : RPT(1ul << IW, NW) {
    for (auto &s: used_map) s.resize(NW, 3);
  }
  virtual ~ReplaceSRRIP() {}

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0){
    if constexpr (EnMT) RPT::lock(s);
    if(alloc_map[s][w] || !DUO || !release)
      used_map[s][w] = (alloc_map[s][w]) ? 2 : 0;
    if(alloc_map[s][w] && !release) { alloc_map[s][w] = false; alloc_num[s]--; }
    if constexpr (EnMT) RPT::unlock(s);
  }

  virtual void invalid(uint32_t s, uint32_t w){
    if constexpr (EnMT) RPT::lock(s);
    used_map[s][w] = 3;
    RPT::invalid(s, w);
    if constexpr (EnMT) RPT::unlock(s);
  }
};

template<int IW, int NW, bool EF = true, bool DUO = true>
using ReplaceSRRIP_MT = ReplaceSRRIP<IW, NW, EF, DUO, true>;

/////////////////////////////////
// Random replacement
// IW: index width, NW: number of ways
// EF: empty first, DUO: demand update only (do not update state for release)
// EnMT: enable multithread support
template<int IW, int NW, bool EF = true, bool DUO = true, bool EnMT = false>
class ReplaceRandom : public ReplaceFuncBase<EF, EnMT>
{
  typedef ReplaceFuncBase<EF, EnMT> RPT;
protected:
  using RPT::alloc_map;
  using RPT::alloc_num;

  RandomGen<uint32_t> * loc_random; // a local randomizer for better thread parallelism

  virtual uint32_t select(uint32_t s) {
    assert(alloc_num[s] < NW || 0 ==
           "Too many ways have been allocated without actual accesses!");
    while(true) {
      auto w = (*loc_random)() % NW;
      if(!alloc_map[s][w]) return w;
    }
  }

public:
  ReplaceRandom() : RPT(1ul << IW, NW), loc_random(cm_alloc_rand32()) {}

  virtual ~ReplaceRandom() {
    delete loc_random;
  }

  virtual void access(uint32_t s, uint32_t w, bool release, uint32_t op = 0){
    if constexpr (EnMT) RPT::lock(s);
    if(alloc_map[s][w] && !release) { alloc_map[s][w] = false; alloc_num[s]--; }
    if constexpr (EnMT) RPT::unlock(s);
  }
};

template<int IW, int NW, bool EF = true, bool DUO = true>
using ReplaceRandom_MT = ReplaceRandom<IW, NW, EF, DUO, true>;

#endif
