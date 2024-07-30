#include "util/random.hpp"
#include <random>
#include <type_traits>

// local variables (file local linkage)
namespace {

  template<typename rv_type, typename rand_gen_type, uint64_t max_value>
  class RandomGenBase : public RandomGen<rv_type> {
    rand_gen_type rand_gen;
    std::uniform_int_distribution<rv_type> uniform;
  public:
    RandomGenBase() : uniform(0, max_value) {}
    RandomGenBase(unsigned int s) : rand_gen(s), uniform(0, max_value) {}
    virtual rv_type operator ()() override { return uniform(rand_gen); }
    virtual void seed(uint64_t s) override { rand_gen.seed(s); }
  };

  template<typename rv_type, uint64_t max_value>
  using RandomGenDeault = RandomGenBase<rv_type, std::default_random_engine, max_value>;

  template<typename rv_type, uint64_t max_value>
  using RandomGenMT = RandomGenBase<rv_type,
                                    typename std::conditional<std::is_same<rv_type, uint64_t>::value, std::mt19937_64, std::mt19937>::type,
                                    max_value>;
  // random engine
  std::random_device rd;
#ifdef NDEBUG
  // release mode
  // seed the mt19937 engine (normally better than default) with actual hardware generated random numbers
  RandomGenMT<uint32_t, (1ull<<31)> g_gen32(rd());
  RandomGenMT<uint64_t, (1ull<<63)> g_gen64(rd());
#else
  RandomGenDeault<uint32_t, (1ull<<31)> g_gen32;
  RandomGenDeault<uint64_t, (1ull<<63)> g_gen64;
#endif

}

unsigned int cm_get_true_random() { return rd(); }
void cm_set_random_seed(uint64_t seed) { g_gen32.seed(seed); g_gen64.seed(seed); }
uint64_t cm_get_random_uint64() { return g_gen64(); }
uint32_t cm_get_random_uint32() { return g_gen32(); }

// allocate localized random number generator, normally for the multi-thread use case
#ifdef NDEBUG
  // release mode
  RandomGen<uint32_t> *cm_alloc_rand32() { return new RandomGenMT<uint32_t, (1ull<<31)>(rd()); }
  RandomGen<uint64_t> *cm_alloc_rand64() { return new RandomGenMT<uint64_t, (1ull<<63)>(rd()); }
#else
  RandomGen<uint32_t> *cm_alloc_rand32() { return new RandomGenDeault<uint32_t, (1ull<<31)>(); }
  RandomGen<uint64_t> *cm_alloc_rand64() { return new RandomGenDeault<uint64_t, (1ull<<63)>(); }
#endif

std::unordered_map<uint32_t, std::string> UniqueID::ids;
