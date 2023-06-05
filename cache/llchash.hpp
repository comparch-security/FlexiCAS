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
  uint32_t nllc; // @wsong83 convert to template const expression?
public:
  LLCHashBase(uint32_t nllc) : nllc(nllc) {}
  virtual void set_nllc(uint32_t size) { nllc = size; }
  virtual uint32_t hash(uint64_t addr) = 0;
  virtual ~LLCHashBase() {}
};

/////////////////////////////////
// normal (no hash)

class LLCHashNorm : public LLCHashBase
{
public:
  LLCHashNorm(uint32_t nllc) : LLCHashBase(nllc) {}
  virtual uint32_t hash(uint64_t addr) { return (addr >> 6) % nllc; }
  virtual ~LLCHashNorm() {}
};

/////////////////////////////////
// hash
class LLCHashHash : public LLCHashBase
{
  uint32_t addr_xor(uint64_t mask, uint64_t addr) {
    uint64_t m = mask & addr;
    uint32_t rv = 0;
    for(auto i=0; i<64; i++) {
      rv ^= (m & 0x1);
      m >>= 1;
    }
    return rv;
  }

  std::unordered_map<uint64_t, uint32_t> hash_cache;
  
public:
  LLCHashHash(uint32_t nllc) : LLCHashBase(nllc) {}

  uint32_t virtual hash(uint64_t addr) {
    uint32_t rv = 0;
    if(hash_cache.count(addr)) return hash_cache[addr];

    switch(nllc) {
    case 2:
      rv = addr_xor(0x15f575440, addr);
      break;
    case 4:
      rv = (addr_xor(0x6b5faa880, addr) << 1) | addr_xor(0x35f575440, addr);
      break;
    case 8:
      rv = (addr_xor(0x3cccc93100, addr) << 2) |
           (addr_xor(0x2eb5faa880, addr) << 1) |
            addr_xor(0x1b5f575400, addr);
      break;
    default:
      //std::cout << "LLCHash: unsupport number of LLCs!";
      assert(0 == "LLCHash: unsupport number of LLCs!");
      return 0xffff;
    }
    hash_cache[addr] = rv;
    return rv;
  }
    
  virtual ~LLCHashHash() {}
};

#endif
