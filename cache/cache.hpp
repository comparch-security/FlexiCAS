#ifndef CM_CACHE_CACHE_HPP
#define CM_CACHE_CACHE_HPP

#include <cstdlib>
#include <cstring>
#include <string>
#include <set>
#include <map>
#include <vector>

#include "util/random.hpp"
#include "util/monitor.hpp"
#include "util/concept_macro.hpp"
#include "util/query.hpp"
#include "cache/index.hpp"
#include "cache/replace.hpp"
#include "cache/metadata.hpp"

// base class for a cache array:
class CacheArrayBase
{
protected:
  const std::string name;               // an optional name to describe this cache

public:
  CacheArrayBase(std::string name = "") : name(name) {}
  virtual ~CacheArrayBase() {}

  virtual bool hit(uint64_t addr, uint32_t s, uint32_t *w) const = 0;
  virtual CMMetadataBase * get_meta(uint32_t s, uint32_t w) = 0;
  virtual CMDataBase * get_data(uint32_t s, uint32_t w) = 0;
};

// normal set associative cache array
// IW: index width, NW: number of ways, MT: metadata type, DT: data type (void if not in use)
template<int IW, int NW, typename MT, typename DT>
  requires C_DERIVE(MT, CMMetadataBase) && C_DERIVE_OR_VOID(MT, CMMetadataBase)
class CacheArrayNorm : public CacheArrayBase
{
protected:
  std::vector<MT *> meta;   // meta array
  std::vector<DT *> data;   // data array, could be null
  unsigned int extra_way = 0;

public:
  static constexpr uint32_t nset = 1ul<<IW;  // number of sets

  CacheArrayNorm(unsigned int extra_way = 0, std::string name = "") : CacheArrayBase(name), extra_way(extra_way){
    size_t meta_num = nset * (NW + extra_way);
    constexpr size_t data_num = nset * NW;
    meta.resize(meta_num);
    for(auto &m:meta) m = new MT();
    if constexpr (!C_VOID(DT)) {
      data.resize(data_num);
      for(auto &d:data) d = new DT();
    }
  }

  virtual ~CacheArrayNorm() {
    for(auto m:meta) delete m;
    if constexpr (!C_VOID(DT)) for(auto d:data) delete d;
  }

  virtual bool hit(uint64_t addr, uint32_t s, uint32_t *w) const {
    for(int i=0; i<(NW+extra_way); i++)
      if(meta[s*NW + i]->match(addr)) {
        *w = i;
        return true;
      }

    return false;
  }

  virtual CMMetadataBase * get_meta(uint32_t s, uint32_t w) { return meta[s*(NW+extra_way) + w]; }
  virtual CMDataBase * get_data(uint32_t s, uint32_t w) {
    if constexpr (C_VOID(DT)) {
      return nullptr;
    } else
      return data[s*NW + w];
  }
};

//////////////// define cache ////////////////////

// base class for a cache
class CacheBase
{
protected:
  const uint32_t id;                    // a unique id to identify this cache
  const std::string name;               // an optional name to describe this cache

  // a vector of cache arrays
  // set-associative: one CacheArrayNorm objects
  // with VC: two CacheArrayNorm objects (one fully associative)
  // skewed: partition number of CacheArrayNorm objects (each as a single cache array)
  // MIRAGE: parition number of CacheArrayNorm (meta only) with one separate CacheArrayNorm for storing data (in derived class)
  std::vector<CacheArrayBase *> arrays;

public:
  MonitorContainerBase *monitors; // monitor container

  CacheBase(std::string name) : id(UniqueID::new_id(name)), name(name) {}

  virtual ~CacheBase() {
    for(auto a: arrays) delete a;
    delete monitors;
  }

  virtual bool hit(uint64_t addr,
                   uint32_t *ai,  // index of the hitting cache array in "arrays"
                   uint32_t *s, uint32_t *w
                   ) = 0;

  bool hit(uint64_t addr) {
    uint32_t ai, s, w;
    return hit(addr, &ai, &s, &w);
  }

  virtual bool replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, unsigned int genre = 0) = 0;

  // hook interface for replacer state update, Monitor and delay estimation
  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay, unsigned int genre = 0) = 0;
  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool is_release, uint64_t *delay, unsigned int genre = 0) = 0;
  // probe, invalidate and writeback
  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, uint64_t *delay, unsigned int genre = 0) = 0;

  virtual CMMetadataBase *access(uint32_t ai, uint32_t s, uint32_t w) {
    return arrays[ai]->get_meta(s, w);
  }

  virtual CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w) {
    return arrays[ai]->get_data(s, w);
  }

  uint32_t get_id() const { return id; }
  const std::string& get_name() const { return name;} 

  // access both meta and data in one function call
  virtual std::pair<CMMetadataBase *, CMDataBase *> access_line(uint32_t ai, uint32_t s, uint32_t w) = 0;

  virtual bool query_coloc(uint64_t addrA, uint64_t addrB) = 0;
  virtual LocInfo query_loc(uint64_t addr) = 0;
};

// Skewed Cache
// IW: index width, NW: number of ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, int P, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon>
  requires C_DERIVE(MT, CMMetadataBase) && C_DERIVE_OR_VOID(DT, CMDataBase) &&
           C_DERIVE(IDX, IndexFuncBase) && C_DERIVE(RPC, ReplaceFuncBase) && C_DERIVE_OR_VOID(DLY, DelayBase)
class CacheSkewed : public CacheBase
{
protected:
  IDX indexer;     // index resolver
  RPC replacer[P]; // replacer

public:
  CacheSkewed(std::string name = "", unsigned int extra_par = 0)
    : CacheBase(name)
  {
    arrays.resize(P+extra_par);
    for(int i=0; i<P; i++) arrays[i] = new CacheArrayNorm<IW,NW,MT,DT>();
    monitors = new CacheMonitorSupport<DLY, EnMon>(CacheBase::id);
  }

  virtual ~CacheSkewed() {}

  virtual bool hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w ) {
    for(*ai=0; *ai<P; (*ai)++) {
      *s = indexer.index(addr, *ai);
      for(*w=0; *w<NW; (*w)++)
        if(access(*ai, *s, *w)->match(addr)) return true;
    }
    return false;
  }

  virtual std::pair<CMMetadataBase *, CMDataBase *> access_line(uint32_t ai, uint32_t s, uint32_t w) {
    auto meta = arrays[ai]->get_meta(s, w);
    if constexpr (!C_VOID(DT))
      return std::make_pair(meta, arrays[ai]->get_data(s, w));
    else
      return std::make_pair(meta, nullptr);
  }

  virtual bool replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, unsigned int genre = 0) {
    if constexpr (P==1) *ai = 0;
    else                *ai = (cm_get_random_uint32() % P);
    *s = indexer.index(addr, *ai);
    replacer[*ai].replace(*s, w);
    return true;
  }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay, unsigned int genre = 0) {
    replacer[ai].access(s, w, false);
    if constexpr (EnMon || !C_VOID(DLY)) monitors->hook_read(addr, ai, s, w, hit, delay);
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool is_release, uint64_t *delay, unsigned int genre = 0) {
    replacer[ai].access(s, w, is_release);
    if constexpr (EnMon || !C_VOID(DLY)) monitors->hook_write(addr, ai, s, w, hit, delay);
  }

  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, uint64_t *delay, unsigned int genre = 0) {
    if(hit && evict) replacer[ai].invalid(s, w);
    if constexpr (EnMon || !C_VOID(DLY)) monitors->hook_manage(addr, ai, s, w, hit, evict, writeback, delay);
  }

  virtual bool query_coloc(uint64_t addrA, uint64_t addrB){
    for(int i=0; i<P; i++) 
      if(indexer.index(addrA, i) == indexer.index(addrB, i)) 
        return true;
    return false;
  }

  virtual LocInfo query_loc(uint64_t addr) {
    LocInfo rv(id, this);
    for(int i=0; i<P; i++){
      rv.insert(LocIdx(i, indexer.index(addr, i)), LocRange(0, NW-1));
    }
    return rv;
  }
};

// Normal set-associative cache
// IW: index width, NW: number of ways
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon>
using CacheNorm = CacheSkewed<IW, NW, 1, MT, DT, IDX, RPC, DLY, EnMon>;

#endif
