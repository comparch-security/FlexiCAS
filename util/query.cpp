#include "util/query.hpp"
#include "cache/cache.hpp"
#include <boost/format.hpp>

std::string LocIdx::to_string() const{
  return (boost::format("partition: %1%, idx: %2%, way: ") % ai % idx).str();
}

std::string LocRange::to_string() const{
  if(range.first == range.second) return (boost::format("[%1%]") % range.first).str();
  else                            return (boost::format("[%1%:%2%]") % range.first % range.second).str();
}

std::string LocInfo::to_string() const{
  std::string rv; rv.reserve(100);
  rv.append(cache->get_name()).append(": ");
  for(auto it = locs.begin(); it != locs.end(); rv.append(++it == locs.end() ? "." : ", "))
    rv.append(it->first.to_string()).append(it->second.to_string());
  return rv;
}

void LocInfo::fill() {
  cache->query_fill_loc(this, addr);
}
