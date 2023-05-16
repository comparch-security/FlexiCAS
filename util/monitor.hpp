#ifndef CM_UTIL_PFC_HPP
#define CM_UTIL_PFC_HPP

#include <cstdint>
#include <set>

// monitor base class
class MonitorBase
{
public:
  MointorBase() {}
  virtual ~MonitorBase() {}

  // standard functions to supprt a type of monitoring
  virtual bool attach(uint64_t cache_id) = 0; // decide whether to attach the mointor to this cache
};

#endif
