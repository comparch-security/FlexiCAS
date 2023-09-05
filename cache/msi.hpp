#ifndef CM_CACHE_MSI_HPP
#define CM_CACHE_MSI_HPP

#include "cache/coherence.hpp"

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

private:
  virtual void to_owned() {}
  virtual void to_exclusive() {}
};

class MetadataMSISupport // handle extra functions needed by MSI
{
public:
  virtual bool evict_need_probe(int32_t target_id, int32_t request_id) const { return target_id != request_id; }
  virtual bool writeback_need_probe(int32_t target_id, int32_t request_id) const { return target_id != request_id; } 
};

typedef MetadataMSISupport MetadataMSIBrodcast;

class MetadataMSIDirectory : public MetadataMSISupport
{
public:
  virtual bool evict_need_probe(int32_t target_id, int32_t request_id) const = 0; // ToDo
  virtual bool writeback_need_probe(int32_t target_id, int32_t request_id) const = 0; // ToDo
};

// Metadata with match function
// AW    : address width
// IW    : index width
// TOfst : tag offset
// ST    : MSI support
template <int AW, int IW, int TOfst, typename ST>
class MetadataMSI : public MetadataMSIBase, public ST
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

template<typename MT, bool isL1, bool isLLC,
         typename = typename std::enable_if<std::is_base_of<MetadataMSIBase, MT>::value>::type,  // MT <- MetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<MetadataMSISupport, MT>::value>::type>  // MT <- MetadataMSISupport
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

  virtual std::pair<bool, coh_cmd_t> writeback_need_sync(const CMMetadataBase *meta) const {
    if constexpr (!isL1) return std::make_pair(true, {-1, probe_msg, evict_act});
    else                 std::make_pair(false, {-1, 0, 0});
  }

  virtual std::pair<bool, coh_cmd_t> flush_need_sync() const {
    if constexpr (isLLC) {
      assert(is_flush(cmd));
      if(is_evict(cmd)) return std::make_pair(true, {-1, probe_msg, evict_act});
      else              return need_sync(meta, -1);
    } else
      return std::make_pair(false, {-1, 0, 0});
  }

};

typedef OuterCohPortUncachedBase OuterPortMSIUncached;
typedef OuterCohPortBase OuterPortMSI;
typedef InnerCohPortUncachedBase InnerPortMSIUncached;
typedef InnerCohPortBase InnerPortMSI;
typedef CoreInterfaceBase CoreInterfaceMSI;

#endif
