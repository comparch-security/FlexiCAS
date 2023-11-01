#ifndef UTIL_QUERY_HPP_
#define UTIL_QUERY_HPP_

#include "cache/definitions.hpp"
#include "util/concept_macro.hpp"
#include <unordered_set>
#include <utility>
#include <string>
#include <boost/format.hpp>

template<typename MT> requires C_DERIVE(MT, CMMetadataBase)
class CBInfo
{
  MT meta;
public:
  CBInfo() {}
  CBInfo(MT m) { meta.copy(m);}
  // TODO: add address function
  bool invalid()  const { return meta.is_valid();}
  bool shared()   const { return meta.is_shared();   }
  bool modified() const { return meta.is_modified(); }
  bool dirty()    const { return meta.is_dirty();    }
  std::string to_string() const{
    std::string state;
    if(invalid()) state = "I";
    if(shared()) state = "S";
    if(modified()) state = "M";
    if(dirty()) state += "(D)";
    auto fmt = boost::format("%s ") % state;
    return fmt.str();
  }
};

template<typename MT> requires C_DERIVE(MT, CMMetadataBase)
class SetInfo{
public:
  std::vector<CBInfo<MT> > ways;
  std::string to_string() const{
    std::string rv;
    auto it = ways.begin();
    while(true) {
      rv += it->to_string();
      it++;
      if(it != ways.end()) rv += " ";
      else                 break;
    }
    return rv;
  }
};

class LocIdx{
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

namespace std{
  template <>
  struct hash<LocIdx>{
    std::size_t operator()(const LocIdx& l) const{
      std::size_t h1 = std::hash<uint32_t>{}(l.ai);
      std::size_t h2 = std::hash<uint32_t>{}(l.idx);
      return h1 ^ (h2 << 1);
    }
  };
}

class LocRange{
  std::pair<uint32_t, uint32_t> range;
public:
  LocRange() : range(0,0){}
  LocRange(uint32_t l, uint32_t h) : range(l, h) {}
  std::string to_string() const;
};

// the possible location of an address in a cache
class LocInfo {
public:
  uint32_t cache_id;
  CacheBase* cache;
//   CoherentCacheBase* wrapper;
  std::unordered_map<LocIdx, LocRange> locs;
  LocInfo() : cache_id(0) {}
  LocInfo(uint32_t cache_id, CacheBase* cache) : cache_id(cache_id), cache(cache) {}
  void insert(LocIdx idx, LocRange r) { locs[idx] = r; }
  std::string to_string() const;
};
#endif