#ifndef CM_INDEX_HPP_
#define CM_INDEX_HPP_

#include<vector>

#include "util/random.hpp"

/////////////////////////////////
// Base class
class IndexFuncBase
{
protected: 
  const uint32_t mask;
public:
  IndexFuncBase(uint32_t mask) : mask(mask) {}
  virtual uint32_t index(uint64_t addr, int partition) = 0;
};


/////////////////////////////////
// Set associative caches
//   IW: index width, IOfst: index offset
template<int IW, int IOfst>
class IndexNorm : public IndexFuncBase
{
public:
  IndexNorm() : IndexFuncBase((1ul << IW) - 1) {}

  virtual uint32_t index(uint64_t addr, int partition) override {
    return (addr >> IOfst) & mask;
  }
};

/////////////////////////////////
// Skewed cache
//   IW: index width, IOfst: index offset, P: number of partitions
template<int IW, int IOfst, int P>
class IndexSkewed : public IndexFuncBase
{
  CMHasher hashers[P];
public:
  IndexSkewed() : IndexFuncBase((1ul << IW) - 1) {}

  virtual uint32_t index(uint64_t addr, int partition) override {
    return (hashers[partition](addr >> IOfst)) & mask;
  }

  void seed(std::vector<uint64_t>& seeds) {
    for(int i=0; i<P; i++) hashers[i].seed(seeds[i]);
  }

  void reseed() {
    for(int i=0; i<P; i++) hashers[i].seed(cm_get_random_uint64());
  }
};

/////////////////////////////////
// Set-associative random cache
//   IW: index width, IOfst: index offset
template<int IW, int IOfst>
using IndexRandom = IndexSkewed<IW,IOfst,1>;


#endif
