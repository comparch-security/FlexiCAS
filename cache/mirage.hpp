#ifndef CM_CACHE_MIRAGE_MSI_HPP
#define CM_CACHE_MIRAGE_MSI_HPP

#include <cassert>
#include <type_traits>
#include <stack>
#include "cache/coherence.hpp"

// MirageMSI protocol
class MirageMSIPolicy {
  // definition of command:
  // [31  :   16] [15 : 8] [7 :0]
  // coherence-id msg-type action
  //---------------------------------------
  // msg type:
  // [1] Acquire [2] Release (writeback) [3] Probe [4] Flush
  //---------------------------------------
  // action:
  // Acquire: fetch for [0] read / [1] write
  // Release: [0] evict / [1] writeback (keep modified)
  // Probe: [0] evict / [1] writeback (keep shared)
  // Flush: [0] evict / [1] writeback

  constexpr static uint32_t acquire_msg = 1 << 8;
  constexpr static uint32_t release_msg = 2 << 8;
  constexpr static uint32_t probe_msg = 3 << 8;
  constexpr static uint32_t flush_msg = 4 << 8;

  constexpr static uint32_t acquire_read = 0;
  constexpr static uint32_t acquire_write = 1;

  constexpr static uint32_t release_evict = 0;
  constexpr static uint32_t release_writeback = 1;

  constexpr static uint32_t probe_evict = 0;
  constexpr static uint32_t probe_writeback = 1;

  constexpr static uint32_t flush_evict = 0;
  constexpr static uint32_t flush_writeback = 1;
public:
  static inline bool is_acquire(uint32_t cmd) {return (cmd & 0x0ff00ul) == acquire_msg; }
  static inline bool is_release(uint32_t cmd) {return (cmd & 0x0ff00ul) == release_msg; }
  static inline bool is_probe(uint32_t cmd)   {return (cmd & 0x0ff00ul) == probe_msg; }
  static inline bool is_flush(uint32_t cmd)   {return (cmd & 0x0ff00ul) == flush_msg; }
  static inline bool is_evict(uint32_t cmd)   {return (is_probe(cmd) && probe_evict == get_action(cmd)) ||
      (is_flush(cmd) && flush_evict == get_action(cmd)); }
  static inline uint32_t get_id(uint32_t cmd) {return cmd >> 16; }
  static inline uint32_t get_action(uint32_t cmd) {return cmd & 0x0fful; }

  // attach an id to a command
  static inline uint32_t attach_id(uint32_t cmd, uint32_t id) {return (cmd & (0x0fffful)) | (id << 16); }

  // check whether reverse probing is needed for a cache block when acquired (by inner) or probed by (outer)
  static inline bool need_sync(uint32_t cmd, CMMetadataBase *meta) {
    return (is_probe(cmd) && probe_evict == get_action(cmd)) || meta->is_modified() || (is_acquire(cmd) && acquire_write == get_action(cmd));
  }

  // check whether a permission upgrade is needed for the required action
  static inline bool need_promote(uint32_t cmd, CMMetadataBase *meta) {
    return (is_acquire(cmd) && acquire_write == get_action(cmd) && !meta->is_modified());
  }

  // avoid self probe
  static inline bool need_probe(uint32_t cmd, uint32_t coh_id) { return coh_id != get_id(cmd); }

  // generate the command for reverse probe
  static inline uint32_t cmd_for_sync(uint32_t cmd) {
    if(is_acquire(cmd) && acquire_read == get_action(cmd)) {
      return attach_id(probe_msg | probe_writeback, get_id(cmd));
    } else if ((is_probe(cmd)   && probe_writeback == get_action(cmd)) ||
               (is_flush(cmd) && flush_writeback == get_action(cmd))) {
      return attach_id(probe_msg | probe_writeback, -1);
    } else {
      assert((is_acquire(cmd) && acquire_write == get_action(cmd)) ||
             (is_probe(cmd)   && probe_evict == get_action(cmd))  ||
             (is_release(cmd) && release_evict == get_action(cmd)) ||
             (is_flush(cmd)) && flush_evict == get_action(cmd));
      return attach_id(probe_msg | probe_evict, -1);
    }
  }

  static inline constexpr uint32_t cmd_for_evict()           { return release_msg | release_evict;     }
  static inline constexpr uint32_t cmd_for_writeback()       { return release_msg | release_writeback; }
  static inline constexpr uint32_t cmd_for_flush_evict()     { return flush_msg   | flush_evict;       }
  static inline constexpr uint32_t cmd_for_flush_writeback() { return flush_msg   | flush_writeback;   }
  static inline constexpr uint32_t cmd_for_core_read()       { return acquire_msg | acquire_read;      }
  static inline constexpr uint32_t cmd_for_core_write()      { return acquire_msg | acquire_write;     }

  // set the meta after processing an acquire
  static inline void meta_after_acquire(uint32_t cmd, CMMetadataBase *meta) {
    assert(is_acquire(cmd)); // must be an acquire
    if(acquire_read == get_action(cmd))
      meta->to_shared();
    else {
      assert(acquire_write == get_action(cmd));
      meta->to_modified();
    }
  }

  // set the metadata for a newly fetched block
  static inline void meta_after_grant(uint32_t cmd, CMMetadataBase *meta, uint64_t addr) {
    assert(is_acquire(cmd)); // must be an acquire
    assert(!meta->is_dirty()); // by default an invalid block must be clean
    meta->init(addr);
    if(acquire_read == get_action(cmd))
      meta->to_shared();
    else {
      assert(acquire_write == get_action(cmd));
      meta->to_modified();
    }
  }

  // set the metadata after a block is written back
  static inline void meta_after_writeback(uint32_t cmd, CMMetadataBase *meta, CMMetadataBase *data_meta) {
    if(is_release(cmd) || (is_flush(cmd) && meta) ){
      meta->to_clean();
      if(release_evict == get_action(cmd) || flush_evict == get_action(cmd)) { meta->to_invalid(); data_meta->to_invalid();}
    }
  }

  // set the meta after the block is released
  static inline void meta_after_release(uint32_t cmd, CMMetadataBase *meta) { meta->to_dirty(); }

  // update the metadata for inner cache after ack a probe
  static inline void meta_after_probe_ack(uint32_t cmd, CMMetadataBase *meta, CMMetadataBase *data_meta) {
    assert(is_probe(cmd)); // must be a probe
    if(probe_evict == get_action(cmd)){
      meta->to_invalid();
      data_meta->to_invalid();
    }
    else {
      assert(meta->is_modified()); // for MSI, probe degradation happens only for modified state
      meta->to_shared();
    }
  }
};

class MirageDataMeta : public CMMetadataBase
{
protected:
  bool state; // false: invalid, true: valid
  uint32_t mai, ms, mw; // data meta pointer to meta

public:
  MirageDataMeta() : state(false), mai(0), ms(0), mw(0) {}
  virtual void bind(uint32_t ai, uint32_t s, uint32_t w) { mai = ai; ms = s; mw = w; state = true; }
  virtual void meta(uint32_t* ai, uint32_t* s, uint32_t* w) { *ai = mai; *s = ms; *w = mw; }
  virtual void to_invalid() { state = false; }
  virtual bool is_valid() const { return state; }
  
  virtual ~MirageDataMeta() {}
};

// Mirage 
// IW: index width, NW: number of ways, EW: extra tag ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use), DTMT: data meta type 
// MIDX: indexer type, MRPC: replacer type
// DIDX: data indexer type, DRPC: data replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, int EW, int P, typename MT, typename DT, typename DTMT, typename MIDX, typename DIDX, typename MRPC, typename DRPC, typename DLY, bool EnMon,
         typename = typename std::enable_if<std::is_base_of<CMMetadataBase, MT>::value>::type,  // MT <- CMMetadataBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type, // DT <- CMDataBase or void
         typename = typename std::enable_if<std::is_base_of<CMMetadataBase, DTMT>::value>::type,  // DTMT <- MirageDataMeta
         typename = typename std::enable_if<std::is_base_of<IndexFuncBase, MIDX>::value>::type,  // MIDX <- IndexFuncBase
         typename = typename std::enable_if<std::is_base_of<IndexFuncBase, DIDX>::value>::type,  // DIDX <- IndexFuncBase
         typename = typename std::enable_if<std::is_base_of<ReplaceFuncBase, MRPC>::value>::type,  // MRPC <- ReplaceFuncBase
         typename = typename std::enable_if<std::is_base_of<ReplaceFuncBase, DRPC>::value>::type,  // MRPC <- ReplaceFuncBase
         typename = typename std::enable_if<std::is_base_of<DelayBase, DLY>::value || std::is_void<DLY>::value>::type>  // DLY <- DelayBase or void
class CacheMirage : public CacheBase
{
// see: https://www.usenix.org/system/files/sec21fall-saileshwar.pdf
protected:
  MIDX m_indexer;     // meta index resolver
  MRPC m_replacer[P]; // meta replacer
  DLY *timer;       // delay estimator
  DIDX d_indexer;   // data index resolver
  DRPC d_replacer;  // data replacer

public:
  CacheMirage(std::string name = "")
    : CacheBase(name)
  { 
    // CacheMirage has P+1 CacheArray
    arrays.resize(P+1);
    for(int i = 0; i < P; i++)  arrays[i] = new CacheArrayNorm<IW,NW+EW,MT,void>();  // The first P CacheArrays only have Meta without Data
    arrays[P] = new CacheArrayNorm<IW,P*NW,DTMT,DT>(); // The last CacheArray has a global data (see mirage paper), and its meta (DTMT) holds a pointer from data to meta
    if constexpr (!std::is_void<DLY>::value) timer = new DLY();
  }

  virtual ~CacheMirage() {
    if constexpr (!std::is_void<DLY>::value) delete timer;
  }

  virtual bool hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w ) {
    for(*ai=0; *ai<P; (*ai)++) {
      *s = m_indexer.index(addr, *ai);
      for(*w=0; *w<NW+EW; (*w)++)
        if(access(*ai, *s, *w)->match(addr)) return true;
    }
    return false;
  }

  virtual bool hit(uint64_t addr){
    uint32_t ai, s, w;
    return hit(addr, &ai, &s, &w);
  }

  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w) {
    if constexpr (P==1) *ai = 0;
    else                *ai = (cm_get_random_uint32() % P);
    *s = m_indexer.index(addr, *ai);
    m_replacer[*ai].replace(*s, w);
  }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) {
    m_replacer[ai].access(s, w);
    if constexpr (EnMon) for(auto m:this->monitors) m->read(addr, ai, s, w, hit);
    if constexpr (!std::is_void<DLY>::value) timer->read(addr, ai, s, w, hit, delay);
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) {
    m_replacer[ai].access(s, w);
    if constexpr (EnMon) for(auto m:this->monitors) m->write(addr, ai, s, w, hit);
    if constexpr (!std::is_void<DLY>::value) timer->write(addr, ai, s, w, hit, delay);
  }

  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, uint64_t *delay) {
    if(hit && evict) {
      m_replacer[ai].invalid(s, w);
      if constexpr (EnMon) for(auto m:this->monitors) m->invalid(addr, ai, s, w);
    }
    if constexpr (!std::is_void<DLY>::value) timer->manage(addr, ai, s, w, hit, evict, writeback, delay);
  }

  virtual CMMetadataBase *access(uint32_t ai, uint32_t s, uint32_t w){
    return arrays[ai]->get_meta(s, w);
  }

  virtual CMMetadataBase *access(uint32_t d_s, uint32_t d_w){
    return arrays[P]->get_meta(d_s, d_w);
  }

  virtual CMDataBase *get_data(uint32_t d_s, uint32_t d_w) {
    return arrays[P]->get_data(d_s, d_w);
  }

  virtual void replace_data(uint64_t addr, uint32_t *d_s, uint32_t *d_w) { 
    *d_s =  d_indexer.index(addr, 0);
    d_replacer.replace(*d_s, d_w); 
  }

  virtual void replace(uint64_t addr, uint32_t ai, uint32_t *s, uint32_t *w){
    *s = m_indexer.index(addr, ai);
    m_replacer[ai].replace(*s, w);
  }

  virtual void replace(uint64_t addr, std::vector<std::pair<std::pair<uint32_t, uint32_t>, uint32_t> > &location) {
    uint32_t s, w;
    location.resize(P);
    for(uint32_t ai = 0; ai < P; ai++){
      uint32_t free_num = 0;
      s = m_indexer.index(addr, ai);
      m_replacer[ai].replace(s, &w);
      for(uint32_t i = 0; i < NW+EW; i++){
        if(!access(ai, s, i)->is_valid()) free_num++;
      }
      location[ai] = std::make_pair(std::make_pair(s, w), free_num);
    }
  }

  virtual bool query_coloc(uint64_t addrA, uint64_t addrB){ 
    for(int i=0; i<P; i++) 
      if(m_indexer.index(addrA, i) == m_indexer.index(addrB, i)) 
        return true;
    return false;
  }
};

// metadata supporting MSI coherency
class MirageMetadataMSIBase : public CMMetadataBase
{
protected:
  unsigned int state : 2; // 0: invalid, 1: shared, 2:modify
  unsigned int dirty : 1; // 0: clean, 1: dirty
  uint32_t d_s, d_w;
public:
  MirageMetadataMSIBase() : state(0), dirty(0), d_s(0), d_w(0) {}
  virtual ~MirageMetadataMSIBase() {}

  virtual void to_invalid() { state = 0; }
  virtual void to_shared() { state = 1; }
  virtual void to_modified() { state = 2; }
  virtual void to_dirty() { dirty = 1; }
  virtual void to_clean() { dirty = 0; }
  virtual bool is_valid() const { return state; }
  virtual bool is_shared() const { return state == 1; }
  virtual bool is_modified() const {return state == 2; }
  virtual bool is_dirty() const { return dirty; }
};

// Metadata with match function
// AW    : address width
// IW    : index width
// TOfst : tag offset
template <int AW, int IW, int TOfst>
class MirageMetadataMSI : public MirageMetadataMSIBase
{
protected:
  uint64_t     tag   : AW-TOfst;
  constexpr static uint64_t mask = (1ull << (AW-TOfst)) - 1;

public:
  MirageMetadataMSI() : tag(0) {}
  virtual ~MirageMetadataMSI() {}

  virtual bool match(uint64_t addr) const { return is_valid() && ((addr >> TOfst) & mask) == tag; }
  virtual void reset() { tag = 0; state = 0; dirty = 0; }
  virtual void init(uint64_t addr) { tag = (addr >> TOfst) & mask; state = 0; dirty = 0;}
  virtual void bind(uint32_t ds, uint32_t dw) { d_s = ds; d_w = dw; }
  virtual void data(uint32_t* ds, uint32_t *dw) { *ds = d_s; *dw = d_w;}
  virtual uint64_t addr(uint32_t s) const {
    uint64_t addr = tag << TOfst;
    if constexpr (IW > 0) {
      constexpr uint32_t index_mask = (1 << IW) - 1;
      addr |= (s & index_mask) << (TOfst - IW);
    }
    return addr;
  }
};

// uncached MSI outer port:
//   no support for reverse probe as if there is no internal cache
//   or the interl cache does not participate in the coherence communication
template<typename MT, typename DT, typename Policy,
         typename = typename std::enable_if<std::is_base_of<MirageMetadataMSIBase, MT>::value>::type, // MT <- MirageMetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
class MirageOuterPortMSIUncached : public OuterCohPortBase
{
public:
  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd, uint64_t *delay) {
    coh->acquire_resp(addr, data, Policy::attach_id(cmd, this->coh_id), delay);
    Policy::meta_after_grant(cmd, meta, addr);
  }
  virtual void writeback_req(uint64_t addr, CMMetadataBase *meta, CMMetadataBase *data_meta, CMDataBase *data, uint32_t cmd, uint64_t *delay) {
    coh->writeback_resp(addr, data, Policy::attach_id(cmd, this->coh_id), delay);
    Policy::meta_after_writeback(cmd, meta, data_meta);
  }
};

// full MSI Outer port
template<typename MT, typename DT, typename Policy>
class MirageOuterPortMSI : public MirageOuterPortMSIUncached<MT, DT, Policy>
{
public:
  virtual void probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, uint32_t cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    bool hit, writeback = false;
    if(hit = this->cache->hit(addr, &ai, &s, &w)) {
      auto meta = this->cache->access(ai, s, w); // oddly here, `this->' is required by the g++ 11.3.0 @wsong83
      CMDataBase *data = nullptr;
      if constexpr (!std::is_void<DT>::value) {
        data = this->cache->get_data(ai, s, w);
      }

      // sync if necessary
      if(Policy::need_sync(cmd, meta)) this->inner->probe_req(addr, meta, data, Policy::cmd_for_sync(cmd), delay);

      // writeback if dirty
      if(writeback = meta->is_dirty()) { // dirty, writeback
        meta_outer->to_dirty();
        if constexpr (!std::is_void<DT>::value) data_outer->copy(data);
        meta->to_clean();
      }

      // update meta
      Policy::meta_after_probe_ack(cmd, meta);
    }
    this->cache->hook_manage(addr, ai, s, w, hit, Policy::is_evict(cmd), writeback, delay);
  }
};

// uncached MSI inner port:
//   no support for reverse probe as if there is no internal cache
//   or the interl cache does not participate in the coherence communication
template<typename MT, typename DT, typename Policy, bool isLLC, bool enableRelocation, int RW,
         typename = typename std::enable_if<std::is_base_of<MirageMetadataMSIBase, MT>::value>::type, // MT <- MirageMetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
class MirageInnerPortMSIUncached : public InnerCohPortBase
{
public:
  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, uint32_t cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    uint32_t ds, dw;
    CMMetadataBase *meta;
    CMDataBase *data;
    bool hit, writeback;
    if(hit = this->cache->hit(addr, &ai, &s, &w)) { // hit
      meta = this->cache->access(ai, s, w);
      meta->data(&ds, &dw);
      if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ds, dw);
      if(Policy::need_sync(cmd, meta)) probe_req(addr, meta, data, Policy::cmd_for_sync(cmd), delay); // sync if necessary
      if constexpr (!isLLC) {
        if(Policy::need_promote(cmd, meta)) {  // promote permission if needed
          outer->acquire_req(addr, meta, data, cmd, delay);
          hit = false;
        }
      }
    } else { // miss
      // get the way to be replaced
      std::stack<uint64_t> addr_stack;
      std::vector<std::pair<std::pair<uint32_t, uint32_t>, uint32_t> > location;
      std::unordered_set<uint64_t> remapped;
      std::vector<uint32_t> free_location;
      CMMetadataBase *mmeta, *data_meta, *data_mmeta;
      uint32_t P; uint32_t relocation = 0; 
      uint32_t m_ai, m_s, m_w, m_ds, m_dw;
      uint32_t max_free = 1;
      uint64_t addrr = addr;
      ai = 0; 
      this->cache->replace(addr, location);
      P = location.size();
      for(auto l : location) {
        if(max_free < l.second){
          free_location.resize(0);
          free_location.push_back(ai);
          max_free = l.second;
        }
        else if (max_free == l.second){
          free_location.push_back(ai);
        }
        ai++;
      }
      if(free_location.size() >= 1){
        ai = free_location[cm_get_random_uint32() % free_location.size()];
        s = location[ai].first.first; w = location[ai].first.second;
        meta = this->cache->access(ai, s, w);
        assert(!meta->is_valid());
      }
      else{ // If all the skews have no free space
        ai = cm_get_random_uint32()%P;
        s = location[ai].first.first; w = location[ai].first.second;
        meta = this->cache->access(ai, s, w);
        while(enableRelocation && meta->is_valid() && relocation < RW){
          relocation++;
          addr = meta->addr(s); 
          this->cache->replace(addr, (ai+1) % P, &m_s, &m_w);
          mmeta = this->cache->access((ai+1) % P, m_s, m_w);
          if(remapped.count(mmeta->addr(s))) break; // Prevent duplicate relocation during cuckoo relocation
          remapped.insert(addr);
          addr_stack.push(addr);
          meta = mmeta;
          ai = (ai+1) % P;
          s = m_s;
          w = m_w;
        }
        meta->data(&ds, &dw);
        data_meta = this->cache->access(ds, dw);
        if(meta->is_valid()) {
          auto replace_addr = meta->addr(s);
          if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ds, dw);
          if(Policy::need_sync(Policy::cmd_for_evict(), meta)) probe_req(replace_addr, meta, data, Policy::cmd_for_sync(Policy::cmd_for_evict()), delay); // sync if necessary
          if(writeback = meta->is_dirty()) 
            outer->writeback_req(replace_addr, meta, data_meta, data, Policy::cmd_for_evict(), delay); // writeback if dirty
          else 
            data_meta->to_invalid();
          this->cache->hook_manage(replace_addr, ai, s, w, true, true, writeback, delay);
        }
        while(addr_stack.size()>=1){
          addr = addr_stack.top();
          addr_stack.pop();
          this->cache->replace(addr, (ai-1+P)%P, &m_s, &m_w);
          mmeta = this->cache->access((ai-1+P)%P, m_s, m_w);
          mmeta->data(&m_ds, &m_dw);
          data_mmeta = this->cache->access(m_ds, m_dw);
          assert(addr == mmeta->addr(m_s));
          meta->init(addr); meta->bind(m_ds, m_dw); data_mmeta->bind(ai, s, w);
          if(mmeta->is_dirty()) { meta->to_dirty(); mmeta->to_clean();} 
          if(mmeta->is_shared()) 
            meta->to_shared();
          else
            meta->to_modified();
          mmeta->to_invalid();
          this->cache->hook_manage(addr, (ai-1+P)%P, m_s, m_w, true, true, false, delay);
          this->cache->hook_read(addr, ai, s, w, false, delay); // hit is true or false? may have impact on delay
            
          ai = (ai-1+P) % P;
          s = m_s;
          w = m_w;
          meta = mmeta;
        }
        addr = addrr;
      }
      this->cache->replace_data(addr, &ds, &dw);
      if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ds, dw);
      data_meta = this->cache->access(ds, dw);
      if(data_meta->is_valid()){
        uint32_t r_ai, r_s, r_w;
        data_meta->meta(&r_ai, &r_s, &r_w);
        CMMetadataBase* replace_meta = this->cache->access(r_ai, r_s, r_w);
        auto replace_addr = replace_meta->addr(s);
        assert(this->cache->hit(replace_addr, &r_ai, &r_s, &r_w));
        if(Policy::need_sync(Policy::cmd_for_evict(), replace_meta)) probe_req(replace_addr, replace_meta, data, Policy::cmd_for_sync(Policy::cmd_for_evict()), delay); // sync if necessary
        if(writeback = replace_meta->is_dirty()) 
          outer->writeback_req(replace_addr, replace_meta, data_meta, data, Policy::cmd_for_evict(), delay); // writeback if dirty
        else
          replace_meta->to_invalid(); 
        this->cache->hook_manage(replace_addr, r_ai, r_s, r_w, true, true, writeback, delay);
      }
      meta->bind(ds, dw);
      data_meta->bind(ai, s, w);
      outer->acquire_req(addr, meta, data, cmd, delay); // fetch the missing block
    }
    // grant
    if constexpr (!std::is_void<DT>::value) data_inner->copy(data);
    Policy::meta_after_acquire(cmd, meta);
    this->cache->hook_read(addr, ai, s, w, hit, delay);
  }

  virtual void writeback_resp(uint64_t addr, CMDataBase *data_inner, uint32_t cmd, uint64_t *delay) {
    if (isLLC || Policy::is_release(cmd)) {
      uint32_t ai, s, w;
      uint32_t ds, dw;
      bool writeback = false, hit = this->cache->hit(addr, &ai, &s, &w);
      CMMetadataBase *meta = nullptr;
      if(Policy::is_release(cmd)) {
        assert(hit); // must hit
        meta = this->cache->access(ai, s, w);
        meta->data(&ds, &dw);
        if constexpr (!std::is_void<DT>::value) this->cache->get_data(ds, dw)->copy(data_inner);
        Policy::meta_after_release(cmd, meta);
        this->cache->hook_write(addr, ai, s, w, hit, delay);
      } else {
        assert(Policy::is_flush(cmd));
        if(hit) {
          CMDataBase *data = nullptr;
          meta = this->cache->access(ai, s, w);
          meta->data(&ds, &dw);
          auto data_meta = this->cache->access(ds, dw);
          if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ds, dw);
          probe_req(addr, meta, data, Policy::cmd_for_sync(cmd), delay);
          if(writeback = meta->is_dirty()) outer->writeback_req(addr, meta, data_meta, data, cmd, delay);
        }
        this->cache->hook_manage(addr, ai, s, w, hit, Policy::is_evict(cmd), writeback, delay);
      }
    } else {
      assert(Policy::is_flush(cmd) && !isLLC);
      outer->writeback_req(addr, nullptr, nullptr, cmd, delay);
    }
  }
};

// full MSI inner port (broadcasting hub, snoop)
template<typename MT, typename DT, typename Policy, bool isLLC, bool enableRelocation, int RW>
class MirageInnerPortMSIBroadcast : public MirageInnerPortMSIUncached<MT, DT, Policy, isLLC, enableRelocation, RW>
{
public:
  virtual void probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd, uint64_t *delay) {
    for(uint32_t i=0; i<this->coh.size(); i++)
      if(Policy::need_probe(cmd, i))
        this->coh[i]->probe_resp(addr, meta, data, cmd, delay);
  }
};

// MSI core interface:
template<typename MT, typename DT, typename Policy, bool EnableDelay, bool isLLC, bool enableRelocation, int RW,
         typename = typename std::enable_if<std::is_base_of<MirageMetadataMSIBase, MT>::value>::type, // MT <- MirageMetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
class MirageCoreInterfaceMSI : public CoreInterfaceBase
{
  inline CMDataBase *access(uint64_t addr, uint32_t cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    uint32_t ds, dw;
    CMMetadataBase *meta;
    CMDataBase *data = nullptr;
    bool hit, writeback;
    if(hit = this->cache->hit(addr, &ai, &s, &w)) { // hit
      meta = this->cache->access(ai, s, w);
      meta->data(&ds, &dw);
      if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ai, s, w);
      if constexpr (!isLLC) {
        if(Policy::need_promote(cmd, meta)) {
          outer->acquire_req(addr, meta, data, cmd, delay);
          hit = false;
        }
      }
    } else { // miss
      // get the way to be replaced
      std::stack<uint64_t> addr_stack;
      std::vector<std::pair<std::pair<uint32_t, uint32_t>, uint32_t> > location;
      std::unordered_set<uint64_t> remapped;
      std::vector<uint32_t> free_location;
      CMMetadataBase *mmeta, *data_meta, *data_mmeta;
      uint32_t P; uint32_t relocation = 0; 
      uint32_t m_ai, m_s, m_w, m_ds, m_dw;
      uint32_t max_free = 1;
      uint64_t addrr = addr;
      ai = 0; 
      this->cache->replace(addr, location);
      P = location.size();
      for(auto l : location) {
        if(max_free < l.second){
          free_location.resize(0);
          free_location.push_back(ai);
          max_free = l.second;
        }
        else if (max_free == l.second){
          free_location.push_back(ai);
        }
        ai++;
      }
      if(free_location.size() >= 1){
        ai = free_location[cm_get_random_uint32() % free_location.size()];
        s = location[ai].first.first; w = location[ai].first.second;
        meta = this->cache->access(ai, s, w);
      }
      else{ // If all the skews have no free space
        ai = cm_get_random_uint32()%P;
        s = location[ai].first.first; w = location[ai].first.second;
        meta = this->cache->access(ai, s, w);
        while(enableRelocation && meta->is_valid() && relocation < RW){
          relocation++;
          addr = meta->addr(s); 
          this->cache->replace(addr, (ai+1) % P, &s, &w);
          mmeta = this->cache->access((ai+1) % P, s, w);
          if(remapped.count(mmeta->addr(s))) break; // Prevent duplicate relocation during cuckoo relocation
          remapped.insert(addr);
          addr_stack.push(addr);
          meta = mmeta;
          ai = (ai+1) % P;
        }
        meta->data(&ds, &dw);
        data_meta = this->cache->access(ds, dw);
        if(meta->is_valid()) {
          auto replace_addr = meta->addr(s);
          if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ds, dw);
          if(writeback = meta->is_dirty()) 
            outer->writeback_req(replace_addr, meta, data_meta, data, Policy::cmd_for_evict(), delay); // writeback if dirty
          else 
            data_meta->to_invalid();
          this->cache->hook_manage(replace_addr, ai, s, w, true, true, writeback, delay);
        }
        while(addr_stack.size()>=1){
          addr = addr_stack.top();
          addr_stack.pop();
          this->cache->replace(addr, (ai-1+P)%P, &m_s, &m_w);
          mmeta = this->cache->access((ai-1+P)%P, m_s, m_w);
          mmeta->data(&m_ds, &m_dw);
          data_mmeta = this->cache->access(m_ds, m_dw);
          assert(addr == mmeta->addr(m_s));
          meta->init(addr); meta->bind(m_ds, m_dw); data_mmeta->bind(ai, s, w);
          if(mmeta->is_dirty()) { meta->to_dirty(); mmeta->to_clean();} 
          if(mmeta->is_shared()) 
            meta->to_shared();
          else
            meta->to_modified();
          mmeta->to_invalid();
          this->cache->hook_manage(addr, (ai-1+P)%P, m_s, m_w, true, true, false, delay);
          this->cache->hook_read(addr, ai, s, w, false, delay); // hit is true or false? may have impact on delay
            
          ai = (ai-1+P) % P;
          s = m_s;
          w = m_w;
          meta = mmeta;
        }
        addr = addrr;
      }
      this->cache->replace_data(addr, &ds, &dw);
      if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ds, dw);
      data_meta = this->cache->access(ds, dw);
      if(data_meta->is_valid()){
        uint32_t r_ai, r_s, r_w;
        data_meta->meta(&r_ai, &r_s, &r_w);
        CMMetadataBase* replace_meta = this->cache->access(r_ai, r_s, r_w);
        auto replace_addr = replace_meta->addr(s);
        assert(this->cache->hit(replace_addr, &r_ai, &r_s, &r_w));
        if(writeback = replace_meta->is_dirty()) 
          outer->writeback_req(replace_addr, replace_meta, data_meta, data, Policy::cmd_for_evict(), delay); // writeback if dirty
        else 
          replace_meta->to_invalid();
        this->cache->hook_manage(replace_addr, r_ai, r_s, r_w, true, true, writeback, delay);
      }
      meta->bind(ds, dw);
      data_meta->bind(ai, s, w);
      outer->acquire_req(addr, meta, data, cmd, delay); // fetch the missing block
    }
    if(cmd == Policy::cmd_for_core_write()) {
      meta->to_dirty();
      this->cache->hook_write(addr, ai, s, w, hit, delay);
    } else
      this->cache->hook_read(addr, ai, s, w, hit, delay);
    return data;
  }

  inline void manage(uint64_t addr, uint32_t cmd, uint64_t *delay) {
    if constexpr (isLLC) {
      uint32_t ai, s, w;
      bool hit, writeback = false;
      CMMetadataBase *meta = nullptr;
      CMDataBase *data = nullptr;
      if(hit = this->cache->hit(addr, &ai, &s, &w)){
        meta = this->cache->access(ai, s, w);
        if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ai, s, w);
        if(writeback = meta->is_dirty()) outer->writeback_req(addr, meta, data, cmd, delay);
      }
      this->cache->hook_manage(addr, ai, s, w, hit, Policy::is_evict(cmd), writeback, delay);
    } else {
      outer->writeback_req(addr, nullptr, nullptr, cmd, delay);
    }
  }

public:
  virtual const CMDataBase *read(uint64_t addr, uint64_t *delay) {
    return access(addr, Policy::cmd_for_core_read(), EnableDelay ? delay : nullptr);
  }

  virtual void write(uint64_t addr, const CMDataBase *data, uint64_t *delay) {
    auto m_data = access(addr, Policy::cmd_for_core_write(), EnableDelay ? delay : nullptr);
    if constexpr (!std::is_void<DT>::value) m_data->copy(data);
  }

  virtual void flush(uint64_t addr, uint64_t *delay)     { manage(addr, Policy::cmd_for_flush_evict(),     delay); }
  virtual void writeback(uint64_t addr, uint64_t *delay) { manage(addr, Policy::cmd_for_flush_writeback(), delay); }
  virtual void writeback_invalidate(uint64_t *delay) {
    assert(nullptr == "Error: L1.writeback_invalidate() is not implemented yet!");
  }

};

#endif
