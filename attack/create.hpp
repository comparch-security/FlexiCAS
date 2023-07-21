#ifndef ATTACK_CREATE_HPP_
#define ATTACK_CREATE_HPP_
#include "search.hpp"

class CacheBase;

extern bool
produce_targeted_evict_set
(
 std::list<uint64_t>& evset_rv,     // the generated eviction set
 uint32_t evset_size,               // expected size of the eviction set
 CoherentL1CacheBase* cache,        // the L1 cache that can be accessed
 CacheBase* c,                      // the cache that to be checked   
 uint64_t target                    // the target address to be evicted
 );



#endif