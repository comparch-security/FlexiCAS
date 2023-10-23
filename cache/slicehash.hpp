#ifndef CM_CACHE_SLICEHASH_HPP_
#define CM_CACHE_SLICEHASH_HPP_

#include <unordered_map>
#include <cstdint>
#include <cassert>
#include <concepts>

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
  uint32_t addr_xor(uint64_t mask, uint64_t addr) {
    auto rv = mask & addr;
    rv = (rv >> 32) ^ (rv & 0x0ffffffffull);
    rv = (rv >> 16) ^ (rv & 0x0ffffull);
    rv = (rv >>  8) ^ (rv & 0x0ffull);
    rv = (rv >>  4) ^ (rv & 0x0full);
    rv = (rv >>  2) ^ (rv & 0x03ull);
    rv = (rv >>  1) ^ (rv & 0x01ull);
    return rv;
  }

public:
  uint32_t virtual operator () (uint64_t addr) {
    if constexpr (NLLC == 2) return  addr_xor(0x15f575440ull,  addr);
    if constexpr (NLLC == 4) return (addr_xor(0x6b5faa880ull,  addr) << 1) |  addr_xor(0x35f575440ull,  addr);
    if constexpr (NLLC == 8) return (addr_xor(0x3cccc93100ull, addr) << 2) | (addr_xor(0x2eb5faa880ull, addr) << 1) | addr_xor(0x1b5f575400ull, addr);
    return 0; // NLLC == 1
  }
};

#endif
