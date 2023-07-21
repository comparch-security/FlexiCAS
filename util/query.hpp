#include "cache/cache.hpp"

extern bool query_hit(uint64_t addr, CacheBase *cache);
bool query_check(uint64_t addr, CacheBase *cache, std::list<uint64_t> evset);