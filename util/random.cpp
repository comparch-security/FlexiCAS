#include "util/random.hpp"
#include <random>

// local variables (file local linkage)
namespace {

  std::random_device rd;
  std::uniform_int_distribution<uint32_t> uniform32(0, 1ul<<31);
  std::uniform_int_distribution<uint64_t> uniform64(0, 1ull<<63);

  // random engine
#ifdef NDEBUG
  // release mode
  // seed the mt19937 engine (normally better than default) with actual hardware generated random numbers
  std::mt19937    gen32(rd());
  std::mt19937_64 gen64(rd());
#else
  std::default_random_engine gen32, gen64;
#endif
}

unsigned int cm_get_true_random() { return rd(); }
void cm_set_random_seed(uint64_t seed) { gen32.seed(seed); gen64.seed(seed); }
uint64_t cm_get_random_uint64() { return uniform64(gen64); }
uint32_t cm_get_random_uint32() { return uniform32(gen32); }

std::unordered_map<uint32_t, std::string> UniqueID::ids;
