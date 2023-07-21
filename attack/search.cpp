#include "util/random.hpp"
#include "attack/search.hpp"
#include "cache/coherence.hpp"
#include <iostream>

static uint32_t loop_size, loop_count, loop_count_max;

bool static loop_guard(uint32_t new_size) {
  if(loop_size == new_size) {
    loop_count++;
    if(loop_count > loop_count_max)
      return false;
  } else {
    loop_count = 0;
    loop_size = new_size;
    loop_count_max = new_size * 4;
  }
  return true;
}

bool find_conflict_set_by_repeat(
                       CoherentL1CacheBase* cache,
                       hit_func_t hit,
                       check_func_t check,
                       uint64_t target,
                       std::list<uint64_t> &evset,
                       uint64_t evsize
                       )
{
  std::unordered_set<uint64_t> evset_set;
  
  cache->read(target);

  while(evset_set.size() < evsize && loop_guard(10000))  {
    uint64_t c = cm_get_random_uint64();
    cache->read(c);

    bool h = hit(target);
    cache->read(target);
    if(!h) {
      evset_set.insert(c);
      loop_guard(10);
    }
  }
  evset.insert(evset.begin(), evset_set.begin(), evset_set.end());
  return evset_set.size() == evsize  && check(evset);
}

