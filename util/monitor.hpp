#ifndef CM_UTIL_PFC_HPP
#define CM_UTIL_PFC_HPP

#include <cstdint>
#include <set>

// monitor base class
class MonitorBase
{
public:
  MonitorBase() {}
  virtual ~MonitorBase() {}

  // standard functions to supprt a type of monitoring
  virtual bool attach(uint64_t cache_id) = 0; // decide whether to attach the mointor to this cache
  virtual void read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w) = 0;
  virtual void write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w) = 0;
  virtual void invalid(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w) = 0;
};

#endif
