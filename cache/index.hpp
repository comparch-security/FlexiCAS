#ifndef CM_INDEX_HPP_
#define CM_INDEX_HPP_

#include<vector>

#include "util/random.hpp"

/////////////////////////////////
// Base class
//   IW: index width
template<int IW>
class IndexFuncBase
{
  const uint32_t mask;
public:
  IndexFuncBase() : mask((1ul << IW) - 1) {}
  virtual ~IndexFuncBase() {}
  virtual void index(uint64_t addr, std::vector<uint32_t>& indices) = 0;
};


/////////////////////////////////
// Set associative caches
//   IW: index width, IOfst: index offset
template<int IW, int IOfst>
class IndexNorm : public IndexFuncBase<IW>
{
public:  
  virtual ~IndexNorm() {}

  virtual void index(uint64_t addr, std::vector<uint32_t>& indices) {
    const uint32_t mask = (1ul << IW) - 1;
    indices[0] = (addr >> IOfst) & mask;
  }
};

/////////////////////////////////
// Skewed cache
//   IW: index width, IOfst: index offset, P: number of partitions
template<int IW, int IOfst, int P>
class IndexSkewed : public IndexFuncBase<IW>
{
  CMHasher hashers[P];
public:
  virtual ~IndexSkewed() {}

  virtual void index(uint64_t addr, std::vector<uint32_t>& indices) {
    uint64_t addr_s = addr >> IOfst;
    for(int i=0; i<P; i++)
      indices[i] = hashers[i](addr_s);
  }

  void seed(std::vector<uint64_t>& seeds) {
    for(int i=0; i<P; i++) hashers[i].seed(seeds[i]);
  }
};

/////////////////////////////////
// Set-associative random cache
//   IW: index width, IOfst: index offset
template<int IW, int IOfst>
using IndexRandom = IndexSkewed<IW,IOfst,1>;


#endif
