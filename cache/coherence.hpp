#ifndef CM_CACHE_COHERENCE_HPP
#define CM_CACHE_COHERENCE_HPP

#include "cache/cache.hpp"

/////////////////////////////////
// Base interface for outer ports

class OuterPortBase
{
protected:
  CacheBase *cache;
public:
  OuterPortBase(CacheBase *cache) : cache(cache) {}
  virtual ~OuterPortBase() {}
  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd) = 0;
  virtual void writeback_req(uint64_t addr, CMDataBase *data) = 0;
  virtual void probe_resp(uint64_t addr, CMDataBase *data, uint32_t cmd) = 0;
}




#endif
