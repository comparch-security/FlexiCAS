#ifndef UTIL_QUERY_HPP_
#define UTIL_QUERY_HPP_

#include <unordered_map>
#include <utility>
#include <string>
#include <cstdint>

class CacheBase;

class LocIdx final {
public:
  uint32_t ai;
  uint32_t idx;
  LocIdx() : ai(0), idx(0){}
  LocIdx(uint32_t ai, uint32_t idx) : ai(ai), idx(idx) {}

  bool operator==(const LocIdx& other) const{
    return ai == other.ai && idx == other.idx;
  }
  std::string to_string() const;
};

namespace std {
  template <>
  struct hash<LocIdx>{
    std::size_t operator()(const LocIdx& l) const {
      std::size_t h1 = std::hash<uint32_t>{}(l.ai);
      std::size_t h2 = std::hash<uint32_t>{}(l.idx);
      return h1 ^ (h2 << 1);
    }
  };
}

class LocRange final {
  std::pair<uint32_t, uint32_t> range;
public:
  LocRange() : range(0,0){}
  LocRange(uint32_t l, uint32_t h) : range(l, h) {}
  std::string to_string() const;
};

// the possible location of an address in a cache
class LocInfo final {
  bool filled;
  uint64_t addr;
public:
  uint32_t cache_id;
  CacheBase* cache;
  std::unordered_map<LocIdx, LocRange> locs;
  LocInfo() : cache_id(0) {}
  LocInfo(uint32_t cache_id, CacheBase* cache, uint64_t addr) : filled(false), addr(addr), cache_id(cache_id), cache(cache) {}
  void insert(LocIdx idx, LocRange r) { locs[idx] = r; }
  void fill();
  std::string to_string() const;
};
#endif
