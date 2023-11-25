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
public:
  virtual uint32_t operator () (uint64_t addr) = 0;
};

/////////////////////////////////
// normal (no hash)
template<int NLLC, int BlkOfst>
class SliceHashNorm : public SliceHashBase
{
public:
  virtual uint32_t operator () (uint64_t addr) { return (addr >> BlkOfst) % NLLC; }
};

/////////////////////////////////
// Inel complex address scheme (CAS)
template<int NLLC> requires NLLC <= 8 && NLLC == ((~NLLC + 1) & NLLC) // NLLC <= 8 and is power of 2
class SliceHashIntelCAS : public SliceHashBase
{
  AddrXORHash hash;
public:
  SliceHashIntelCAS() {
    if constexpr (NLLC == 2) hash.key({0x15f575440ull});
    if constexpr (NLLC == 4) hash.key({0x6b5faa880ull, 0x35f575440ull});
    if constexpr (NLLC == 8) hash.key({0x3cccc93100ull, 0x2eb5faa880ull, 0x1b5f575400ull});
  }

  uint32_t virtual operator () (uint64_t addr) { return hash(addr); }

};

#endif
