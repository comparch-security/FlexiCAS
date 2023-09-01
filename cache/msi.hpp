#ifndef CM_CACHE_MSI_HPP
#define CM_CACHE_MSI_HPP

#include "cache/coherence.hpp"

template<typename MT, bool isL1, bool isLLC,
         typename = typename std::enable_if<std::is_base_of<MetadataMSIBase, MT>::value>::type> // MT <- MetadataMSIBase
class MSIPolicy : public CohPolicyBase
{
public:
  MSIPolicy() : CohPolicyBase(1, 2, 3, 4, 0, 1, 2, 3) {}
  virtual ~MSIPolicy();

  virtual std::pair<bool, coh_cmd_t> acquire_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const {
    if constexpr (!isL1) {
      assert(is_acquire(cmd));
      if(is_fetch_write(cmd)) return std::make_pair(true, {cmd.id, probe_msg, evict_act});
      else                    return need_sync(meta, cmd.id);
    } else return std::make_pair(false, {-1, 0, 0});
  }

  virtual std::pair<bool, coh_cmd_t> acquire_need_promote(coh_cmd_t cmd, const CMMetadataBase *meta) const {
    if constexpr (!isLLC) {
      assert(is_acquire(cmd));
      if(is_fetch_write(cmd) || meta->need_promote())
        return std::make_pair(true, {-1, acquire_msg, fetch_write_act}); // reconstruct cmd especially when cross coherence policies
      else
        return std::make_pair(false, {-1, 0, 0});
    } else return std::make_pair(false, {-1, 0, 0});
  }

  virtual void meta_after_fetch(coh_cmd_t cmd, CMMetadataBase *meta, uint64_t addr) const {
    assert(is_probe(cmd));
    assert(!meta->is_dirty());
    meta->init(addr);
    if(is_fetch_read(cmd)) meta->to_shared();
    else {
      assert(is_fetch_write(cmd));
      meta->to_modified();
    }
  }

  virtual void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta) const {
    assert(is_acquire(cmd));
    if(is_fetch_read(cmd)) meta->to_shared();
    else {
      assert(is_fetch_write(cmd));
      meta->to_modified();
    }
  }

  virtual std::pair<bool, coh_cmd_t> probe_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const {
    if constexpr (!isL1) {
      assert(is_probe(cmd));
      if(is_evict(cmd)) return std::make_pair(true, {-1, probe_msg, evict_act});
      else              return need_sync(meta, -1);
    } else return std::make_pair(false, {-1, 0, 0});
  }

  virtual std::pair<bool, coh_cmd_t> probe_need_probe(coh_cmd_t cmd, const CMMetadataBase *meta, uint32_t target_inner_id) const {
    assert(is_probe(cmd));
    auto meta_msi = static_cast<MetadataMSIBase *>(meta);
    if((is_evict(cmd)     && meta_msi->evict_need_probe(target_inner_id, cmd.id))     ||
       (is_writeback(cmd) && meta_msi->writeback_need_probe(target_inner_id, cmd.id)) ) {
      cmd.id = -1;
      return std::make_pair(true, cmd);
    } else
      return std::make_pair(false, {-1, 0, 0});
  }

  virtual void meta_after_probe(coh_cmd_t cmd, CMMetadataBase *meta) const {
    assert(is_probe(cmd));
    if(is_evict(cmd)) meta->to_invalid();
    else {
      assert(is_writeback(cmd));
      meta->to_shared();
    }
  }

  virtual std::pair<bool, coh_cmd_t> writeback_need_sync(const CMMetadataBase *meta) const {
    if constexpr (!isL1) return std::make_pair(true, {-1, probe_msg, evict_act});
    else                 std::make_pair(false, {-1, 0, 0});
  }

  virtual void meta_after_writeback(coh_cmd_t cmd, CMMetadataBase *meta) const {
    if(meta) { // flush may send out writeback request with null meta
      if(is_evict(cmd)) meta->to_invalid();
      else              meta->to_clean();
    }
  }

  virtual std::pair<bool, coh_cmd_t> flush_need_sync() const {
    if constexpr (isLLC) {
      assert(is_flush(cmd));
      if(is_evict(cmd)) return std::make_pair(true, {-1, probe_msg, evict_act});
      else              return need_sync(meta, -1);
    } else
      return std::make_pair(false, {-1, 0, 0});
  }

  virtual constexpr uint32_t cmd_for_evict()           { return release_msg | release_evict;     }
  virtual constexpr uint32_t cmd_for_writeback()       { return release_msg | release_writeback; }
  virtual constexpr uint32_t cmd_for_flush_evict()     { return flush_msg   | flush_evict;       }
  virtual constexpr uint32_t cmd_for_flush_writeback() { return flush_msg   | flush_writeback;   }
  virtual constexpr uint32_t cmd_for_core_read()       { return acquire_msg | acquire_read;      }
  virtual constexpr uint32_t cmd_for_core_write()      { return acquire_msg | acquire_write;     }

  // set the meta after processing an acquire
  virtual void meta_after_acquire(uint32_t cmd, CMMetadataBase *meta) {
    assert(is_acquire(cmd)); // must be an acquire
    if(acquire_read == get_action(cmd))
      meta->to_shared();
    else {
      assert(acquire_write == get_action(cmd));
      meta->to_modified();
    }
  }

  // set the metadata after a block is written back
  virtual void meta_after_writeback(uint32_t cmd, CMMetadataBase *meta) {
    if(is_release(cmd) || (is_flush(cmd) && meta != nullptr)){
      meta->to_clean();
      if(release_evict == get_action(cmd) || flush_evict == get_action(cmd)) meta->to_invalid();
    }
  }

  // set the meta after the block is released
  virtual void meta_after_release(uint32_t cmd, CMMetadataBase *meta) { meta->to_dirty(); }

  // update the metadata for inner cache after ack a probe
  virtual void meta_after_probe_ack(uint32_t cmd, CMMetadataBase *meta) {
    assert(is_probe(cmd)); // must be a probe
    if(probe_evict == get_action(cmd))
      meta->to_invalid();
    else {
      assert(meta->is_modified()); // for MSI, probe degradation happens only for modified state
      meta->to_shared();
    }
  }

};

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

  // extra functions supporting probes using MSI
  // default using snoopying protocol
  virtual bool evict_need_probe(int32_t target_id, int32_t request_id) const { return target_id != request_id; }
  virtual bool writeback_need_probe(int32_t target_id, int32_t request_id) const { return target_id != request_id; }

private:
  virtual void to_owned() {}
  virtual void to_exclusive() {}
};

// Metadata with match function
// AW    : address width
// IW    : index width
// TOfst : tag offset
template <int AW, int IW, int TOfst>
class MetadataMSI : public MetadataMSIBase
{
protected:
  uint64_t     tag   : AW-TOfst;
  constexpr static uint64_t mask = (1ull << (AW-TOfst)) - 1;

public:
  MetadataMSI() : tag(0) {}
  virtual ~MetadataMSI() {}

  virtual bool match(uint64_t addr) const { return is_valid() && ((addr >> TOfst) & mask) == tag; }
  virtual void reset() { tag = 0; state = 0; dirty = 0; }
  virtual void init(uint64_t addr) { tag = (addr >> TOfst) & mask; state = 0; dirty = 0; }
  virtual uint64_t addr(uint32_t s) const {
    uint64_t addr = tag << TOfst;
    if constexpr (IW > 0) {
      constexpr uint32_t index_mask = (1 << IW) - 1;
      addr |= (s & index_mask) << (TOfst - IW);
    }
    return addr;
  }
};

typedef OuterCohPortUncachedBase OuterPortMSIUncached;
typedef OuterCohPortBase OuterPortMSI;
typedef InnerCohPortUncachedBase InnerPortMSIUncached;
typedef InnerCohPortBase InnerPortMSI;
typedef CoreInterfaceBase CoreInterfaceMSI;

#endif
