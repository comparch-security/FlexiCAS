#ifndef CM_CACHE_MSI_HPP
#define CM_CACHE_MSI_HPP

#include "cache/coh_policy.hpp"

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
class MSIPolicy : public CohPolicyBase
{
public:
  MSIPolicy() : CohPolicyBase(1, 2, 3, 4, 0, 1, 2, 3) {}
  virtual ~MSIPolicy() {}

  virtual coh_cmd_t cmd_for_outer_acquire(coh_cmd_t cmd) const {
    assert(is_acquire(cmd));
    if(is_fetch_write(cmd)) return outer->cmd_for_write();
    else                    return outer->cmd_for_read();
  }

  virtual coh_cmd_t cmd_for_outer_flush(coh_cmd_t cmd) const {
    assert(is_flush(cmd));
    if(is_evict(cmd)) return outer->cmd_for_flush();
    else              return outer->cmd_for_writeback();
  }

  virtual std::pair<bool, coh_cmd_t> acquire_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const {
    if constexpr (!isL1) {
      assert(is_acquire(cmd));
      if(is_fetch_write(cmd)) return std::make_pair(true, coh_cmd_t{cmd.id, probe_msg, evict_act});
      else                    return need_sync(meta, cmd.id);
    } else return std::make_pair(false, cmd_for_null());
  }

  virtual std::pair<bool, coh_cmd_t> acquire_need_promote(coh_cmd_t cmd, const CMMetadataBase *meta) const {
    if(is_fetch_write(cmd) && !meta->get_outer_meta()->allow_write())
      return std::make_pair(true, outer->cmd_for_write());
    else return std::make_pair(false, cmd_for_null());
  }

  virtual std::pair<bool, coh_cmd_t> probe_need_sync(coh_cmd_t outer_cmd, const CMMetadataBase *meta) const {
    if constexpr (!isL1) {
      assert(outer->is_probe(outer_cmd));
      if(outer->is_evict(outer_cmd)) return std::make_pair(true, coh_cmd_t{-1, probe_msg, evict_act});
      else                           return need_sync(meta, -1);
    } else return std::make_pair(false, cmd_for_null());
  }

  virtual std::pair<bool, coh_cmd_t> probe_need_probe(coh_cmd_t cmd, const CMMetadataBase *meta, int32_t target_inner_id) const {
    assert(is_probe(cmd));
    if(meta){
      auto meta_msi = static_cast<const MT *>(meta);
      if((is_evict(cmd)     && meta_msi->evict_need_probe(target_inner_id, cmd.id))     ||
        (is_writeback(cmd)  && meta_msi->writeback_need_probe(target_inner_id, cmd.id)) )
      {
        cmd.id = -1;
        return std::make_pair(true, cmd);
      } else
        return std::make_pair(false, cmd_for_null());
    }
    else{
      cmd.id = -1;
      return std::make_pair(true, cmd);
    }

  }

  virtual bool probe_need_writeback(coh_cmd_t outer_cmd, CMMetadataBase *meta){
    assert(outer->is_probe(outer_cmd));
    return meta->is_dirty();
  }
  

  virtual std::pair<bool, coh_cmd_t> writeback_need_sync(const CMMetadataBase *meta) const {
    if constexpr (!isL1) return std::make_pair(true, coh_cmd_t{-1, probe_msg, evict_act});
    else                 return std::make_pair(false, cmd_for_null());
  }

  virtual std::pair<bool, coh_cmd_t> flush_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const {
    if constexpr (isLLC) {
      assert(is_flush(cmd));
      if(is_evict(cmd)) return std::make_pair(true, coh_cmd_t{-1, probe_msg, evict_act});
      else              return need_sync(meta, -1);
    } else
      return std::make_pair(false, cmd_for_null());
  }

};

#endif
