#ifndef CM_CACHE_COHERENCE_HPP
#define CM_CACHE_COHERENCE_HPP

#include <type_traits>
#include "cache/cache.hpp"

class OuterCohPortBase;
class InnerCohPortBase;
class CoherentCacheBase;

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
  InnerCohPortBase *inner; // inner port for probe when sync
  CohMasterBase *coh; // hook up with the coherence hub
  uint32_t coh_id; // the identifier used in locating this cache client by the coherence mster
public:
  OuterCohPortBase() {}
  virtual ~OuterCohPortBase() {}

  virtual void connect(CohMasterBase *h, uint32_t id) {coh = h; coh_id = id;}

  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd) = 0;
  virtual void writeback_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd) = 0;
  virtual void probe_resp(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd) {} // may not implement if not supported

  friend CoherentCacheBase; // deferred assignment for cache
};

/////////////////////////////////
// Base interface for inner ports

class InnerCohPortBase
{
protected:
  CacheBase *cache; // reverse pointer for the cache parent
  OuterCohPortBase *outer; // outer port for writeback when replace
  std::vector<CohClientBase *> coh; // hook up with the inner caches, indexed by vector index
public:
  InnerCohPortBase() {}
  virtual ~InnerCohPortBase() {}

  virtual uint32_t connect(CohClientBase *c) { coh.push_back(c); return coh.size() - 1;}

  virtual void acquire_resp(uint64_t addr, CMDataBase *data, uint32_t cmd) = 0;
  virtual void writeback_resp(uint64_t addr, CMDataBase *data, uint32_t cmd) = 0;
  virtual void probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd) {} // may not implement if not supported

  friend CoherentCacheBase; // deferred assignment for cache
};

// interface with the processing core is a special InnerCohPort
class CoreInterfaceBase : public InnerCohPortBase {
public:
  CoreInterfaceBase() {}
  virtual ~CoreInterfaceBase() {}

  virtual const CMDataBase *read(uint64_t addr) = 0;
  virtual void write(uint64_t addr, const CMDataBase *data) = 0;
  virtual void flush(uint64_t addr) = 0; // flush a cache block from the whole cache hierarchy, (clflush in x86-64)
  virtual void writeback(uint64_t addr) = 0; // if the block is dirty, write it back to memory, while leave the block cache in shared state (clwb in x86-64)
  virtual void writeback_invalidate() = 0; // writeback and invalidate all dirty cache blocks, sync with NVM (wbinvd in x86-64)

private:
  // hide and prohibit calling these functions
  virtual uint32_t connect(CohClientBase *c) { return 0;}
  virtual void acquire_resp(uint64_t addr, CMDataBase *data, uint32_t cmd) {}
  virtual void writeback_resp(uint64_t addr, CMDataBase *data, uint32_t cmd) {}
};


/////////////////////////////////
// Base class for a Cache supporting coherence operations

class CoherentCacheBase
{
protected:
  const std::string name;               // an optional name to describe this cache
  CacheBase *cache; // pointer to the actual cache

public:
  OuterCohPortBase *outer; // coherence outer port, nullptr if last level
  InnerCohPortBase *inner; // coherence inner port, nullptr if 1st level

  CoherentCacheBase(CacheBase *cache, OuterCohPortBase *outer, InnerCohPortBase *inner, std::string name)
    : name(name), cache(cache), outer(outer), inner(inner)
  {
    // deferred assignment for the reverse pointer to cache
    if(outer) { outer->cache = cache; outer->inner = inner; }
    if(inner) { inner->cache = cache; inner->outer = outer; }
  }

  virtual ~CoherentCacheBase() {
    delete cache;
    if(outer) delete outer;
    if(inner) delete inner;
  }
};


// Normal coherent cache
template<typename CacheT, typename OuterT, typename InnerT,
         typename = typename std::enable_if<std::is_base_of<CacheBase, CacheT>::value>::type,  // CacheT <- CacheBase
         typename = typename std::enable_if<std::is_base_of<OuterCohPortBase, OuterT>::value>::type, // OuterCohPortBase <- OuterT
         typename = typename std::enable_if<std::is_base_of<InnerCohPortBase, InnerT>::value>::type> // InnerCohPortBase <- InnerT
class CoherentCacheNorm : public CoherentCacheBase
{
public:
  CoherentCacheNorm(std::string name = "") : CoherentCacheBase(new CacheT(), new OuterT(), new InnerT(), name) {}
  virtual ~CoherentCacheNorm() {}
};

// Normal L1 coherent cache
template<typename CacheT, typename OuterT, typename CoreT,
         typename = typename std::enable_if<std::is_base_of<CoreInterfaceBase, CoreT>::value>::type> // CoreInterfaceBase <= CoreT
using CoherentL1CacheNorm = CoherentCacheNorm<CacheT, OuterT, CoreT>;


// Normal LLC supporting coherence
//  MCT: memory controller type, must be a derived class of OuterCohPortBase
template<typename CacheT, typename OuterT, typename InnerT>
using CoherentLLCNorm = CoherentCacheNorm<CacheT, OuterT, InnerT>;


#endif
