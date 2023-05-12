#ifndef CM_UTIL_RANDOM_HPP_
#define CM_UTIL_RANDOM_HPP_

#include <cstdint>

extern void cm_set_random_seed(uint64_t seed);
extern uint64_t cm_get_random_uint64();
extern uint32_t cm_get_random_uint32();

#include "cryptopp/cryptlib.h"
#include "cryptopp/tiger.h"

// see https://cryptopp.com/wiki/Tiger

class CMHasher {
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

#endif
