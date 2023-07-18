#ifndef CM_REPLACE_HPP_
#define CM_REPLACE_HPP_

#include <list>
#include <unordered_set>
#include <unordered_map>
#include "util/random.hpp"

///////////////////////////////////
// Base class

class ReplaceFuncBase
{
protected:
  const uint32_t nset;
public:
  ReplaceFuncBase(uint32_t nset) : nset(nset) {};
  virtual uint32_t replace(uint32_t s, uint32_t *w) = 0;
  virtual void access(uint32_t s, uint32_t w) = 0;
  virtual void invalid(uint32_t s, uint32_t w) = 0;
  virtual ~ReplaceFuncBase() {}
};

/////////////////////////////////
// FIFO replacement
// IW: index width, NW: number of ways
template<int IW, int NW>
class ReplaceFIFO : public ReplaceFuncBase
{
protected:
  std::unordered_map<uint32_t, std::list<uint32_t> > used_map;
  std::unordered_map<uint32_t, std::unordered_set<uint32_t> > free_map;

public:
  ReplaceFIFO() : ReplaceFuncBase(1ul<<IW) {}
  virtual ~ReplaceFIFO() {}
  virtual uint32_t replace(uint32_t s, uint32_t *w){
    if(!free_map.count(s))
      for(uint32_t i=0; i<NW; i++) free_map[s].insert(i);
    if(free_map[s].size() > 0)
      *w = *(free_map[s].begin());
    else
      *w = used_map[s].front();

    return 0;
  }
  virtual void access(uint32_t s, uint32_t w) {
    if(free_map[s].count(w)) {
      free_map[s].erase(w);
      used_map[s].push_back(w);
    }
  }
  virtual void invalid(uint32_t s, uint32_t w){
    used_map[s].remove(w);
    free_map[s].insert(w);
  }
};

/////////////////////////////////
// LRU replacement
// IW: index width, NW: number of ways
template<int IW, int NW>
class ReplaceLRU : public ReplaceFIFO<IW, NW>
{
public:
  using ReplaceFIFO<IW,NW>::free_map;
  using ReplaceFIFO<IW,NW>::used_map;
  
  ReplaceLRU() : ReplaceFIFO<IW,NW>() {}
  ~ReplaceLRU() {}

  virtual void access(uint32_t s, uint32_t w) {
    if(free_map[s].count(w)) {
      free_map[s].erase(w);
      used_map[s].push_back(w);
    }
    else{
      used_map[s].remove(w);
      used_map[s].push_back(w);
    }
  }
};

#endif
