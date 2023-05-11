#ifndef CM_CACHE_MSI_HPP
#define CM_CACHE_MSI_HPP

#include <cassert>
#include <type_traits>
#include "cache/coherence.hpp"

namespace // file visibility
{
  // MSI protocol
  class Policy {
    // definition of command:
    // [31  :   16] [15 : 8] [7 :0]
    // coherence-id msg-type action
    //---------------------------------------
    // msg type:
    // [1] Acquire [2] Release (writeback) [3] Probe
    //---------------------------------------
    // action:
    // Acquire: fetch for [0] read / [1] write
    // Release: [0] evict / [1] writeback (keep modified)
    // Probe: [0] evict / [1] writeback (keep shared)

    static const uint32_t acquire_msg = 1 << 8;
    static const uint32_t release_msg = 2 << 8;
    static const uint32_t probe_msg = 3 << 8;

    static const uint32_t acquire_read = 0;
    static const uint32_t acquire_write = 1;

    static const uint32_t release_evict = 0;
    static const uint32_t release_writeback = 1;

    static const uint32_t probe_evict = 0;
    static const uint32_t probe_writeback = 1;

    static inline bool is_acquire(uint32_t cmd) {return (cmd & 0x0ff00ul) == acquire_msg; }
    static inline bool is_release(uint32_t cmd) {return (cmd & 0x0ff00ul) == release_msg; }
    static inline bool is_probe(uint32_t cmd)   {return (cmd & 0x0ff00ul) == probe_msg; }
    static inline uint32_t get_id(uint32_t cmd) {return cmd >> 16; }
    static inline uint32_t get_action(uint32_t cmd) {return cmd & 0x0fful; }
    static inline uint32_t attach_id(uint32_t cmd, uint32_t id) {return (cmd & (~0x0fffful)) | (id << 16); }
    
  public:

    // check whether reverse probing is needed for a cache block when acquired (by inner) or probed by (outer)
    static inline bool need_sync(uint32_t cmd, CMMetadataBase *meta) {
      return (is_probe(cmd) && probe_evict == get_action(cmd)) || meta->is_modified();
    }

    // avoid self probe
    static inline bool need_probe(uint32_t cmd, uint32_t coh_id) { return coh_id != get_id(cmd); }

    // generate the command for reverse probe
    static inline uint32_t cmd_for_sync(uint32_t cmd) {
      uint32_t rv = attach_id(probe_msg, get_id(cmd));

      // set whether the probe will purge the block from inner caches
      if((is_acquire(cmd) && acquire_read == get_action(cmd)) ||
         (is_probe(cmd)   && probe_writeback == get_action(cmd))) // need to purge
        return rv | probe_writeback;
      else {
        assert((is_acquire(cmd) && acquire_write == get_action(cmd)) ||
               (is_probe(cmd)   && probe_evict == get_action(cmd)));
        return rv | probe_evict;
      }
    }

    // command to evict a cache block from this cache
    static inline uint32_t cmd_for_evict() { return attach_id(release_msg | release_evict, -1); } // eviction needs no coh_id

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
    static inline void meta_after_grant(uint32_t cmd, CMMetadataBase *meta) {
      assert(is_acquire(cmd)); // must be an acquire
      assert(!meta->is_dirty()); // by default an invalid block must be clean
      if(acquire_read == get_action(cmd))
        meta->to_shared();
      else {
        assert(acquire_write == get_action(cmd));
        meta->to_modified();
      }
    }

    // set the metadata after a block is written back
    static inline void meta_after_writeback(uint32_t cmd, CMMetadataBase *meta) {
      assert(is_release(cmd)); // must be an acquire
      meta->to_clean();
      if(release_evict == get_action(cmd)) meta->to_invalid();
    }

    // set the meta after the block is released
    static inline void meta_after_release(uint32_t cmd, CMMetadataBase *meta) { meta->to_dirty(); }

    // update the metadata for inner cache after ack a probe
    static inline void meta_after_probe_ack(uint32_t cmd, CMMetadataBase *meta) {
      assert(is_probe(cmd)); // must be a probe
      if(probe_evict == get_action(cmd))
        meta->to_invalid();
      else {
        assert(meta->is_modified()); // for MSI, probe degradation happens only for modified state
        meta->to_shared();
      }
    }

  };

}

// metadata supporting MSI coherency
class MetadataMSIBase : public CMMetadataBase
{
protected:
  unsigned int state : 2; // 0: invalid, 1: shared, 2:modify
  unsigned int dirty : 1; // 0: clean, 1: dirty
public:
  MetadataMSIBase() : state(0), dirty(0) {}
  virtual ~MetadataMSIBase() {}

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
// TOfst : tag offset
template <int AW, int TOfst>
class MetadataMSI : public MetadataMSIBase
{
protected:
  uint64_t     tag   : AW-TOfst;
  static const uint64_t mask = (1ull << (AW-TOfst)) - 1;

public:
  MetadataMSI() : tag(0) {}
  virtual ~MetadataMSI() {}

  virtual bool match(uint64_t addr) { return ((addr >> TOfst) & mask) == tag; }
  virtual void reset() { tag = 0; state = 0; dirty = 0; }
};


// uncached MSI outer port:
//   no support for reverse probe as if there is no internal cache
//   or the interl cache does not participate in the coherence communication
template<typename MT, typename DT,
         typename = typename std::enable_if<std::is_base_of<MetadataMSIBase, MT>::value>::type, // MT <- MetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
class OuterPortMSIUncached : public OuterCohPortBase
{
public:
  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd) {
    coh->acquire_resp(addr, data, Policy::attach_id(cmd, this->coh_id));
    Policy::meta_after_grant(cmd, meta);
  }
  virtual void writeback_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd) {
    coh->writeback_resp(addr, data, Policy::attach_id(cmd, this->coh_id));
    Policy::meta_after_writeback(cmd, meta);
  }
};

// full MSI Outer port
template<typename MT, typename DT>
class OuterPortMSI : public OuterPortMSIUncached<MT, DT>
{
public:
  virtual void probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, uint32_t cmd) {
    uint32_t ai, s, w;
    if(this->cache->hit(addr, &ai, &s, &w)) {
      auto meta = this->cache->access(ai, s, w); // oddly here, `this->' is required by the g++ 11.3.0 @wsong83
      CMDataBase *data = std::is_void<DT>::value ? nullptr : this->cache->get_data(ai, s, w);

      // sync if necessary
      if(this->inner && Policy::need_sync(cmd, meta)) this->inner->probe_req(addr, meta, data, Policy::cmd_for_sync(cmd));

      // writeback if dirty
      if(meta->is_dirty()) { // dirty, writeback
        meta_outer->to_dirty();
        if(!std::is_void<DT>::value) data_outer->copy(data);
        meta->to_clean();
      }

      // update meta
      Policy::meta_after_probe_ack(cmd, meta);
    }
  }
};

// uncached MSI inner port:
//   no support for reverse probe as if there is no internal cache
//   or the interl cache does not participate in the coherence communication
template<typename MT, typename DT,
         typename = typename std::enable_if<std::is_base_of<MetadataMSIBase, MT>::value>::type, // MT <- MetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
class InnerPortMSIUncached : public InnerCohPortBase
{
public:
  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, uint32_t cmd) {
    uint32_t ai, s, w;
    CMMetadataBase *meta;
    CMDataBase *data;
    if(this->cache->hit(addr, &ai, &s, &w)) { // hit
      meta = this->cache->access(ai, s, w);
      if(!std::is_void<DT>::value) data = this->cache->get_data(ai, s, w);

      // sync if necessary
      if(Policy::need_sync(cmd, meta)) probe_req(addr, meta, data, Policy::cmd_for_sync(cmd));
    } else { // miss
      assert(outer);

      // get the way to be replaced
      this->cache->replace(addr, &ai, &s, &w);
      meta = this->cache->access(ai, s, w);

      if(meta->is_valid()) {
        if(!std::is_void<DT>::value) data = this->cache->get_data(ai, s, w);

        // sync if necessary
        if(Policy::need_sync(Policy::cmd_for_evict(), meta)) probe_req(addr, meta, data, Policy::cmd_for_sync(Policy::cmd_for_evict()));

        // writeback if dirty
        if(meta->is_dirty()) outer->writeback_req(addr, meta, data, Policy::cmd_for_evict());
      }

      // fetch the missing block
      outer->acquire_req(addr, meta, data, cmd);
    }
    // grant
    if(!std::is_void<DT>::value) data_inner->copy(this->cache->get_data(ai, s, w));
    Policy::meta_after_acquire(cmd, meta);
  }

  virtual void writeback_resp(uint64_t addr, CMDataBase *data, uint32_t cmd) {
    uint32_t ai, s, w;
    CMMetadataBase *meta;
    auto h = this->cache->hit(addr, &ai, &s, &w);
    assert(h); // must hit
    meta = this->cache->access(ai, s, w);
    if(!std::is_void<DT>::value) this->cache->get_data(ai, s, w)->copy(data);
    Policy::meta_after_release(cmd, meta);
  }
};

// full MSI inner port (broadcasting hub, snoop)
template<typename MT, typename DT>
class InnerPortMSIBroadcast : public InnerPortMSIUncached<MT, DT>
{
public:
  virtual void probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd) {
    for(uint32_t i=0; i<this->coh.size(); i++)
      if(Policy::need_probe(cmd, i))
        this->coh[i]->probe_resp(addr, meta, data, cmd);
  }
};

#endif
