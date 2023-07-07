#ifndef CM_CACHE_MIRAGE_MSI_HPP
#define CM_CACHE_MIRAGE_MSI_HPP

#include <cassert>
#include <type_traits>
#include "cache/coherence.hpp"

namespace // file visibility
{
  // MirageMSI protocol
  class MiragePolicy {
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
      if(is_release(cmd)){
        meta->to_clean();
        if(release_evict == get_action(cmd)) { meta->to_invalid(); data_meta->to_invalid();}
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
}
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
template<typename MT, typename DT,
         typename = typename std::enable_if<std::is_base_of<MirageMetadataMSIBase, MT>::value>::type, // MT <- MirageMetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
class MirageOuterPortMSIUncached : public OuterCohPortBase
{
public:
  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd, uint64_t *delay) {
    coh->acquire_resp(addr, data, MiragePolicy::attach_id(cmd, this->coh_id), delay);
    MiragePolicy::meta_after_grant(cmd, meta, addr);
  }
  virtual void writeback_req(uint64_t addr, CMMetadataBase *meta, CMMetadataBase *data_meta, CMDataBase *data, uint32_t cmd, uint64_t *delay) {
    coh->writeback_resp(addr, data, MiragePolicy::attach_id(cmd, this->coh_id), delay);
    MiragePolicy::meta_after_writeback(cmd, meta, data_meta);
  }
};

// full MSI Outer port
template<typename MT, typename DT>
class MirageOuterPortMSI : public MirageOuterPortMSIUncached<MT, DT>
{
public:
  virtual void probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, uint32_t cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    bool hit, writeback = false;
    if(this->cache->hit(addr, &ai, &s, &w)) {
      auto meta = this->cache->access(ai, s, w); // oddly here, `this->' is required by the g++ 11.3.0 @wsong83
      CMDataBase *data = nullptr;
      if constexpr (!std::is_void<DT>::value) {
        data = this->cache->get_data(ai, s, w);
      }

      // sync if necessary
      if(MiragePolicy::need_sync(cmd, meta)) this->inner->probe_req(addr, meta, data, MiragePolicy::cmd_for_sync(cmd), delay);

      // writeback if dirty
      if(writeback = meta->is_dirty()) { // dirty, writeback
        meta_outer->to_dirty();
        if constexpr (!std::is_void<DT>::value) data_outer->copy(data);
        meta->to_clean();
      }

      // update meta
      MiragePolicy::meta_after_probe_ack(cmd, meta);
    }
    this->cache->hook_manage(addr, ai, s, w, hit, MiragePolicy::is_evict(cmd), writeback, delay);
  }
};

// uncached MSI inner port:
//   no support for reverse probe as if there is no internal cache
//   or the interl cache does not participate in the coherence communication
template<typename MT, typename DT, bool isLLC,
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
      if(MiragePolicy::need_sync(cmd, meta)) probe_req(addr, meta, data, MiragePolicy::cmd_for_sync(cmd), delay); // sync if necessary
      if constexpr (!isLLC) {
        if(MiragePolicy::need_promote(cmd, meta)) {  // promote permission if needed
          outer->acquire_req(addr, meta, data, cmd, delay);
          hit = false;
        }
      }
    } else { // miss
      // get the way to be replaced
      CMMetadataBase* data_meta;
      this->cache->replace(addr, &ai, &s, &w);
      meta = this->cache->access(ai, s, w);
      if(meta->is_valid()) {
        auto replace_addr = meta->addr(s);
        meta->data(&ds, &dw);
        if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ds, dw);
        data_meta = this->cache->access(ds, dw);
        if(MiragePolicy::need_sync(MiragePolicy::cmd_for_evict(), meta)) probe_req(replace_addr, meta, data, MiragePolicy::cmd_for_sync(MiragePolicy::cmd_for_evict()), delay); // sync if necessary
        if(writeback = meta->is_dirty()) outer->writeback_req(replace_addr, meta, data_meta, data, MiragePolicy::cmd_for_evict(), delay); // writeback if dirty
        this->cache->hook_manage(replace_addr, ai, s, w, true, true, writeback, delay);
      }
      else{
        this->cache->replace_data(addr, &ds, &dw);
        if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ds, dw);
        data_meta = this->cache->access(ds, dw);
        if(data_meta->is_valid()){
          uint32_t r_ai, r_s, r_w;
          data_meta->meta(&r_ai, &r_s, &r_w);
          CMMetadataBase* replace_meta = this->cache->access(r_ai, r_s, r_w);
          auto replace_addr = replace_meta->addr(s);
          assert(this->cache->hit(replace_addr, &r_ai, &r_s, &r_w));
          if(MiragePolicy::need_sync(MiragePolicy::cmd_for_evict(), replace_meta)) probe_req(replace_addr, replace_meta, data, MiragePolicy::cmd_for_sync(MiragePolicy::cmd_for_evict()), delay); // sync if necessary
          if(writeback = replace_meta->is_dirty()) outer->writeback_req(replace_addr, replace_meta, data_meta, data, MiragePolicy::cmd_for_evict(), delay); // writeback if dirty
          this->cache->hook_manage(replace_addr, r_ai, r_s, r_w, true, true, writeback, delay);
        }
        meta->bind(ds, dw);
        data_meta->bind(ai, s, w);
      }
      outer->acquire_req(addr, meta, data, cmd, delay); // fetch the missing block
    }
    // grant
    data_inner->copy(data);
    MiragePolicy::meta_after_acquire(cmd, meta);
    this->cache->hook_read(addr, ai, s, w, hit, delay);
  }

  virtual void writeback_resp(uint64_t addr, CMDataBase *data_inner, uint32_t cmd, uint64_t *delay) {
    if (isLLC || MiragePolicy::is_release(cmd)) {
      uint32_t ai, s, w;
      uint32_t ds, dw;
      bool writeback = false, hit = this->cache->hit(addr, &ai, &s, &w);
      CMMetadataBase *meta = nullptr;
      if(MiragePolicy::is_release(cmd)) {
        assert(hit); // must hit
        meta = this->cache->access(ai, s, w);
        meta->data(&ds, &dw);
        if constexpr (!std::is_void<DT>::value) this->cache->get_data(ds, dw)->copy(data_inner);
        MiragePolicy::meta_after_release(cmd, meta);
        this->cache->hook_write(addr, ai, s, w, hit, delay);
      } else {
        assert(MiragePolicy::is_flush(cmd));
        if(hit) {
          CMDataBase *data = nullptr;
          meta = this->cache->access(ai, s, w);
          if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ai, s, w);
          probe_req(addr, meta, data, MiragePolicy::cmd_for_sync(cmd), delay);
          if(writeback = meta->is_dirty()) outer->writeback_req(addr, meta, data, cmd, delay);
        }
        this->cache->hook_manage(addr, ai, s, w, hit, MiragePolicy::is_evict(cmd), writeback, delay);
      }
    } else {
      assert(MiragePolicy::is_flush(cmd) && !isLLC);
      outer->writeback_req(addr, nullptr, nullptr, cmd, delay);
    }
  }
};

// full MSI inner port (broadcasting hub, snoop)
template<typename MT, typename DT, bool isLLC>
class MirageInnerPortMSIBroadcast : public MirageInnerPortMSIUncached<MT, DT, isLLC>
{
public:
  virtual void probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd, uint64_t *delay) {
    for(uint32_t i=0; i<this->coh.size(); i++)
      if(MiragePolicy::need_probe(cmd, i))
        this->coh[i]->probe_resp(addr, meta, data, cmd, delay);
  }
};

// MSI core interface:
template<typename MT, typename DT, bool EnableDelay, bool isLLC,
         typename = typename std::enable_if<std::is_base_of<MirageMetadataMSIBase, MT>::value>::type, // MT <- MirageMetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
class MirageCoreInterfaceMSI : public CoreInterfaceBase
{
  inline CMDataBase *access(uint64_t addr, uint32_t cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    CMMetadataBase *meta;
    CMDataBase *data = nullptr;
    bool hit, writeback;
    if(hit = this->cache->hit(addr, &ai, &s, &w)) { // hit
      meta = this->cache->access(ai, s, w);
      if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ai, s, w);
      if constexpr (!isLLC) {
        if(MiragePolicy::need_promote(cmd, meta)) {
          outer->acquire_req(addr, meta, data, cmd, delay);
          hit = false;
        }
      }
    } else { // miss
      // get the way to be replaced
      this->cache->replace(addr, &ai, &s, &w);
      meta = this->cache->access(ai, s, w);
      if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ai, s, w);
      if(meta->is_valid()) {
        auto replace_addr = meta->addr(s);
        if(writeback = meta->is_dirty()) outer->writeback_req(replace_addr, meta, data, MiragePolicy::cmd_for_evict(), delay); // writeback if dirty
        this->cache->hook_manage(replace_addr, ai, s, w, true, true, writeback, delay);
      }
      outer->acquire_req(addr, meta, data, cmd, delay); // fetch the missing block
    }

    if(cmd == MiragePolicy::cmd_for_core_write()) {
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
      this->cache->hook_manage(addr, ai, s, w, hit, MiragePolicy::is_evict(cmd), writeback, delay);
    } else {
      outer->writeback_req(addr, nullptr, nullptr, cmd, delay);
    }
  }

public:
  virtual const CMDataBase *read(uint64_t addr, uint64_t *delay) {
    return access(addr, MiragePolicy::cmd_for_core_read(), EnableDelay ? delay : nullptr);
  }

  virtual void write(uint64_t addr, const CMDataBase *data, uint64_t *delay) {
    auto m_data = access(addr, MiragePolicy::cmd_for_core_write(), EnableDelay ? delay : nullptr);
    if constexpr (!std::is_void<DT>::value) m_data->copy(data);
  }

  virtual void flush(uint64_t addr, uint64_t *delay)     { manage(addr, MiragePolicy::cmd_for_flush_evict(),     delay); }
  virtual void writeback(uint64_t addr, uint64_t *delay) { manage(addr, MiragePolicy::cmd_for_flush_writeback(), delay); }
  virtual void writeback_invalidate(uint64_t *delay) {
    assert(nullptr == "Error: L1.writeback_invalidate() is not implemented yet!");
  }

};

#endif
