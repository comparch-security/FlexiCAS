#ifndef CM_CACHE_SLICEHASH_HPP_
#define CM_CACHE_SLICEHASH_HPP_

#include <unordered_map>
#include <cstdint>
#include <cassert>
#include "util/random.hpp"

/////////////////////////////////
// base class

class SliceHashBase
{
protected:
  int slice;
public:
  SliceHashBase(int s) : slice(s) {}
  virtual uint32_t operator () (uint64_t addr) = 0;
};

/////////////////////////////////
// normal (no hash)
template<int BlkOfst = 6>
class SliceHashNorm : public SliceHashBase
{
public:
  SliceHashNorm(int s): SliceHashBase(s) {}
  virtual uint32_t operator () (uint64_t addr) override { return (addr >> BlkOfst) % slice; }
};

/////////////////////////////////
// Inel complex address scheme (CAS)
class SliceHashIntelCAS : public SliceHashBase
{
  AddrXORHash hash;
public:
  SliceHashIntelCAS(int s) : SliceHashBase(s) {
    if (s == 2)      hash.key({0x15f575440ull});
    else if (s == 4) hash.key({0x6b5faa880ull, 0x35f575440ull});
    else if (s == 8) hash.key({0x3cccc93100ull, 0x2eb5faa880ull, 0x1b5f575400ull});
    else             assert(0 == "The number of slices must be equal to 2, 4 or 8!");
  }

  uint32_t virtual operator () (uint64_t addr) override { return hash(addr); }

};

#endif
