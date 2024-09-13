#ifndef CM_REPLACE_HPP_
#define CM_REPLACE_HPP_

#include <vector>
#include <cassert>
#include <mutex>
#include "util/random.hpp"
#include "util/multithread.hpp"

///////////////////////////////////
// Base class
// EF: empty first
template<bool EF>
class ReplaceFuncBase
{
  const uint32_t NW;
protected:
  std::vector<std::vector<uint32_t> > used_map; // at the size of 16, vector is actually faster than list and do not require alloc
  std::vector<std::vector<bool> > free_map;
  std::vector<int32_t> alloc_map; // record the way allocated for the next access (only one allocated ay at any time)
  std::vector<uint32_t> free_num;

#ifdef CHECK_MULTI
  #ifdef BOOST_STACKTRACE_LINK
    std::vector<std::pair<uint64_t, std::string> > alloc_record;
  #else
    std::vector<uint64_t> alloc_record;
  #endif
#endif

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

  __always_inline void delist_from_free(uint32_t s, uint32_t w, bool demand_acc) {
    // in multithread simulation, a simultaneous probe may invalidate a cache block waiting for permission promotion
    if(!free_map[s][w]) return;
    assert(demand_acc); // assume such situation can occur only in permission promotion
    free_map[s][w] = false;
    free_num[s]--;
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
  ReplaceFuncBase(uint32_t nset, uint32_t nway)
    :NW(nway), used_map(nset), free_map(nset), alloc_map(nset, -1), free_num(nset, nway) {
#ifdef CHECK_MULTI
  #ifdef BOOST_STACKTRACE_LINK
    alloc_record.resize(nset, {0, ""});
  #else
    alloc_record.resize(nset, 0);
  #endif
#endif
    for (auto &s: free_map) s.resize(NW, true);
  }

  virtual ~ReplaceFuncBase() = default;

  __always_inline uint32_t get_free_num(uint32_t s) const { return free_num[s]; }

  virtual void replace(uint32_t s, uint32_t *w) {
    uint32_t i = 0;
    if constexpr (EF) {
      if(free_num[s] > 0) i = alloc_from_free(s);
      else                i = select(s);
    } else {
      i = select(s);
      if(free_map[s][i]) { free_num[s]--; free_map[s][i] = false; }
    }
    assert(i < NW || 0 == "replacer used_map corrupted!");
	this->set_alloc_map(s, i);
    *w = i;
  }

  virtual void access(uint32_t s, uint32_t w, bool demand_acc, bool prefetch) = 0;

  virtual void invalid(uint32_t s, uint32_t w) {
    if((int32_t)w != alloc_map[s]) {
      free_map[s][w] = true;
      free_num[s]++;
    }
  }
};

/////////////////////////////////
// FIFO replacement
// IW: index width, NW: number of ways
// EF: empty first, DUO: demand update only (do not update state for release)
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceFIFO : public ReplaceFuncBase<EF>
{
  typedef ReplaceFuncBase<EF> RPT;
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
  ReplaceFIFO() : RPT(1ul << IW, NW) {
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
    RPT::delist_from_free(s, w, demand_acc);
  }
};

/////////////////////////////////
// LRU replacement
// IW: index width, NW: number of ways
// EF: empty first, DUO: demand update only (do not update state for release)
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceLRU : public ReplaceFIFO<IW, NW, EF, DUO>
{
  typedef ReplaceFuncBase<EF> RPT;
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
    RPT::delist_from_free(s, w, demand_acc);
  }
};

/////////////////////////////////
// Static RRIP replacement
// IW: index width, NW: number of ways
// EF: empty first, DUO: demand update only (do not update state for release)
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceSRRIP : public ReplaceFuncBase<EF>
{
  typedef ReplaceFuncBase<EF> RPT;
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
  ReplaceSRRIP() : RPT(1ul << IW, NW) {
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
    RPT::delist_from_free(s, w, demand_acc);
  }

  virtual void invalid(uint32_t s, uint32_t w) override {
    used_map[s][w] = 3;
    RPT::invalid(s, w);
  }
};

/////////////////////////////////
// Random replacement
// IW: index width, NW: number of ways
// EF: empty first, DUO: demand update only (do not update state for release)
template<int IW, int NW, bool EF = true, bool DUO = true>
class ReplaceRandom : public ReplaceFuncBase<EF>
{
  typedef ReplaceFuncBase<EF> RPT;
protected:
  using RPT::alloc_map;

  RandomGen<uint32_t> * loc_random; // a local randomizer for better thread parallelism

  virtual uint32_t select(uint32_t s) override {
    return (*loc_random)() % NW;
  }

public:
  ReplaceRandom() : RPT(1ul << IW, NW), loc_random(cm_alloc_rand32()) {}
  virtual ~ReplaceRandom() override { delete loc_random; }

  virtual void access(uint32_t s, uint32_t w, bool demand_acc, bool prefetch) override {
    if((int32_t)w == alloc_map[s] && demand_acc) this->set_alloc_map(s, -1);
    RPT::delist_from_free(s, w, demand_acc);
  }
};

#endif
