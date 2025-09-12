#ifndef CM_CHAMELEON_REPLACE_HPP_
#define CM_CHAMELEON_REPLACE_HPP_

#include "cache/replace.hpp"

/////////////////////////////////
// Chameleon Victim replacement
// IW: index width, NW: number of ways
// EF: empty first, DUO: demand update only (do not update state for release)
template<int IW, int NW, bool EF, bool DUO, bool EnMT>
class ReplaceChameleonVictim : public ReplaceFIFO<IW, NW, EF, DUO, EnMT>
{
  typedef ReplaceFIFO<IW, NW, EF, DUO, EnMT> RPT;
protected:
  using RPT::used_map;

public:
  virtual void replace(uint32_t s, uint32_t *w, bool empty_fill_rt = true) override {
    int32_t i = 0;
    i = this->select(s);
    if constexpr (!EF) {
      this->delist_from_free(s, i);
    }
    assert((uint32_t)i < NW || 0 == "replacer used_map corrupted!");
	this->set_alloc_map(s, i);
    *w = i;
  }

  std::vector<uint32_t> reinsert(uint32_t s) {
    std::vector<uint32_t> ways;
    for (int i = 0; i < NW; i++) {
      if(used_map[s][i] < NW-1) ways.push_back(i);
      // Perhaps only the cache block before insert pointer needs to be reinserted
      // But for the current auto-reinsert strategy: victim auto-reinsert every time it traverses, there is no need.
      if(used_map[s][i] == NW-1) break;
    }
    return ways;
  }
  
};

#endif
