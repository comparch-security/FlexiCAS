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
  virtual ~IndexFuncBase() {}
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
  virtual ~IndexNorm() {}

  virtual uint32_t index(uint64_t addr, int partition) {
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
  virtual ~IndexSkewed() {}

  virtual uint32_t index(uint64_t addr, int partition) {
    return (hashers[partition](addr >> IOfst)) & mask;
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

class IndexRemapSupport
{
public:
  IndexRemapSupport() {}
  virtual ~IndexRemapSupport() {}

  virtual uint32_t next_index(uint64_t addr, int partition) = 0;
  virtual void rotate_indexer() = 0;
};

template<int IW, int IOfst, int P, typename IDX>
  requires C_DERIVE<IDX, IndexSkewed<IW, IOfst, P>>
class IndexRemapSkewed : public IndexFuncBase, 
                         public IndexRemapSupport 
{
  IDX* index_curr;
  IDX* index_next;
  std::vector<uint64_t> next_seeds;
public:
  IndexRemapSkewed() : IndexFuncBase((1ul << IW) - 1), index_curr(new IDX()), index_next(new IDX()) {
    next_seeds.resize(P);
    for(auto &s:next_seeds) s = cm_get_random_uint64();
    index_next->seed(next_seeds);
  }
  ~IndexRemapSkewed() {
    delete index_curr;
    delete index_next;
  }

  virtual uint32_t index(uint64_t addr, int partition) {
    return index_curr->index(addr, partition);
  }

  virtual uint32_t next_index(uint64_t addr, int partition) {
    return index_next->index(addr, partition);
  }

  virtual void rotate_indexer() {
    index_curr->seed(next_seeds);
    for(auto &s:next_seeds) s = cm_get_random_uint64();
    index_next->seed(next_seeds);
  }
};


#endif
