#ifndef CM_UTIL_RANDOM_HPP_
#define CM_UTIL_RANDOM_HPP_

#include <cstdint>

extern void set_random_seed(uint64_t seed);
extern uint64_t get_random_uint64();
extern uint64_t get_random_uint32();

#endif
