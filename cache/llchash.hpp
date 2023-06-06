#ifndef CM_CACHE_LLCHASH_HPP_
#define CM_CACHE_LLCHASH_HPP_

#include <unordered_map>
#include <cstdint>
#include <cassert>

/////////////////////////////////
// base class

class LLCHashBase  // @wsong83 LLCHash or just SliceHash ?
{
protected:
  const uint32_t nllc; // @wsong83 convert to template const expression?
public:
  LLCHashBase(uint32_t nllc) : nllc(nllc) {}
  virtual uint32_t operator () (uint64_t addr) = 0;
};

/////////////////////////////////
// normal (no hash)

class LLCHashNorm : public LLCHashBase
{
public:
  LLCHashNorm(uint32_t nllc) : LLCHashBase(nllc) {}
  virtual uint32_t operator () (uint64_t addr) { return (addr >> 6) % nllc; }
};

/////////////////////////////////
// hash
class LLCHashHash : public LLCHashBase
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
  LLCHashHash(uint32_t nllc) : LLCHashBase(nllc) {}

  uint32_t virtual operator () (uint64_t addr) {
    switch(nllc) {
    case 2:
      return addr_xor(0x15f575440, addr);
    case 4:
      return (addr_xor(0x6b5faa880, addr) << 1) | addr_xor(0x35f575440, addr);
    case 8:
      return (addr_xor(0x3cccc93100, addr) << 2) |
             (addr_xor(0x2eb5faa880, addr) << 1) |
             addr_xor(0x1b5f575400, addr);
    }
    return 0;
  }
};

#endif
