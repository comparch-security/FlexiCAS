#ifndef CM_UTIL_RANDOM_HPP_
#define CM_UTIL_RANDOM_HPP_

#include <cstdint>
#include <unordered_map>
#include <string>

template<typename rv_type>
class RandomGen {
public:
  virtual ~RandomGen() = default;
  virtual rv_type operator ()() = 0;
  virtual void seed(uint64_t s) = 0;
};

extern unsigned int cm_get_true_random();
extern void cm_set_random_seed(uint64_t seed);
extern uint64_t cm_get_random_uint64();
extern uint32_t cm_get_random_uint32();
extern RandomGen<uint32_t> *cm_alloc_rand32(); // generate a local random generator for a thread
extern RandomGen<uint64_t> *cm_alloc_rand64();

#include "cryptopp/cryptlib.h"
#include "cryptopp/tiger.h"

// see https://cryptopp.com/wiki/Tiger

class CMHasher final {
  uint8_t msg[16];
  uint8_t result[8];
  CryptoPP::Tiger hasher;

public:
  CMHasher() {
    // set an initial seed
    *(uint64_t *)(msg+8) = cm_get_random_uint64();
  }

  CMHasher(uint64_t s) { *(uint64_t *)(msg+8) = s; }

  uint64_t operator () (uint64_t data) {
    *(uint64_t *)(msg) = data;
    hasher.Update(msg, 16);
    hasher.TruncatedFinal(result, 8);
    return *(uint64_t *)(result);
  }

  void seed(uint64_t s) {
    *(uint64_t *)(msg+8) = s;
  }
};

// record and generate a unique ID
class UniqueID final
{
protected:
  static std::unordered_map<uint32_t, std::string> ids;
public:
  // generate a new unique id
  static uint32_t new_id(const std::string &name) {
    uint32_t id = cm_get_random_uint32();
    while(ids.count(id)) id = cm_get_random_uint32();
    ids[id] = name;
    return id;
  }

  static std::string name(uint32_t id) {
    if(ids.count(id)) return ids[id];
    return "";
  }
};

// the XOR hash used by Intel comples address scheme
class AddrXORHash final
{
  std::vector<uint64_t> keys;

  uint32_t hash(uint64_t mask, uint64_t addr) {
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
  AddrXORHash() {}
  AddrXORHash(unsigned int nkey) : keys(nkey, 0) { key(); }
  AddrXORHash(const std::vector<uint64_t>& keys) : keys(keys) {}

  void key() {
    for(auto &k : keys) k = cm_get_random_uint64();
  }

  void key(const std::vector<uint64_t>& k) {
    keys = k;
  }

  uint32_t operator() (uint64_t addr) {
    uint64_t rv = 0;
    for(auto g: keys) rv = (rv << 1) | hash(g, addr);
    return rv;
  }
};


#endif
