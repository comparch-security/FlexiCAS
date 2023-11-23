#ifndef CM_CACHE_MSI_HPP
#define CM_CACHE_MSI_HPP

#include "cache/mi.hpp"

// metadata supporting MSI coherency
template <typename BT>
  requires C_SAME(BT, MetadataBroadcastBase) || C_SAME(BT, MetadataDirectoryBase)
class MetadataMSIBase : public BT
{
public:
  MetadataMSIBase(): BT() {}
  virtual ~MetadataMSIBase() {}

private:
  virtual void to_owned(int32_t coh_id) {}
  virtual void to_exclusive(int32_t coh_id) {}
};

template <int AW, int IW, int TOfst>
using MetadataMSIBroadcast = MetadataBroadcast<AW, IW, TOfst, MetadataMSIBase<MetadataBroadcastBase> >;

template <int AW, int IW, int TOfst>
using MetadataMSIDirectory = MetadataDirectory<AW, IW, TOfst, MetadataMSIBase<MetadataDirectoryBase> >;

template<typename MT, bool isL1, bool isLLC> requires C_DERIVE(MT, MetadataBroadcastBase)
  class MSIPolicy : public MIPolicy<MT, isL1, isLLC>
{
  typedef MIPolicy<MT, isL1, isLLC> PolicT;
protected:
  using CohPolicyBase::outer;
  using CohPolicyBase::is_fetch_read;
  using CohPolicyBase::is_fetch_write;
  using CohPolicyBase::cmd_for_probe_release;
  using CohPolicyBase::cmd_for_null;
  using CohPolicyBase::need_sync;

public:
  MSIPolicy() : PolicT() {}
  virtual ~MSIPolicy() {}

  virtual coh_cmd_t cmd_for_outer_acquire(coh_cmd_t cmd) const {
    if(is_fetch_write(cmd)) return outer->cmd_for_write();
    else                    return outer->cmd_for_read();
  }

  virtual std::pair<bool, coh_cmd_t> acquire_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const {
    if(is_fetch_write(cmd)) return std::make_pair(true, cmd_for_probe_release(cmd.id));
    else                    return need_sync(meta, cmd.id);
  }

  virtual std::pair<bool, coh_cmd_t> acquire_need_promote(coh_cmd_t cmd, const CMMetadataBase *meta) const {
    if(is_fetch_write(cmd) && !meta->allow_write())
      return std::make_pair(true, outer->cmd_for_write());
    else return std::make_pair(false, cmd_for_null());
  }

  virtual void meta_after_fetch(coh_cmd_t outer_cmd, CMMetadataBase *meta, uint64_t addr) const {
    assert(outer->is_acquire(outer_cmd));
    if(meta){ // exclusive snooping cache use nullptr as meta when acquire outer
      meta->init(addr);
      if(outer->is_fetch_read(outer_cmd)) meta->to_shared(-1);
      else {
        assert(outer->is_fetch_write(outer_cmd) && meta->allow_write());
        meta->to_modified(-1);
      }
    }
  }

  virtual void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase *meta_inner) const {
    int32_t id = cmd.id;
    if(is_fetch_read(cmd)) {
      meta->to_shared(id);
      meta_inner->to_shared(-1);
    } else {
      assert(is_fetch_write(cmd));
      meta->to_modified(id);
      meta_inner->to_modified(-1);
    }
  }
};

#endif
