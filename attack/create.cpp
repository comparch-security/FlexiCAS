#include "attack/create.hpp"
#include "cache/cache.hpp"
#include <unordered_set>

bool
produce_targeted_evict_set
(
 std::list<uint64_t>& evset_rv,     // the generated eviction set
 uint32_t evset_size,               // expected size of the eviction set
 CoherentL1CacheBase* cache,        // the L1 cache that can be accessed
 CacheBase* c,                      // the cache that to be checked   
 uint64_t target                    // the target address to be evicted
 )
{
  uint64_t i = 0;
  std::unordered_set<uint64_t> evset;
  while(evset.size() < evset_size){
    uint64_t addr = i * 64;
    if(c->query_coloc(target, addr))
        evset.insert(addr);
    i++;
  }
  if(evset.size() == evset_size) {
    evset_rv.insert(evset_rv.end(), evset.begin(), evset.end());
    return true;
  } else
    return false;
}