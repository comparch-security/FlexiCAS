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
public:
  virtual ~CacheArrayBase() = default;

  virtual bool hit(uint64_t addr, uint32_t s, uint32_t *w) const = 0;
  virtual CMMetadataCommon * get_meta(uint32_t s, uint32_t w) = 0;
  virtual CMDataBase * get_data(uint32_t s, uint32_t w) = 0;

  // support multithread
  virtual void set_mt_state(uint32_t s, uint16_t prio) = 0;   // preserve a cache set according to transaction priority
  virtual bool check_mt_state(uint32_t s, uint16_t prio) = 0; // check priority before continuing remaining work on a cache set
  virtual void wait_mt_state(uint32_t s, uint16_t prio) = 0; // wait for priority before continuing remaining work on a cache set
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
  std::vector<DT *> data;     // data array, could be null
  const unsigned int way_num;
  std::vector<AtomicVar<uint16_t> > cache_set_state;  // record current transactions for multithread support

public:
  static constexpr uint32_t nset = 1ul<<IW;  // number of sets

  CacheArrayNorm(unsigned int extra_way = 0) : way_num(NW+extra_way){
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

  virtual ~CacheArrayNorm() override {
    for(auto m:meta) delete m;
    if constexpr (!C_VOID<DT>) for(auto d:data) delete d;
  }

  virtual bool hit(uint64_t addr, uint32_t s, uint32_t *w) const override {
    for(unsigned int i=0; i<way_num; i++)
      if(meta[s*way_num + i]->match(addr)) {
        *w = i;
        return true;
      }
    return false;
  }

  virtual CMMetadataCommon * get_meta(uint32_t s, uint32_t w) override { return meta[s*way_num + w]; }
  virtual CMDataBase * get_data(uint32_t s, uint32_t w) {
    if constexpr (C_VOID<DT>) return nullptr;
    else                      return data[s*NW + w];
  }

  virtual __always_inline void set_mt_state(uint32_t s, uint16_t prio) override {
    if constexpr (EnMT) {
      while(true) {
        auto state = cache_set_state[s].read();
        if(prio <= state) { cache_set_state[s].wait(); continue; }
        if(cache_set_state[s].swap(state, state|prio)) break;
      }
    }
  }

  virtual __always_inline bool check_mt_state(uint32_t s, uint16_t prio) override {
    if constexpr (EnMT) {
      auto prio_upper = (prio << 1) - 1;
      auto state = cache_set_state[s].read();
      assert(state >= prio);
      return prio_upper >= state;
    } else
      return true;
  }

  virtual __always_inline void wait_mt_state(uint32_t s, uint16_t prio) override {
    if constexpr (EnMT) {
      auto prio_upper = (prio << 1) - 1;
      while(true) {
        auto state = cache_set_state[s].read();
        assert(state >= prio);
        if(prio_upper >= state) break;
        cache_set_state[s].wait();
      }
    }
  }

  virtual __always_inline void reset_mt_state(uint32_t s, uint16_t prio) override {
    if constexpr (EnMT) {
      while(true) {
        auto state = cache_set_state[s].read();
        assert(state == (state | prio));
        if(cache_set_state[s].swap(state, state & (~prio), true)) break;
      }
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
                   uint32_t *s, uint32_t *w,
                   uint16_t prio, // transaction priority
                   bool check_and_set // whether to check and set the priority if hit
                   ) = 0;

  __always_inline bool hit(uint64_t addr) {
    uint32_t ai, s, w;
    return hit(addr, &ai, &s, &w, 0, false);
  }

  virtual bool replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, uint16_t prio, unsigned int genre = 0) = 0;

  __always_inline CMMetadataCommon *access(uint32_t ai, uint32_t s, uint32_t w) { return arrays[ai]->get_meta(s, w); }
  __always_inline CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w) { return arrays[ai]->get_data(s, w); }

  // methods for supporting multithread execution
  virtual CMDataBase *data_copy_buffer() = 0;               // allocate a copy buffer, needed by exclusive cache with extended meta
  virtual void data_return_buffer(CMDataBase *buf) = 0;     // return a copy buffer, used to detect conflicts in copy buffer
  virtual CMMetadataBase *meta_copy_buffer() = 0;           // allocate a copy buffer, needed by exclusive cache with extended meta
  virtual void meta_return_buffer(CMMetadataBase *buf) = 0; // return a copy buffer, used to detect conflicts in copy buffer
  __always_inline void set_mt_state(uint32_t ai, uint32_t s, uint16_t prio)   { arrays[ai]->set_mt_state(s, prio);   }
  __always_inline bool check_mt_state(uint32_t ai, uint32_t s, uint16_t prio) { return arrays[ai]->check_mt_state(s, prio); }
  __always_inline void wait_mt_state(uint32_t ai, uint32_t s, uint16_t prio)  { arrays[ai]->wait_mt_state(s, prio);  }
  __always_inline void reset_mt_state(uint32_t ai, uint32_t s, uint16_t prio) { arrays[ai]->reset_mt_state(s, prio); }

  virtual std::tuple<int, int, int> size() const = 0;           // return the size parameters of the cache
  uint32_t get_id() const { return id; }
  const std::string& get_name() const { return name;} 

  // access both meta and data in one function call
  virtual std::pair<CMMetadataBase *, CMDataBase *> access_line(uint32_t ai, uint32_t s, uint32_t w) = 0;

  virtual void replace_read(uint32_t ai, uint32_t s, uint32_t w, bool prefetch, bool genre = false) = 0;
  virtual void replace_write(uint32_t ai, uint32_t s, uint32_t w, bool demand_acc, bool genre = false) = 0;
  virtual void replace_manage(uint32_t ai, uint32_t s, uint32_t w, bool hit, uint32_t evict, bool genre = false) = 0;

  virtual bool query_coloc(uint64_t addrA, uint64_t addrB) = 0;
  virtual LocInfo query_loc(uint64_t addr) { return LocInfo(id, this, addr); }
  virtual void query_fill_loc(LocInfo *loc, uint64_t addr) = 0;
};

// Skewed Cache
// IW: index width, NW: number of ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
// EnMT: enable multithread, MSHR: maximal number of transactions on the fly
template<int IW, int NW, int P, typename MT, typename DT, typename IDX, typename RPC, typename DLY,
         bool EnMon, bool EnMT = false, int MSHR = 4>
  requires C_DERIVE<MT, CMMetadataBase> && C_DERIVE_OR_VOID<DT, CMDataBase> &&
           C_DERIVE<IDX, IndexFuncBase> && C_DERIVE_OR_VOID<DLY, DelayBase> &&
           (MSHR >= 2) // 2 buffers are required even for single-thread simulation
class CacheSkewed : public CacheBase
{
protected:
  IDX indexer;      // index resolver
  RPC replacer[P];  // replacer
  RandomGen<uint32_t> * loc_random = nullptr; // a local randomizer for better thread parallelism

  std::unordered_set<CMDataBase *> data_buffer_pool_set;
  std::vector<CMDataBase *>        data_buffer_pool;
  uint16_t                         data_buffer_state = MSHR;
  std::mutex                       data_buffer_mutex;
  std::condition_variable          data_buffer_cv;

  std::unordered_set<CMMetadataBase *> meta_buffer_pool_set;
  std::vector<CMMetadataBase *>        meta_buffer_pool;
  uint16_t                             meta_buffer_state = MSHR;
  std::mutex                           meta_buffer_mutex;
  std::condition_variable              meta_buffer_cv;

  virtual void replace_choose_set(uint64_t addr, uint32_t *ai, uint32_t *s, unsigned int) {
    if constexpr (P==1) *ai = 0;
    else                *ai = ((*loc_random)() % P);
    *s = indexer.index(addr, *ai);
  }

public:
  CacheSkewed(std::string name, unsigned int extra_par = 0, unsigned int extra_way = 0)
    : CacheBase(name), meta_buffer_pool(MSHR)
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

  virtual ~CacheSkewed() override {
    delete CacheMonitorSupport::monitors;
    if (!data_buffer_pool_set.empty()) for(auto b: data_buffer_pool_set) delete b;
    for(auto b: meta_buffer_pool_set) delete b;
    if constexpr (P>1) delete loc_random;
  }

  virtual std::tuple<int, int, int> size() const override { return std::make_tuple(P, 1ul<<IW, NW); }

  virtual bool hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, uint16_t prio, bool check_and_set) override {
    for(*ai=0; *ai<P; (*ai)++) {
      *s = indexer.index(addr, *ai);
      if(EnMT && check_and_set) this->set_mt_state(*ai, *s, prio);
      if(arrays[*ai]->hit(addr, *s, w)) return true;
      if(EnMT && check_and_set) this->reset_mt_state(*ai, *s, prio);
    }
    return false;
  }

  virtual std::pair<CMMetadataBase *, CMDataBase *> access_line(uint32_t ai, uint32_t s, uint32_t w) override {
    auto meta = static_cast<CMMetadataBase *>(arrays[ai]->get_meta(s, w));
    if constexpr (!C_VOID<DT>)
      return std::make_pair(meta, w < NW ? arrays[ai]->get_data(s, w) : nullptr);
    else
      return std::make_pair(meta, nullptr);
  }

  virtual bool replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, uint16_t prio, unsigned int genre = 0) override {
    replace_choose_set(addr, ai, s, genre);
    if(EnMT) {
      this->set_mt_state(*ai, *s, prio);
      // double check the miss status
      if(CacheBase::hit(addr)) { // the exact cache block is re-inserted by other transactions
        this->reset_mt_state(*ai, *s, prio);
        return false;
      }
    }
    replacer[*ai].replace(*s, w);
    return true;
  }

  __always_inline void relocate(uint64_t addr, CMMetadataBase *s_meta, CMMetadataBase *d_meta) {
    d_meta->init(addr);
    d_meta->copy(s_meta);
    s_meta->to_clean();
    s_meta->to_invalid();
  }

  __always_inline void relocate(uint64_t addr, CMMetadataBase *s_meta, CMMetadataBase *d_meta, CMDataBase *s_data, CMDataBase *d_data) {
    relocate(addr, s_meta, d_meta);
    if(s_data) d_data->copy(s_data);
  }

  __always_inline std::pair<MT*, uint64_t> relocate(uint32_t s_ai, uint32_t s_s, uint32_t s_w, uint32_t d_ai, uint32_t d_s, uint32_t d_w) {
    if constexpr (C_VOID<DT>) {
      auto s_meta = static_cast<CMMetadataBase *>(this->access(s_ai, s_s, s_w));
      auto d_meta = static_cast<CMMetadataBase *>(this->access(d_ai, d_s, d_w));
      auto addr = s_meta->addr(s_s);
      relocate(addr, s_meta, d_meta);
      return std::make_pair(static_cast<MT *>(d_meta), addr);
    } else {
      auto [s_meta, s_data] = this->access_line(s_ai, s_s, s_w);
      auto [d_meta, d_data] = this->access_line(d_ai, d_s, d_w);
      auto addr = s_meta->addr(s_s);
      relocate(addr, s_meta, d_meta); d_data->copy(s_data);
      return std::make_pair(static_cast<MT *>(d_meta), addr);
    }
  }

  __always_inline void swap(uint64_t a_addr, uint64_t b_addr, CMMetadataBase *a_meta, CMMetadataBase *b_meta, CMDataBase *a_data, CMDataBase *b_data) {
    auto buffer_meta = meta_copy_buffer();
    auto buffer_data = a_data ? data_copy_buffer() : nullptr;
    relocate(a_addr, a_meta, buffer_meta, a_data, buffer_data);
    relocate(b_addr, b_meta, a_meta, b_data, a_data);
    relocate(a_addr, buffer_meta, b_meta, buffer_data, b_data);
    meta_return_buffer(buffer_meta);
    data_return_buffer(buffer_data);
  }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, const CMMetadataBase * meta, const CMDataBase *data, uint64_t *delay) override {
    if constexpr (EnMon || !C_VOID<DLY>) monitors->hook_read(addr, ai, s, w, (ai < P ? replacer[ai].eviction_rank(s, w) : -1), hit, meta, data, delay);
  }

  virtual void replace_read(uint32_t ai, uint32_t s, uint32_t w, bool prefetch, bool genre = false) override {
    if(ai < P) replacer[ai].access(s, w, true, prefetch);
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, const CMMetadataBase * meta, const CMDataBase *data, uint64_t *delay) override {
    if constexpr (EnMon || !C_VOID<DLY>) monitors->hook_write(addr, ai, s, w, (ai < P ? replacer[ai].eviction_rank(s, w) : -1), hit, meta, data, delay);
  }

  virtual void replace_write(uint32_t ai, uint32_t s, uint32_t w, bool demand_acc, bool genre = false) override {
    if(ai < P) replacer[ai].access(s, w, demand_acc, false);
  }

  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint32_t evict, bool writeback, const CMMetadataBase * meta, const CMDataBase *data, uint64_t *delay) override {
    if constexpr (EnMon || !C_VOID<DLY>) monitors->hook_manage(addr, ai, s, w, (ai < P ? replacer[ai].eviction_rank(s, w) : -1), hit, evict, writeback, meta, data, delay);
  }

  virtual void replace_manage(uint32_t ai, uint32_t s, uint32_t w, bool hit, uint32_t evict, bool genre = false) override {
    if(ai < P && hit && evict) replacer[ai].invalid(s, w, evict == 2);
  }

  virtual CMDataBase *data_copy_buffer() override {
    if (data_buffer_pool_set.empty()) return nullptr;
    if constexpr (EnMT) {
      std::unique_lock lk(data_buffer_mutex);
      while(data_buffer_state == 0) data_buffer_cv.wait(lk);
      return data_buffer_pool[--data_buffer_state];
    } else {
      assert(data_buffer_state > 0);
      return data_buffer_pool[--data_buffer_state];
    }
  }

  virtual void data_return_buffer(CMDataBase *buf) override {
    if (!buf) return;
    if(data_buffer_pool_set.count(buf)) { // only recycle previous allocated buffer
      if constexpr (EnMT) {
        {
          std::lock_guard lk(data_buffer_mutex);
          data_buffer_pool[data_buffer_state] = buf;
          data_buffer_state++;
        }
        data_buffer_cv.notify_one();
      } else {
        data_buffer_pool[data_buffer_state] = buf;
        data_buffer_state++;
      }
    }
  }

  virtual CMMetadataBase *meta_copy_buffer() override {
    if (meta_buffer_pool_set.empty()) return nullptr;
    if constexpr (EnMT) {
      std::unique_lock lk(meta_buffer_mutex);
      while(meta_buffer_state == 0) meta_buffer_cv.wait(lk);
      return meta_buffer_pool[--meta_buffer_state];
    } else {
      assert(meta_buffer_state > 0);
      return meta_buffer_pool[--meta_buffer_state];
    }
  }

  virtual void meta_return_buffer(CMMetadataBase *buf) override {
    if(meta_buffer_pool_set.count(buf)) { // only recycle previous allocated buffer
      if constexpr (EnMT) {
        {
          std::lock_guard lk(meta_buffer_mutex);
          meta_buffer_pool[meta_buffer_state] = buf;
          meta_buffer_state++;
        }
        meta_buffer_cv.notify_one();
      } else {
        meta_buffer_pool[meta_buffer_state] = buf;
        meta_buffer_state++;
      }
    }
  }

  virtual bool query_coloc(uint64_t addrA, uint64_t addrB) override {
    for(int i=0; i<P; i++) 
      if(indexer.index(addrA, i) == indexer.index(addrB, i)) 
        return true;
    return false;
  }

  virtual void query_fill_loc(LocInfo *loc, uint64_t addr) override {
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
template<int IW, int NW, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon, bool EnMT = false, int MSHR = 4>
using CacheNorm = CacheSkewed<IW, NW, 1, MT, DT, IDX, RPC, DLY, EnMon, EnMT, MSHR>;

#endif
