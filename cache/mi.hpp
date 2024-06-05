#ifndef CM_CACHE_MI_HPP
#define CM_CACHE_MI_HPP

#include "cache/coh_policy.hpp"

class MetadataMIBase : public CMMetadataBase
{
public:
  MetadataMIBase(): CMMetadataBase() {}
  virtual ~MetadataMIBase() {}

private:
  virtual void to_shared(int32_t coh_id) {}
  virtual void to_owned(int32_t coh_id) {}
  virtual void to_exclusive(int32_t coh_id) {}
};

typedef MetadataMIBase MetadataMI;

template <int AW, int IW, int TOfst>
using MetadataMIBroadcast = MetadataBroadcast<AW, IW, TOfst, MetadataMIBase>;

template<typename MT, bool isL1, bool isLLC> requires C_DERIVE(MT, CMMetadataBase)
class MIPolicy : public CohPolicyBase
{
protected:
  using CohPolicyBase::outer;
  using CohPolicyBase::cmd_for_probe_release;
  using CohPolicyBase::cmd_for_probe_writeback;
  using CohPolicyBase::cmd_for_null;
  using CohPolicyBase::is_evict;

public:
  virtual ~MIPolicy() {}

  virtual coh_cmd_t cmd_for_outer_acquire(coh_cmd_t cmd) const {
    return outer->cmd_for_write();
  }

  virtual std::pair<bool, coh_cmd_t> access_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const {
    return std::make_pair(true, cmd_for_probe_release(cmd.id));
  }

  virtual std::tuple<bool, bool, coh_cmd_t> access_need_promote(coh_cmd_t cmd, const CMMetadataBase *meta) const {
    return std::make_tuple(false, false, cmd_for_null());
  }

  virtual void meta_after_fetch(coh_cmd_t outer_cmd, CMMetadataBase *meta, uint64_t addr) const {
    meta->init(addr);
    assert(outer->is_fetch_write(outer_cmd) && meta->allow_write());
    meta->to_modified(-1);
  }

  virtual void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase *meta_inner) const {
    meta->to_modified(cmd.id);
    meta_inner->to_modified(-1);
  }

  virtual std::pair<bool, coh_cmd_t> probe_need_sync(coh_cmd_t outer_cmd, const CMMetadataBase *meta) const {
    if constexpr (!isL1) {
      assert(outer->is_probe(outer_cmd));
      if(outer->is_evict(outer_cmd) || outer->is_downgrade(outer_cmd))
        return std::make_pair(true, cmd_for_probe_release());
      else
        return std::make_pair(true, cmd_for_probe_writeback());
    } else return std::make_pair(false, cmd_for_null());
  }

  virtual void meta_after_probe(coh_cmd_t outer_cmd, CMMetadataBase *meta, CMMetadataBase* meta_outer, int32_t inner_id, bool writeback) const {
    CohPolicyBase::meta_after_probe(outer_cmd, meta, meta_outer, inner_id, writeback);
    if(meta) {
      if(outer->is_evict(outer_cmd) || outer->is_downgrade(outer_cmd)) meta->to_invalid();
    }
  }

  virtual std::tuple<bool, bool, coh_cmd_t> flush_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta, bool uncached) const {
    if (isLLC || uncached) {
      if(meta){
        if(is_evict(cmd)) return std::make_tuple(true, true, cmd_for_probe_release());
        else              return std::make_tuple(true, true, cmd_for_probe_writeback());
      } else              return std::make_tuple(true, false, cmd_for_null());
    } else
      return std::make_tuple(false, false, cmd_for_null());
  }

};

#endif
