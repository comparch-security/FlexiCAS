#ifndef CM_CACHE_COHERENCE_HPP
#define CM_CACHE_COHERENCE_HPP

#include "cache/cache.hpp"

class OuterCohPortBase;
class InnerCohPortBase;

// proactive support for future parallel simulation using multi-thread
//   When multi-thread is supported,
//   actual coherence client and master helper classes will implement the cross-thread FIFOs
typedef OuterCohPortBase CohClientBase;
typedef InnerCohPortBase CohMasterBase;

/////////////////////////////////
// Base interface for outer ports

class OuterCohPortBase
{
protected:
  CacheBase *cache; // reverse pointer for the cache parent
  CohMasterBase *coh; // hook up with the coherence hub
  uint32_t coh_id; // the identifier used in locating this cache client by the coherence mster
public:
  OuterCohPortBase(CacheBase *cache) : cache(cache), coh(nullptr) {}
  virtual ~OuterCohPortBase() {}

  void connect(CohMasterBase *h, uint32_t id) {coh = h; coh_id = id;}

  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd) = 0;
  virtual void writeback_req(uint64_t addr, CMDataBase *data) = 0;
  virtual void probe_resp(uint64_t addr, CMDataBase *data, uint32_t cmd) {} // may not implement if not supported
};


/////////////////////////////////
// Base interface for inner ports

class InnerCohPortBase
{
protected:
  CacheBase *cache; // reverse pointer for the cache parent
  std::vector<CohClientBase *> coh; // hook up with the inner caches, indexed by vector index
public:
  InnerCohPortBase(CacheBase *cache) : cache(cache) {}
  virtual ~InnerCohPortBase() {}

  void connect(CohClientBase *c) { coh.push_back(c); }

  virtual void acquire_resp(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd) = 0;
  virtual void writeback_resp(uint64_t addr, CMDataBase *data) = 0;
  virtual void probe_req(uint64_t addr, CMDataBase *data, uint32_t cmd) {} // may not implement if not supported
};


/////////////////////////////////
// Base class for a Cache supporting coherence operations

class CoherentCacheBase
{
protected:
  const std::string name;               // an optional name to describe this cache

  CacheBase *cache; // pointer to the actual cache
  OuterCohPortBase *outer; // coherence outer port, nullptr if last level
  InnerCohPortBase *inner; // coherence inner port, nullptr if 1st level

public:
  CoherentCacheBase(CacheBase *cache, OuterCohPortBase *outer, InnerCohPortBase *inner, std::string name)
    : name(name), cache(cache), outer(outer), inner(inner)
  {}
  virtual ~CoherentCacheBase() {
    delete cache;
    if(outer) delete outer;
    if(inner) delete inner;
  }

  virtual std::pair<const CMMetadataBase *, const CMDataBase *> read(uint64_t addr) = 0; // read a cache block
  virtual std::pair<CMMetadataBase *, CMDataBase *> access(uint64_t addr) = 0; // access a cache block for modification (write)
};

#endif
