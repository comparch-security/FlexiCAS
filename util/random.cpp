#include "util/random.hpp"
#include <random>

// local variables (file local linkage)
namespace {
  std::default_random_engine gen;
  std::uniform_int_distribution<uint32_t> uniform32(0, 1ul<<31);
  std::uniform_int_distribution<uint64_t> uniform64(0, 1ull<<63);
}

void cm_set_random_seed(uint64_t seed) { gen.seed(seed); }
uint64_t cm_get_random_uint64() { return uniform64(gen); }
uint32_t cm_get_random_uint32() { return uniform32(gen); }

