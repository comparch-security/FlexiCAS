#ifndef CM_CACHE_MSI_HPP
#define CM_CACHE_MSI_HPP

#include "cache/mi.hpp"
#include "util/util.hpp"
#include <cstdio>

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
  using CohPolicyBase::is_write;
  using CohPolicyBase::is_evict;
  using CohPolicyBase::is_writeback;
  using CohPolicyBase::is_downgrade;
  using CohPolicyBase::cmd_for_probe_release;
  using CohPolicyBase::cmd_for_probe_writeback;
  using CohPolicyBase::cmd_for_probe_downgrade;
  using CohPolicyBase::cmd_for_null;

public:
  virtual ~MSIPolicy() {}

  virtual coh_cmd_t cmd_for_outer_acquire(coh_cmd_t cmd) const {
    if(is_fetch_write(cmd) || is_evict(cmd) || is_writeback(cmd))
      return outer->cmd_for_write();
    else
      return outer->cmd_for_read();
  }

  virtual std::pair<bool, coh_cmd_t> access_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const {
    if(is_fetch_write(cmd)) return std::make_pair(true, cmd_for_probe_release(cmd.id));
    if(meta->is_shared())   return std::make_pair(false, cmd_for_null());
    return std::make_pair(true, cmd_for_probe_downgrade(cmd.id));
  }

  virtual std::tuple<bool, bool, coh_cmd_t> access_need_promote(coh_cmd_t cmd, const CMMetadataBase *meta) const {
    if(is_write(cmd)) {
      if(!meta->allow_write())       return std::make_tuple(true,  false, outer->cmd_for_write());
      else if(!meta->is_modified())  return std::make_tuple(false, true,  cmd_for_null()); // promote locally
    }
    return std::make_tuple(false, false, cmd_for_null());
  }

  virtual void meta_after_fetch(coh_cmd_t outer_cmd, CMMetadataBase *meta, uint64_t addr) const {
    meta->init(addr);
    if(outer->is_fetch_read(outer_cmd)) meta->to_shared(-1);
    else {
#ifndef NDEBUG 
      assert(outer->is_fetch_write(outer_cmd) && meta->allow_write());
#endif
      meta->to_modified(-1);
    }
  }

  virtual void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase *meta_inner) const {
    int32_t id = cmd.id;
    if(is_fetch_read(cmd)) {
      meta->to_shared(id);
      meta_inner->to_shared(-1);
    } else {
#ifndef NDEBUG 
      assert(is_fetch_write(cmd));
#endif
      meta->to_modified(id);
      meta_inner->to_modified(-1);
    }
  }

  virtual std::pair<bool, coh_cmd_t> probe_need_sync(coh_cmd_t outer_cmd, const CMMetadataBase *meta) const {
    if constexpr (!isL1) {
      if(outer->is_evict(outer_cmd))
        return std::make_pair(true, cmd_for_probe_release());
      else {
        if(meta && meta->is_shared())
          return std::make_pair(false, cmd_for_null());
        else if(outer->is_downgrade(outer_cmd))
          return std::make_pair(true, cmd_for_probe_downgrade());
        else
          return std::make_pair(true, cmd_for_probe_writeback());
      }
    } else return std::make_pair(false, cmd_for_null());
  }

  virtual void meta_after_probe(coh_cmd_t outer_cmd, CMMetadataBase *meta, CMMetadataBase* meta_outer, int32_t inner_id, bool writeback) const {
    CohPolicyBase::meta_after_probe(outer_cmd, meta, meta_outer, inner_id, writeback);
    if(meta) {
      if(outer->is_evict(outer_cmd))
        meta->to_invalid();
      else if(outer->is_downgrade(outer_cmd)) {
        meta->get_outer_meta()->to_shared(-1);
        meta->to_shared(-1);
        meta->to_clean();
      }
    }
  }

  virtual std::tuple<bool, bool, coh_cmd_t> flush_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta, bool uncached) const {
    if (isLLC || uncached) {
      if(meta) {
        if(is_evict(cmd)) return std::make_tuple(true, true, cmd_for_probe_release());
        else if(meta->is_shared())
          return std::make_tuple(true, false, cmd_for_null());
        else
          return std::make_tuple(true, true, cmd_for_probe_writeback());
      } else
        return std::make_tuple(true, false, cmd_for_null());
    } else
      return std::make_tuple(false, false, cmd_for_null());
  }
};

#endif
