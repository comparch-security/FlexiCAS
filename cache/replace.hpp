#ifndef CM_REPLACE_HPP_
#define CM_REPLACE_HPP_

#include <vector>
#include <cassert>
#include <mutex>
#include "util/random.hpp"
#include "util/multithread.hpp"

#include <version>
#ifdef __cpp_lib_bitops
// for the popcount() supported in C++20
#include <bit>
#endif

///////////////////////////////////
// Base class
// EF: empty first, EnMT: multithread
template<bool EF, int NW, bool EnMT> requires (NW <= 64)
class ReplaceFuncBase
{
protected:
  std::vector<std::vector<uint32_t> > used_map; // at the size of 16, vector is actually faster than list and do not require alloc
  std::vector<uint64_t> free_map_st;            // free map when single thread
  std::vector<std::atomic<uint64_t> *> free_map_mt; // multi-thread version
  std::vector<int32_t> alloc_map; // record the way allocated for the next access (only one allocated ay at any time)

#ifdef CHECK_MULTI
  #ifdef BOOST_STACKTRACE_LINK
    std::vector<std::pair<uint64_t, std::string> > alloc_record;
  #else
    std::vector<uint64_t> alloc_record;
  #endif
#endif

  __always_inline int32_t alloc_from_free(uint32_t s) {
    while(true) {
      auto fmap = EnMT ? free_map_mt[s]->load() : free_map_st[s];
      if(fmap) {
        auto way_oh = fmap & (~fmap + 1ull);
        if constexpr (EnMT) {
          if(!free_map_mt[s]->compare_exchange_strong(fmap, fmap & ~way_oh)) continue;
        } else {
          free_map_st[s] &= ~way_oh;
        }
        for(int i=0; i<64; i++) if(way_oh == (1ull << i)) return i;
        assert(0 == "replacer free_map corrupted!");
        return -1;
      } else
        return -1;
    }
  }

  virtual uint32_t select(uint32_t s) = 0;

  __always_inline void delist_from_free(uint32_t s, uint32_t w) {
    uint64_t way_oh = 1ull << w;
    if constexpr (EnMT) {
      while(true) {
        auto fmap = free_map_mt[s]->load();
        if(0 == (fmap & way_oh)) return;
        if(free_map_mt[s]->compare_exchange_strong(fmap, fmap & ~way_oh)) return;
      }
    } else
      free_map_st[s] &= ~way_oh;
  }

  __always_inline void list_to_free(uint32_t s, uint32_t w) {
    uint64_t way_oh = 1ull << w;
    if constexpr (EnMT) {
      while(true) {
        auto fmap = free_map_mt[s]->load();
        if(free_map_mt[s]->compare_exchange_strong(fmap, fmap | way_oh)) return;
      }
    } else
      free_map_st[s] |= way_oh;
  }

  __always_inline void set_alloc_map(uint32_t s, int32_t v) {
#ifdef CHECK_MULTI
    auto thread_id = global_lock_checker->thread_id();
#endif
    if(v >= 0) {
      // check there is no pending operations
#ifdef CHECK_MULTI
  #ifdef BOOST_STACKTRACE_LINK
      if(alloc_record[s].first != 0) {
        std::cout << "caceh set " << s << " is being operated by trhead " << alloc_record[s].first << std::endl;
        std::cout << alloc_record[s].second << std::endl;
      }
      assert(alloc_record[s].first == 0);
      alloc_record[s] = std::make_pair(thread_id, boost::stacktrace::to_string(boost::stacktrace::stacktrace()));
  #else
      assert(alloc_record[s] == 0);
      alloc_record[s] = thread_id;
  #endif
#else
      assert(alloc_map[s] == -1 || 0 == "potential parallel allocated cache blocks in one cache set!");
#endif
    } else {
#ifdef CHECK_MULTI
  #ifdef BOOST_STACKTRACE_LINK
      if(alloc_record[s].first != thread_id) {
        std::cout << "caceh set " << s << " is being operated by trhead " << alloc_record[s].first << std::endl;
        std::cout << alloc_record[s].second << std::endl;
      }
      assert(alloc_record[s].first == thread_id);
      alloc_record[s] = std::make_pair(0, boost::stacktrace::to_string(boost::stacktrace::stacktrace()));
  #else
      assert(alloc_record[s] == thread_id);
      alloc_record[s] = 0;
  #endif
#endif
    }
    alloc_map[s] = v;
  }

public:
  ReplaceFuncBase(uint32_t nset)
    :used_map(nset), free_map_st(nset), free_map_mt(nset, nullptr), alloc_map(nset, -1) {
#ifdef CHECK_MULTI
  #ifdef BOOST_STACKTRACE_LINK
    alloc_record.resize(nset, {0, ""});
  #else
    alloc_record.resize(nset, 0);
  #endif
#endif
    constexpr uint64_t fmap = NW < 64 ? (1ull << NW) - 1 : ~(0ull);
    if constexpr (EnMT) {
      for (auto &s: free_map_mt) s = new std::atomic<uint64_t>(fmap);
    } else
      for (auto &s: free_map_st) s = fmap;
  }

  virtual ~ReplaceFuncBase() {
    if constexpr (EnMT) for (auto s: free_map_mt) delete s;
  }

  __always_inline uint32_t get_free_num(uint32_t s) { // return the number of free places by popcount the free map
    auto fmap = EnMT ? free_map_mt[s]->load() : free_map_st[s];
#ifdef __cpp_lib_bitops
    return std::popcount(fmap);
#elif defined __GNUG__
    return __builtin_popcountll(fmap);
#else
    uint32_t rv = 0;
    while(fmap) {
      rv += (fmap & 0x1ull);
      fmap >> 1;
    }
    return rv;
#endif
  }

  virtual void replace(uint32_t s, uint32_t *w, bool empty_fill_rt = true) {
    int32_t i = 0;
    if (EF && empty_fill_rt) {
      i = alloc_from_free(s);
      if (i<0) i = select(s);
    } else {
      i = select(s);
      delist_from_free(s, i);
    }
    assert((uint32_t)i < NW || 0 == "replacer used_map corrupted!");
	this->set_alloc_map(s, i);
    *w = i;
  }

  virtual void access(uint32_t s, uint32_t w, bool demand_acc, bool prefetch) = 0;

  virtual void invalid(uint32_t s, uint32_t w, bool flush = false) {
    if((int32_t)w != alloc_map[s]) list_to_free(s, w);
  }

  virtual uint32_t eviction_rank(uint32_t s, uint32_t w) const {
    return used_map[s][w];
  }
};

/////////////////////////////////
// FIFO replacement
// IW: index width, NW: number of ways
// EF: empty first, DUO: demand update only (do not update state for release)
template<int IW, int NW, bool EF, bool DUO, bool EnMT>
class ReplaceFIFO : public ReplaceFuncBase<EF, NW, EnMT>
{
  typedef ReplaceFuncBase<EF, NW, EnMT> RPT;
protected:
  using RPT::alloc_map;
  using RPT::used_map;

  virtual uint32_t select(uint32_t s) override {
    for(uint32_t i=0; i<NW; i++)
      if(used_map[s][i] == 0) return i;
    assert(0 == "replacer used_map corrupted!");
    return -1;
  }

public:
  ReplaceFIFO() : RPT(1ul << IW) {
    for (auto &s: used_map) {
      s.resize(NW);
      for(uint32_t i=0; i<NW; i++) s[i] = i;
    }
  }

  virtual void access(uint32_t s, uint32_t w, bool demand_acc, bool prefetch) override {
    if((int32_t)w == alloc_map[s] && demand_acc) {
      this->set_alloc_map(s, -1);
      auto prio = used_map[s][w];
      if(!prefetch) {
        for(uint32_t i=0; i<NW; i++) if(used_map[s][i] > prio) used_map[s][i]--;
        used_map[s][w] = NW-1;
      } else if(prio > 0) { // prefetch and insert into empty
        for(uint32_t i=0; i<NW; i++) if(used_map[s][i] < prio) used_map[s][i]++;
        used_map[s][w] = 0; // insert at LRU position
      }
    }
    if constexpr (EnMT) RPT::delist_from_free(s, w);
  }
};

/////////////////////////////////
// LRU replacement
// IW: index width, NW: number of ways
// EF: empty first, DUO: demand update only (do not update state for release)
template<int IW, int NW, bool EF, bool DUO, bool EnMT>
class ReplaceLRU : public ReplaceFIFO<IW, NW, EF, DUO, EnMT>
{
  typedef ReplaceFuncBase<EF, NW, EnMT> RPT;
protected:
  using RPT::alloc_map;
  using RPT::used_map;

public:
  virtual void access(uint32_t s, uint32_t w, bool demand_acc, bool prefetch) override {
    if((int32_t)w == alloc_map[s] || !DUO || demand_acc) {
      auto prio = used_map[s][w];
      if(!prefetch) {
        for(uint32_t i=0; i<NW; i++) if(used_map[s][i] > prio) used_map[s][i]--;
        used_map[s][w] = NW-1;
      } else if(prio > 0){ // prefetch and insert into empty
        for(uint32_t i=0; i<NW; i++) if(used_map[s][i] < prio) used_map[s][i]++;
        used_map[s][w] = 0; // insert at LRU position
      }
    }
    if((int32_t)w == alloc_map[s] && demand_acc) this->set_alloc_map(s, -1);
    if constexpr (EnMT) RPT::delist_from_free(s, w);
  }
};

/////////////////////////////////
// Static RRIP replacement
// IW: index width, NW: number of ways
// EF: empty first, DUO: demand update only (do not update state for release)
template<int IW, int NW, bool EF, bool DUO, bool EnMT>
class ReplaceSRRIP : public ReplaceFuncBase<EF, NW, EnMT>
{
  typedef ReplaceFuncBase<EF, NW, EnMT> RPT;
protected:
  using RPT::used_map;
  using RPT::alloc_map;

  virtual uint32_t select(uint32_t s) override {
    uint32_t max_prio = used_map[s][0];
    uint32_t max_i    = 0;
    for(uint32_t i=1; i<NW; i++) if(used_map[s][i] > max_prio) {max_prio = used_map[s][i]; max_i = i;}
    uint32_t gap = 3 - max_prio;
    if(gap > 0) for(uint32_t i=0; i<NW; i++) used_map[s][i] += gap;
    return max_i;
  }

public:
  ReplaceSRRIP() : RPT(1ul << IW) {
    for (auto &s: used_map) s.resize(NW, 3);
  }

  virtual void access(uint32_t s, uint32_t w, bool demand_acc, bool prefetch) override {
    if((int32_t)w == alloc_map[s] || !DUO || demand_acc) {
      if(!prefetch)
        used_map[s][w] = ((int32_t)w == alloc_map[s]) ? 2 : 0;
      else // prefetch
        used_map[s][w] = 3;
    }
    if((int32_t)w == alloc_map[s] && demand_acc) this->set_alloc_map(s, -1);
    if constexpr (EnMT) RPT::delist_from_free(s, w, demand_acc);
  }

  virtual void invalid(uint32_t s, uint32_t w, bool flush) override {
    used_map[s][w] = 3;
    RPT::invalid(s, w, false);
  }

  virtual uint32_t eviction_rank(uint32_t s, uint32_t w) const {
    uint32_t prio = used_map[s][w];
    uint32_t rank = 0;
    for(uint32_t i=1; i<NW; i++) if(used_map[s][i] > prio || (used_map[s][i] == prio && i < w)) rank++;
    return rank;
  }
};

/////////////////////////////////
// Random replacement
// IW: index width, NW: number of ways
// EF: empty first, DUO: demand update only (do not update state for release)
template<int IW, int NW, bool EF, bool DUO, bool EnMT>
class ReplaceRandom : public ReplaceFuncBase<EF, NW, EnMT>
{
  typedef ReplaceFuncBase<EF, NW, EnMT> RPT;
protected:
  using RPT::alloc_map;

  RandomGen<uint32_t> * loc_random; // a local randomizer for better thread parallelism

  virtual uint32_t select(uint32_t s) override {
    return (*loc_random)() % NW;
  }

public:
  ReplaceRandom() : RPT(1ul << IW), loc_random(cm_alloc_rand32()) {}
  virtual ~ReplaceRandom() override { delete loc_random; }

  virtual void access(uint32_t s, uint32_t w, bool demand_acc, bool prefetch) override {
    if((int32_t)w == alloc_map[s] && demand_acc) this->set_alloc_map(s, -1);
    if constexpr (EnMT) RPT::delist_from_free(s, w, demand_acc);
  }

  virtual uint32_t eviction_rank(uint32_t s, uint32_t w) const {
    return NW/2;
  }
};

#endif
