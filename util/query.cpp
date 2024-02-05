#include "util/query.hpp"
#include "cache/cache.hpp"
#include <boost/format.hpp>

std::string LocIdx::to_string() const{
  auto fmt = boost::format("partition: %1%, idx: %2%, way: ") % ai % idx;
  return fmt.str();
}

std::string LocRange::to_string() const{
  if(range.first == range.second){
    auto fmt = boost::format("[%1%]") % range.first;
    return fmt.str();
  } else{
    auto fmt = boost::format("[%1%:%2%]") % range.first % range.second;
    return fmt.str();
  }
}

std::string LocInfo::to_string() const{
  std::string rv = cache->get_name() + ": ";
  auto it = locs.begin();
  while(true){
    rv += it->first.to_string() + it->second.to_string();
    it++;
    if(it != locs.end()) rv += ", ";
    else { rv += "."; break; }
  }
  return rv;
}

void LocInfo::fill() {
  cache->query_fill_loc(this, addr);
}
