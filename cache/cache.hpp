#ifndef CM_CACHE_CACHE_HPP
#define CM_CACHE_CACHE_HPP

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <set>
#include <map>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>


#include "util/random.hpp"
#include "util/monitor.hpp"
#include "cache/index.hpp"
#include "cache/replace.hpp"
#include "util/log.hpp"
#include "util/common.hpp"
#include "util/util.hpp"


class CMMetadataBase
{
public:
  // implement a totally useless base class
  virtual bool match(uint64_t addr) const { return false; } // wether an address match with this block
  virtual bool match_b(uint64_t addr) const { return false; }
  virtual void reset() {}                                   // reset the metadata
  virtual void init(uint64_t addr) = 0;                     // initialize the meta for addr
  virtual void init_buffer(uint64_t addr) = 0;              // initialize the buffer addr
  virtual uint64_t addr(uint32_t s) const = 0;              // assemble the block address from the metadata
  virtual uint64_t addr_buffer() const = 0;                 // get buffer_addr
  virtual void sync(int32_t coh_id) = 0;                    // sync after probe

  virtual void to_invalid() = 0;                            // change state to invalid
  virtual void to_shared(int32_t coh_id) = 0;               // change to shared
  virtual void to_modified(int32_t coh_id) = 0;             // change to modified
  virtual void to_owned(int32_t coh_id) = 0;                // change to owned
  virtual void to_exclusive(int32_t coh_id) = 0;            // change to exclusive
  virtual void to_dirty() = 0;                              // change to dirty
  virtual void to_clean() = 0;                              // change to clean
  virtual void to_extend() = 0;                             // change to extend directory meta
  virtual void to_full_b() = 0;                             // change buffer to full
  virtual void to_empty_b() = 0;                            // change buffer to empty
  virtual bool is_valid() const { return false; }
  virtual bool is_shared() const { return false; }
  virtual bool is_modified() const { return false; }
  virtual bool is_owned() const { return false; }
  virtual bool is_exclusive() const { return false; }
  virtual bool is_dirty() const { return false; }
  virtual bool is_extend() const { return false; }

  virtual bool is_full_b() const { return false; }

  virtual void copy(const CMMetadataBase *meta) = 0;        // copy the content of meta

  virtual ~CMMetadataBase() {}
};

// TODO : support owner
class  MetadataDirectorySupportBase
{
protected:
  uint64_t sharer = 0;
  virtual void add_sharer(int32_t coh_id) = 0;
  virtual void clean_sharer() = 0;
  virtual void delete_sharer(int32_t coh_id) = 0;
  virtual bool is_sharer(int32_t coh_id) const = 0;
public:
  virtual uint64_t get_sharer()= 0;
  virtual void set_sharer(uint64_t c_sharer) = 0;
  MetadataDirectorySupportBase() : sharer(0) {}
  virtual ~MetadataDirectorySupportBase() {}

};

class CMDataBase
{
public:
  // implement a totally useless base class
  virtual void reset() {} // reset the data block, normally unnecessary
  virtual uint64_t read(unsigned int index) const { return 0; } // read a 64b data
  virtual void write(unsigned int index, uint64_t wdata, uint64_t wmask) {} // write a 64b data with wmask
  virtual void write(uint64_t *wdata) {} // write the whole cache block
  virtual void copy(const CMDataBase *block) = 0; // copy the content of block

  virtual ~CMDataBase() {}
};

// typical 64B data block
class Data64B : public CMDataBase
{
protected:
  uint64_t data[8];

public:
  Data64B() : data{0} {}
  virtual ~Data64B() {}

  virtual void reset() { for(auto d:data) d = 0; }
  virtual uint64_t read(unsigned int index) const { return data[index]; }
  virtual void write(unsigned int index, uint64_t wdata, uint64_t wmask) { data[index] = (data[index] & (~wmask)) | (wdata & wmask); }
  virtual void write(uint64_t *wdata) { for(int i=0; i<8; i++) data[i] = wdata[i]; }
  virtual void copy(const CMDataBase *m_block) {
    auto block = static_cast<const Data64B *>(m_block);
    for(int i=0; i<8; i++) data[i] = block->data[i];
  }
};


//////////////// define cache array ////////////////////

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
  virtual std::vector<uint32_t> *get_status() = 0;
  virtual std::mutex* get_mutex() = 0;
  virtual std::condition_variable* get_cv() = 0;
  virtual std::mutex* get_cacheline_mutex(uint32_t s, uint32_t w) = 0;
  virtual std::set<uint64_t>* get_acquiring_set(uint32_t s) = 0;
  virtual std::mutex* get_acquiring_mutex(uint32_t s) = 0;
  virtual std::condition_variable* get_acquiring_cv(uint32_t s) = 0;

};

// normal set associative cache array
// IW: index width, NW: number of ways, MT: metadata type, DT: data type (void if not in use)
template<int IW, int NW, typename MT, typename DT,
         typename = typename std::enable_if<std::is_base_of<CMMetadataBase, MT>::value>::type, // MT <- CMMetadataBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
class CacheArrayNorm : public CacheArrayBase
{
protected:
  std::vector<MT *> meta;   // meta array
  std::vector<DT *> data;   // data array, could be null
  std::vector<std::mutex *> mutexs; // mutex array for meta
  std::vector<uint32_t> status; // record every set status

  std::vector<std::set<uint64_t> > acquiring_set; // record every set acquiring address to outer cache 
  std::vector<std::mutex *> acquiring_mutexs; // mutex for protecting acquiring set
  std::vector<std::condition_variable *> acquiring_cv; // cv for supporting acquiring set work
  std::mutex mtx; // mutex for status
  std::condition_variable cv;
  unsigned int extra_way = 0;

public:
  static constexpr uint32_t nset = 1ul<<IW;  // number of sets

  CacheArrayNorm(unsigned int extra_way = 0, std::string name = "") : CacheArrayBase(name), extra_way(extra_way){
    size_t meta_num = nset * (NW + extra_way);
    constexpr size_t data_num = nset * NW;
    meta.resize(meta_num);
    for(auto &m:meta) m = new MT();
    if constexpr (!std::is_void<DT>::value) {
      data.resize(data_num);
      for(auto &d:data) d = new DT();
    }
    status.resize(nset);
    for(int i = 0; i < nset; i++) status[i] = 0;
    
    mutexs.resize(meta_num);
    for(auto &t:mutexs) t = new std::mutex();

    acquiring_set.resize(nset);
    acquiring_mutexs.resize(nset);
    acquiring_cv.resize(nset);
    for(int i = 0; i < nset; i++) {
      acquiring_mutexs[i] = new std::mutex();
      acquiring_cv[i] = new std::condition_variable(); 
    }
  }

  virtual ~CacheArrayNorm() {
    for(auto m:meta) delete m;
    if constexpr (!std::is_void<DT>::value) for(auto d:data) delete d;
    for(auto t:mutexs) delete t;
    for(auto am : acquiring_mutexs) delete am;
    for(auto ac : acquiring_cv) delete ac;
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
    if constexpr (std::is_void<DT>::value) {
      return nullptr;
    } else
      return data[s*NW + w];
  }

  virtual std::vector<uint32_t> *get_status(){ return &status; }
  virtual std::mutex* get_mutex() { return &mtx; }
  virtual std::condition_variable* get_cv() { return &cv; }

  virtual std::mutex* get_cacheline_mutex(uint32_t s, uint32_t w) { return mutexs[s*(NW+extra_way) + w]; }

  virtual std::set<uint64_t>* get_acquiring_set(uint32_t s){
    return &acquiring_set[s];
  }
  virtual std::mutex* get_acquiring_mutex(uint32_t s){
    return acquiring_mutexs[s];
  }
  virtual std::condition_variable* get_acquiring_cv(uint32_t s){
    return acquiring_cv[s];
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

  CacheBase(std::string name) : id(UniqueID::new_id()), name(name) {}

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

  virtual bool hit_t(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, uint32_t value = 0x1, InnerProbeRecord *record = nullptr) = 0;
  

  virtual bool replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, unsigned int genre = 0) = 0;

  // virtual void withdraw_replace(uint32_t ai, uint32_t s, uint32_t w) = 0;

  // hook interface for replacer state update, Monitor and delay estimation
  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay, unsigned int genre = 0) = 0;
  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay, unsigned int genre = 0) = 0;
  // probe, invalidate and writeback
  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, uint64_t *delay, unsigned int genre = 0) = 0;

  virtual CMMetadataBase *access(uint32_t ai, uint32_t s, uint32_t w) {
    return arrays[ai]->get_meta(s, w);
  }

  virtual CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w) {
    return arrays[ai]->get_data(s, w);
  }

  // access both meta and data in one function call
  virtual std::pair<CMMetadataBase *, CMDataBase *> access_line(uint32_t ai, uint32_t s, uint32_t w) = 0;

  virtual bool query_coloc(uint64_t addrA, uint64_t addrB) = 0;

  virtual std::vector<uint32_t> *get_status(uint32_t ai){
    return arrays[ai]->get_status();
  }
  virtual std::mutex* get_mutex(uint32_t ai){
    return arrays[ai]->get_mutex();
  }
  virtual std::condition_variable* get_cv(uint32_t ai) {
    return arrays[ai]->get_cv();
  }

  virtual std::mutex* get_cacheline_mutex(uint32_t ai, uint32_t s, uint32_t w){
    return arrays[ai]->get_cacheline_mutex(s, w);
  }
  
  virtual uint32_t get_id() {
    return id;
  }

  virtual std::string get_name(){
    return name;
  }

  virtual void add_acquiring_addr(uint64_t addr, int32_t ai, uint32_t s) = 0;

  virtual void delete_acquiring_addr(uint64_t addr, int32_t ai, uint32_t s) = 0;

};

// Skewed Cache
// IW: index width, NW: number of ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, int P, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon,
         typename = typename std::enable_if<std::is_base_of<CMMetadataBase, MT>::value>::type,  // MT <- CMMetadataBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type, // DT <- CMDataBase or void
         typename = typename std::enable_if<std::is_base_of<IndexFuncBase, IDX>::value>::type,  // IDX <- IndexFuncBase
         typename = typename std::enable_if<std::is_base_of<ReplaceFuncBase, RPC>::value>::type,  // RPC <- ReplaceFuncBase
         typename = typename std::enable_if<std::is_base_of<DelayBase, DLY>::value || std::is_void<DLY>::value>::type>  // DLY <- DelayBase or void
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

  virtual bool probe_waiting(uint64_t addr, int32_t ai, uint32_t s, uint32_t *w){
    auto aset = arrays[ai]->get_acquiring_set(s);
    auto amtx = arrays[ai]->get_acquiring_mutex(s);
    auto acv  = arrays[ai]->get_acquiring_cv(s);
    std::unique_lock lk(*amtx, std::defer_lock);
    bool hit = false;
    SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
    mutex: %p, probe check(set lock)\n", get_time(), database.get_id(get_thread_id), addr, \
    get_name().c_str(), ai, s, amtx);
    WAIT_PACV(acv, lk, addr, aset,"time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
    probe check, get cv\n", get_time(), database.get_id(get_thread_id), addr, get_name().c_str(), amtx);
    for(*w = 0; *w < NW; (*w)++){
      if(access(ai, s, *w)->match(addr)){
        hit = true;
        break;
      }
    }
    UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
    mutex: %p, probe check(unset lock)\n", get_time(), database.get_id(get_thread_id), addr, \
    get_name().c_str(), ai, s, amtx);
    acv->notify_all();
    return hit;
  }

  virtual void add_acquiring_addr(uint64_t addr, int32_t ai, uint32_t s){
    auto aset = arrays[ai]->get_acquiring_set(s);
    auto amtx = arrays[ai]->get_acquiring_mutex(s);
    auto acv  = arrays[ai]->get_acquiring_cv(s);
    std::unique_lock lk(*amtx, std::defer_lock);
    SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
    mutex: %p, add acquiring addr(set lock)\n", get_time(), database.get_id(get_thread_id), addr, \
    get_name().c_str(), ai, s, amtx);
    aset->insert(addr);
    UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
    mutex: %p, add acquiring addr(unset lock)\n", get_time(), database.get_id(get_thread_id), addr, \
    get_name().c_str(), ai, s, amtx);
    acv->notify_all();
    return ;
  }

  virtual void delete_acquiring_addr(uint64_t addr, int32_t ai, uint32_t s){
    auto aset = arrays[ai]->get_acquiring_set(s);
    auto amtx = arrays[ai]->get_acquiring_mutex(s);
    auto acv  = arrays[ai]->get_acquiring_cv(s);
    std::unique_lock lk(*amtx, std::defer_lock);
    SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
    mutex: %p, delete acquiring addr(set lock)\n", get_time(), database.get_id(get_thread_id), addr, \
    get_name().c_str(), ai, s, amtx);
    aset->erase(addr);
    UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
    mutex: %p, delete acquiring addr(unset lock)\n", get_time(), database.get_id(get_thread_id), addr, \
    get_name().c_str(), ai, s, amtx);
    acv->notify_all();
    return ;
  }

  virtual bool hit_t(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, uint32_t value = 0x1, InnerProbeRecord *record = nullptr){
    for(*ai=0; *ai<P; (*ai)++){
      *s = indexer.index(addr, *ai);
      auto status = get_status(*ai);
      auto mtx    = get_mutex(*ai);
      auto cv     = get_cv(*ai);
      std::unique_lock lk(*mtx, std::defer_lock);
      uint32_t ss = *s;
      if(value == 0x10) { // probe
        auto aset = arrays[*ai]->get_acquiring_set(*s);
        auto amtx = arrays[*ai]->get_acquiring_mutex(*s);
        auto acv  = arrays[*ai]->get_acquiring_cv(*s);
        SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
        mutex: %p, probe check acquiring set(set lock)\n", get_time(), database.get_id(get_thread_id), addr, \
        get_name().c_str(), *ai, *s, amtx);
        WAIT_PACV(acv, lk, addr, aset,"time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
        probe check check acquiring set, get cv\n", get_time(), database.get_id(get_thread_id), addr, get_name().c_str(), amtx);

        UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
        mutex: %p, probe check(unset lock)\n", get_time(), database.get_id(get_thread_id), addr, \
        get_name().c_str(), *ai, *s, amtx);

        SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
        mutex: %p, check hit(set lock)\n", get_time(), database.get_id(get_thread_id), addr, \
        get_name().c_str(), *ai, *s, mtx);
        WAIT_CV(cv, lk, ss, status, value, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
        check hit, get cv\n", get_time(), database.get_id(get_thread_id), addr, get_name().c_str(), mtx);
        (*status)[*s] |= value;
        UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
        mutex: %p, check hit(unset lock)\n",get_time(), database.get_id(get_thread_id), addr, \
        get_name().c_str(), *ai, *s, mtx);
        acv->notify_all();
      }else{
        SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
        mutex: %p, check hit(set lock)\n", get_time(), database.get_id(get_thread_id), addr, \
        get_name().c_str(), *ai, *s, mtx);
        WAIT_CV_WITH_RECORD(cv, lk, ss, status, value, addr, record, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, mutex: %p, \
        check hit, get cv\n", get_time(), database.get_id(get_thread_id), addr, get_name().c_str(), mtx);
        if(value == 0x1) add_acquiring_addr(addr, *ai, *s);
        (*status)[*s] |= value;
        UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
        mutex: %p, check hit(unset lock)\n",get_time(), database.get_id(get_thread_id), addr, \
        get_name().c_str(), *ai, *s, mtx);
      }
      for(*w=0; *w<NW; (*w)++){
        if(access(*ai, *s, *w)->match(addr)) return true;
      }
      SET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
      mutex: %p, check hit(set lock), miss on ai\n", get_time(), database.get_id(get_thread_id), addr, \
      get_name().c_str(), *ai, *s, mtx);
      (*status)[*s] &= ~(value);
      UNSET_LOCK(lk, "time : %lld, thread : %d, addr: 0x%-7lx,  name: %s, ai:%d, s:%d \
      mutex: %p, check hit(unset lock), miss on ai\n",get_time(), database.get_id(get_thread_id), addr, \
      get_name().c_str(), *ai, *s, mtx);
      cv->notify_all();
    }
    return false;
  }

  virtual std::pair<CMMetadataBase *, CMDataBase *> access_line(uint32_t ai, uint32_t s, uint32_t w) {
    auto meta = arrays[ai]->get_meta(s, w);
    if constexpr (!std::is_void<DT>::value)
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
    replacer[ai].access(s, w);
    if constexpr (EnMon || !std::is_void<DLY>::value) monitors->hook_read(addr, ai, s, w, hit, delay);
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay, unsigned int genre = 0) {
    replacer[ai].access(s, w);
    if constexpr (EnMon || !std::is_void<DLY>::value) monitors->hook_write(addr, ai, s, w, hit, delay);
  }

  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, uint64_t *delay, unsigned int genre = 0) {
    if(hit && evict) replacer[ai].invalid(s, w);
    if constexpr (EnMon || !std::is_void<DLY>::value) monitors->hook_manage(addr, ai, s, w, hit, evict, writeback, delay);
  }

  virtual bool query_coloc(uint64_t addrA, uint64_t addrB){
    for(int i=0; i<P; i++) 
      if(indexer.index(addrA, i) == indexer.index(addrB, i)) 
        return true;
    return false;
  }

  // virtual void withdraw_replace(uint32_t ai, uint32_t s, uint32_t w) {
  //   replacer[ai].withdraw_replace(s, w);
  // }
};

// Normal set-associative cache
// IW: index width, NW: number of ways
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon>
using CacheNorm = CacheSkewed<IW, NW, 1, MT, DT, IDX, RPC, DLY, EnMon>;

#endif
