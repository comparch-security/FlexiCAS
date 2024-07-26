#ifndef CM_CACHE_CACHE_HPP
#define CM_CACHE_CACHE_HPP

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "util/random.hpp"
#include "util/monitor.hpp"
#include "util/concept_macro.hpp"
#include "util/query.hpp"
#include "util/multithread.hpp"
#include "cache/index.hpp"
#include "cache/replace.hpp"
#include "cache/metadata.hpp"

//#include <iostream>

// base class for a cache array:
class CacheArrayBase
{
protected:
  const std::string name;               // an optional name to describe this cache

public:
  CacheArrayBase(std::string name = "") : name(name) {}
  virtual ~CacheArrayBase() {}

  virtual bool hit(uint64_t addr, uint32_t s, uint32_t *w) const = 0;
  virtual CMMetadataCommon * get_meta(uint32_t s, uint32_t w) = 0;
  virtual CMDataBase * get_data(uint32_t s, uint32_t w) = 0;

  // support multithread
  virtual void set_mt_state(uint32_t s, uint16_t prio) = 0;   // preserve a cache set according to transaction priority
  virtual void check_mt_state(uint32_t s, uint16_t prio) = 0; // check priority before continuing remaining work  on a cache set
  virtual void reset_mt_state(uint32_t s, uint16_t prio) = 0; // reset the state of a cache set after processing a transaction
};

// normal set associative cache array
// IW: index width, NW: number of ways, MT: metadata type, DT: data type (void if not in use)
// EnMT: enable multithread support
template<int IW, int NW, typename MT, typename DT, bool EnMT>
  requires C_DERIVE<MT, CMMetadataCommon> && C_DERIVE_OR_VOID<DT, CMDataBase>
class CacheArrayNorm : public CacheArrayBase
{
  typedef typename std::conditional<EnMT, MetaLock<MT>, MT>::type C_MT;
protected:
  std::vector<C_MT *> meta;   // meta array
  std::vector<DT *> data;   // data array, could be null
  const unsigned int way_num;
  std::vector<AtomicVar<uint16_t> > cache_set_state;  // record current transactions for multithread support

public:
  static constexpr uint32_t nset = 1ul<<IW;  // number of sets

  CacheArrayNorm(unsigned int extra_way = 0, std::string name = "") : CacheArrayBase(name), way_num(NW+extra_way){
    size_t meta_num = nset * way_num;
    constexpr size_t data_num = nset * NW;

    meta.resize(meta_num);
    for(auto &m:meta) m = new C_MT();
    if(extra_way)
      for(unsigned int s=0; s<nset; s++)
        for(unsigned int w=NW; w<way_num; w++)
          meta[s*way_num+w]->to_extend();

    if constexpr (!C_VOID<DT>) {
      data.resize(data_num);
      for(auto &d:data) d = new DT();
    }

    if constexpr (EnMT) cache_set_state.resize(nset);
  }

  virtual ~CacheArrayNorm() {
    for(auto m:meta) delete m;
    if constexpr (!C_VOID<DT>) for(auto d:data) delete d;
  }

  virtual bool hit(uint64_t addr, uint32_t s, uint32_t *w) const {
    for(unsigned int i=0; i<way_num; i++)
      if(meta[s*way_num + i]->match(addr)) {
        *w = i;
        return true;
      }
    return false;
  }

  virtual CMMetadataCommon * get_meta(uint32_t s, uint32_t w) { return meta[s*way_num + w]; }
  virtual CMDataBase * get_data(uint32_t s, uint32_t w) {
    if constexpr (C_VOID<DT>) return nullptr;
    else                      return data[s*NW + w];
  }

  virtual void set_mt_state(uint32_t s, uint16_t prio) {
    while(true) {
      auto state = cache_set_state[s].read();
      if(prio <= state) { cache_set_state[s].wait(); continue; }
      if(prio > state && cache_set_state[s].swap(state, state|prio)) break;
    }
  }

  virtual void check_mt_state(uint32_t s, uint16_t prio) {
    auto prio_upper = (prio << 1) - 1;
    while(true) {
      auto state = cache_set_state[s].read();
      assert(state >= prio);
      if(prio_upper >= state) break;
      cache_set_state[s].wait();
    }
  }

  virtual void reset_mt_state(uint32_t s, uint16_t prio) {
    while(true) {
      auto state = cache_set_state[s].read();
      assert(state == state | prio);
      if(cache_set_state[s].swap(state, state & (~prio), true)) break;
    }
  }
};

//////////////// define cache ////////////////////

// base class for a cache
class CacheBase : public CacheMonitorSupport
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
  CacheBase(std::string name) : id(UniqueID::new_id(name)), name(name) {}

  virtual ~CacheBase() {
    for(auto a: arrays) delete a;
  }

  virtual bool hit(uint64_t addr,
                   uint32_t *ai,  // index of the hitting cache array in "arrays"
                   uint32_t *s, uint32_t *w
                   ) = 0;

  bool hit(uint64_t addr) {
    uint32_t ai, s, w;
    return hit(addr, &ai, &s, &w);
  }

  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, unsigned int genre = 0) = 0;

  virtual CMMetadataCommon *access(uint32_t ai, uint32_t s, uint32_t w) {
    return arrays[ai]->get_meta(s, w);
  }

  virtual CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w) {
    return arrays[ai]->get_data(s, w);
  }

  // methods for supporting multithread execution
  virtual CMDataBase *data_copy_buffer() = 0;               // allocate a copy buffer, needed by exclusive cache with extended meta
  virtual void data_return_buffer(CMDataBase *buf) = 0;     // return a copy buffer, used to detect conflicts in copy buffer
  virtual CMMetadataBase *meta_copy_buffer() = 0;           // allocate a copy buffer, needed by exclusive cache with extended meta
  virtual void meta_return_buffer(CMMetadataBase *buf) = 0; // return a copy buffer, used to detect conflicts in copy buffer
  __always_inline void lock_line(uint32_t ai, uint32_t s, uint32_t w)   { access(ai, s, w)->lock();   }
  __always_inline void unlock_line(uint32_t ai, uint32_t s, uint32_t w) { access(ai, s, w)->unlock(); }

  virtual std::tuple<int, int, int> size() const = 0;           // return the size parameters of the cache
  uint32_t get_id() const { return id; }
  const std::string& get_name() const { return name;} 

  // access both meta and data in one function call
  virtual std::pair<CMMetadataBase *, CMDataBase *> access_line(uint32_t ai, uint32_t s, uint32_t w) = 0;

  virtual bool query_coloc(uint64_t addrA, uint64_t addrB) = 0;
  virtual LocInfo query_loc(uint64_t addr) { return LocInfo(id, this, addr); }
  virtual void query_fill_loc(LocInfo *loc, uint64_t addr) = 0;
};

// Skewed Cache
// IW: index width, NW: number of ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
// EF: empty first in replacer
// EnMT: enable multithread, MSHR: maximal number of transactions on the fly
template<int IW, int NW, int P, typename MT, typename DT, typename IDX, typename RPC, typename DLY,
         bool EnMon, bool EF = true, bool EnMT = false, int MSHR = 4>
  requires C_DERIVE<MT, CMMetadataBase> && C_DERIVE_OR_VOID<DT, CMDataBase> &&
           C_DERIVE<IDX, IndexFuncBase> && C_DERIVE<RPC, ReplaceFuncBase<EF, EnMT> > && C_DERIVE_OR_VOID<DLY, DelayBase> &&
           MSHR >= 2 // 2 buffers are required even for single-thread simulation
class CacheSkewed : public CacheBase
{
  typedef typename std::conditional<EnMT, AtomicVar<uint16_t>, uint16_t>::type buffer_state_t;
protected:
  IDX indexer;      // index resolver
  RPC replacer[P];  // replacer
  RandomGen<uint32_t> * loc_random; // a local randomizer for better thread parallelism

  std::unordered_set<CMDataBase *> data_buffer_pool_set;
  std::vector<CMDataBase *>        data_buffer_pool;
  buffer_state_t                   data_buffer_state;

  std::unordered_set<CMMetadataBase *> meta_buffer_pool_set;
  std::vector<CMMetadataBase *>        meta_buffer_pool;
  buffer_state_t                       meta_buffer_state;

public:
  CacheSkewed(std::string name = "", unsigned int extra_par = 0, unsigned int extra_way = 0)
    : CacheBase(name), loc_random(nullptr), data_buffer_state(MSHR), meta_buffer_pool(MSHR), meta_buffer_state(MSHR)
  {
    arrays.resize(P+extra_par);
    for(int i=0; i<P; i++) arrays[i] = new CacheArrayNorm<IW,NW,MT,DT,EnMT>(extra_way);
    CacheMonitorSupport::monitors = new CacheMonitorImp<DLY, EnMon>(CacheBase::id);

    if constexpr (P>1) loc_random = cm_alloc_rand32();

    // allocate buffer pools
    meta_buffer_pool.resize(MSHR, nullptr);
    for(auto &b : meta_buffer_pool) { b = new MT(); meta_buffer_pool_set.insert(b); }
    if constexpr (!C_VOID<DT>) {
      data_buffer_pool.resize(MSHR, nullptr);
      for(auto &b : data_buffer_pool) { b = new DT(); data_buffer_pool_set.insert(b); }
    }
  }

  virtual ~CacheSkewed() {
    delete CacheMonitorSupport::monitors;
    if (!data_buffer_pool_set.empty()) for(auto b: data_buffer_pool_set) delete b;
    for(auto b: meta_buffer_pool_set) delete b;
    if constexpr (P>1) delete loc_random;
  }

  virtual std::tuple<int, int, int> size() const { return std::make_tuple(P, 1ul<<IW, NW); }

  virtual bool hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w ) {
    for(*ai=0; *ai<P; (*ai)++) {
      *s = indexer.index(addr, *ai);
      if(arrays[*ai]->hit(addr, *s, w))
        return true;
    }
    return false;
  }

  virtual std::pair<CMMetadataBase *, CMDataBase *> access_line(uint32_t ai, uint32_t s, uint32_t w) {
    auto meta = static_cast<CMMetadataBase *>(arrays[ai]->get_meta(s, w));
    if constexpr (!C_VOID<DT>)
      return std::make_pair(meta, w < NW ? arrays[ai]->get_data(s, w) : nullptr);
    else
      return std::make_pair(meta, nullptr);
  }

  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, unsigned int genre = 0) {
    if constexpr (P==1) *ai = 0;
    else                *ai = ((*loc_random)() % P);
    *s = indexer.index(addr, *ai);
    replacer[*ai].replace(*s, w);
  }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, const CMMetadataBase * meta, const CMDataBase *data, uint64_t *delay) {
    if(ai < P) replacer[ai].access(s, w, false);
    if constexpr (EnMon || !C_VOID<DLY>) monitors->hook_read(addr, ai, s, w, hit, meta, data, delay);
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool is_release, const CMMetadataBase * meta, const CMDataBase *data, uint64_t *delay) {
    if(ai < P) replacer[ai].access(s, w, is_release);
    if constexpr (EnMon || !C_VOID<DLY>) monitors->hook_write(addr, ai, s, w, hit, meta, data, delay);
  }

  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, const CMMetadataBase * meta, const CMDataBase *data, uint64_t *delay) {
    if(ai < P && hit && evict) replacer[ai].invalid(s, w);
    if constexpr (EnMon || !C_VOID<DLY>) monitors->hook_manage(addr, ai, s, w, hit, evict, writeback, meta, data, delay);
  }

  virtual CMDataBase *data_copy_buffer() {
    if (data_buffer_pool_set.empty()) return nullptr;
    uint16_t index;
    if constexpr (EnMT) { // when multithread
      while(true) {
        index = data_buffer_state.read();
        if(index == 0) { data_buffer_state.wait(); continue; } // pool empty
        if(!data_buffer_state.swap(index, index-1)) continue;  // atomic write
        index--; break;
      }
    } else {
      assert(data_buffer_state > 0);
      index = --data_buffer_state;
    }
    return data_buffer_pool[index];
  }

  virtual void data_return_buffer(CMDataBase *buf) {
    if (!buf) return;
    if(data_buffer_pool_set.count(buf)) { // only recycle previous allocated buffer
      uint16_t index;
      if constexpr (EnMT) { // when multithread
        while(true) {
          index = data_buffer_state.read();
          if(data_buffer_state.swap(index, index+1, true)) break;  // atomic write
        }
      } else
        index = data_buffer_state++;
      data_buffer_pool[index] = buf;
    }
  }

  virtual CMMetadataBase *meta_copy_buffer() {
    uint16_t index;
    if constexpr (EnMT) { // when multithread
      while(true) {
        index = meta_buffer_state.read();
        if(index == 0) { meta_buffer_state.wait(); continue; } // pool empty
        if(!meta_buffer_state.swap(index, index-1)) continue;  // atomic write
        index--; break;
      }
    } else {
      assert(meta_buffer_state > 0);
      index = --meta_buffer_state;
    }
    return meta_buffer_pool[index];
  }

  virtual void meta_return_buffer(CMMetadataBase *buf) {
    if(meta_buffer_pool_set.count(buf)) { // only recycle previous allocated buffer
      uint16_t index;
      if constexpr (EnMT) { // when multithread
        while(true) {
          index = meta_buffer_state.read();
          if(meta_buffer_state.swap(index, index+1, true)) break;  // atomic write
        }
      } else
        index = meta_buffer_state++;
      meta_buffer_pool[index] = buf;
    }
  }

  virtual bool query_coloc(uint64_t addrA, uint64_t addrB){
    for(int i=0; i<P; i++) 
      if(indexer.index(addrA, i) == indexer.index(addrB, i)) 
        return true;
    return false;
  }

  virtual void query_fill_loc(LocInfo *loc, uint64_t addr) {
    for(int i=0; i<P; i++){
      loc->insert(LocIdx(i, indexer.index(addr, i)), LocRange(0, NW-1));
    }
  }
};

// Normal set-associative cache
// IW: index width, NW: number of ways
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon>
using CacheNorm = CacheSkewed<IW, NW, 1, MT, DT, IDX, RPC, DLY, EnMon>;

// Dynamic-Randomized support for CacheBase
class CacheBaseRemapSupport
{
public:
  CacheBaseRemapSupport() {}
  virtual ~CacheBaseRemapSupport() {}

  virtual std::vector<int>* get_SPtr() = 0;
  virtual CMDataBase *remap_data_copy_buffer() = 0;
  virtual void remap_data_return_buffer(CMDataBase *buf) = 0;
  virtual CMMetadataBase *remap_meta_copy_buffer() = 0;
  virtual void remap_meta_return_buffer(CMMetadataBase *buf) = 0;
  virtual void rotate_indexer() = 0;
  virtual void next_replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, unsigned int genre = 0) = 0;
};

// Dynamic-Randomized Skewed Cache 
// IW: index width, NW: number of ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, int P, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon, bool EF = true, bool EnMT = false, int MSHR = 4>
  requires C_DERIVE<MT, MetadataBroadcastBase, RemapMetadataSupport>
        && C_DERIVE_OR_VOID<DT, CMDataBase>
        && C_DERIVE<IDX, IndexFuncBase, IndexRemapSupport>
        && C_DERIVE_OR_VOID<DLY, DelayBase>
class CacheRemap : public CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon, EF, EnMT, MSHR>, 
                      public CacheBaseRemapSupport
{
  typedef CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon> CacheT;
  typedef typename std::conditional<EnMT, AtomicVar<uint16_t>, uint16_t>::type buffer_state_t;

protected:
  using CacheT::indexer;
  using CacheT::arrays;
  using CacheT::replacer;
  using CacheT::loc_random;

  std::unordered_set<CMDataBase *> remap_data_buffer_pool_set;
  std::vector<CMDataBase *>        remap_data_buffer_pool;
  buffer_state_t                   remap_data_buffer_state;

  std::unordered_set<CMMetadataBase *> remap_meta_buffer_pool_set;
  std::vector<CMMetadataBase *>        remap_meta_buffer_pool;
  buffer_state_t                       remap_meta_buffer_state;

  std::vector<int> SPtr;
public:
  CacheRemap(std::string name = "", unsigned int extra_par = 0, unsigned int extra_way = 0) 
  : CacheT(name, extra_par, extra_way), remap_data_buffer_state(2), remap_meta_buffer_pool(2), remap_meta_buffer_state(2), SPtr(1ul<<IW, -1){
    // allocate buffer pools
    // for single thread simulator, we assume a maximum of 2 buffers should be enough
    remap_meta_buffer_pool.resize(2, nullptr);
    for(auto &b : remap_meta_buffer_pool) { b = new MT(); remap_meta_buffer_pool_set.insert(b); }
    if constexpr (!C_VOID<DT>) {
      remap_data_buffer_pool.resize(2, nullptr);
      for(auto &b : remap_data_buffer_pool) { b = new DT(); remap_data_buffer_pool_set.insert(b); }
    }
  }
  virtual ~CacheRemap() {
    if (!remap_data_buffer_pool_set.empty()) for(auto b: remap_data_buffer_pool_set) delete b;
    for(auto b: remap_meta_buffer_pool_set) delete b;
  }

  virtual std::vector<int>* get_SPtr() { return &SPtr;}

  virtual CMDataBase *remap_data_copy_buffer() {
    if (remap_data_buffer_pool_set.empty()) return nullptr;
    uint16_t index;
    if constexpr (EnMT) { // when multithread
      while(true) {
        index = remap_data_buffer_state.read();
        if(index == 0) { remap_data_buffer_state.wait(); continue; } // pool empty
        if(!remap_data_buffer_state.swap(index, index-1)) continue;  // atomic write
        index--; break;
      }
    } else {
      assert(remap_data_buffer_state > 0);
      index = --remap_data_buffer_state;
    }
    return remap_data_buffer_pool[index];
  }

  virtual void remap_data_return_buffer(CMDataBase *buf) {
    if (!buf) return;
    if(remap_data_buffer_pool_set.count(buf)) { // only recycle previous allocated buffer
      uint16_t index;
      if constexpr (EnMT) { // when multithread
        while(true) {
          index = remap_data_buffer_state.read();
          if(remap_data_buffer_state.swap(index, index+1, true)) break;  // atomic write
        }
      } else
        index = remap_data_buffer_state++;
      remap_data_buffer_pool[index] = buf;
    }
  }

  virtual CMMetadataBase *remap_meta_copy_buffer() {
    uint16_t index;
    if constexpr (EnMT) { // when multithread
      while(true) {
        index = remap_meta_buffer_state.read();
        if(index == 0) { remap_meta_buffer_state.wait(); continue; } // pool empty
        if(!remap_meta_buffer_state.swap(index, index-1)) continue;  // atomic write
        index--; break;
      }
    } else {
      assert(remap_meta_buffer_state > 0);
      index = --remap_meta_buffer_state;
    }
    return remap_meta_buffer_pool[index];
  }

  virtual void remap_meta_return_buffer(CMMetadataBase *buf) {
    if(remap_meta_buffer_pool_set.count(buf)) { // only recycle previous allocated buffer
      uint16_t index;
      if constexpr (EnMT) { // when multithread
        while(true) {
          index = remap_meta_buffer_state.read();
          if(remap_meta_buffer_state.swap(index, index+1, true)) break;  // atomic write
        }
      } else
        index = remap_meta_buffer_state++;
      remap_meta_buffer_pool[index] = buf;
    }
  }

  virtual bool hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w ) {
    for(*ai=0; *ai<P; (*ai)++) {
      *s = indexer.index(addr, *ai);
      if(SPtr[*ai] < 0){
        if (arrays[*ai]->hit(addr, *s, w)) return true;
      }
      else{
        if(*s >= SPtr[*ai]){
          if (arrays[*ai]->hit(addr, *s, w)) return true;
          
          *s = indexer.next_index(addr, *ai);
          if (arrays[*ai]->hit(addr, *s, w)) return true;
        }
        else{
          *s = indexer.next_index(addr, *ai);
          if (arrays[*ai]->hit(addr, *s, w)) return true;
        }
      }
    }
    return false;
  }

  virtual void next_replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, unsigned int genre = 0) {
    if constexpr (P==1) *ai = 0;
    else                *ai = ((*loc_random)() % P);
    *s = indexer.next_index(addr, *ai);
    replacer[*ai].replace(*s, w);
  }

  virtual void rotate_indexer(){
    indexer.rotate_indexer();
  }
};

#endif
