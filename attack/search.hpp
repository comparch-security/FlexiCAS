#ifndef ATT_SEARCH_HPP_
#define ATT_SEARCH_HPP_
#include <cstdint>
#include <list>
#include <functional>

typedef std::function<bool(uint64_t)> hit_func_t;
typedef std::function<bool(std::list<uint64_t>&)> check_func_t;


class CoherentL1CacheBase;

extern bool
find_conflict_set_by_repeat
(
 CoherentL1CacheBase * cache,       // the L1 cache that can be accessed
 hit_func_t hit,                    // the hit check function
 check_func_t check,                // check whether the evset is actually colocated
 uint64_t target,                   // the target to be evicted
 std::list<uint64_t> &evset,        // the found eviction set
 uint64_t evsize                    // size of the expected eviction set
 );

#endif