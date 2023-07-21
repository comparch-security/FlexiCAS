#include "util/query.hpp"

bool query_hit(uint64_t addr, CacheBase *cache) {
  uint32_t ai, s, w;
  return cache->hit(addr);
}

bool query_check(uint64_t addr, CacheBase *cache, std::list<uint64_t> evset){
  for(auto a:evset) if(!cache->query_coloc(addr, a)) return false;
  return true;
}