#ifndef CM_CACHE_MSI_HPP
#define CM_CACHE_MSI_HPP

#include "cache/mi.hpp"

// metadata supporting MSI coherency
template <typename BT>
  requires C_SAME<BT, MetadataBroadcastBase> || C_SAME<BT, MetadataDirectoryBase>
class MetadataMSIBase : public BT
{
private:
  virtual void to_owned(int32_t coh_id) override {}
  virtual void to_exclusive(int32_t coh_id) override {}
};

template <int AW, int IW, int TOfst>
using MetadataMSIBroadcast = MetadataBroadcast<AW, IW, TOfst, MetadataMSIBase<MetadataBroadcastBase> >;

template <int AW, int IW, int TOfst>
using MetadataMSIDirectory = MetadataDirectory<AW, IW, TOfst, MetadataMSIBase<MetadataDirectoryBase> >;

template<bool isL1, bool uncached, typename Outer>
struct MSIPolicy : public MIPolicy<isL1, uncached, Outer>
{
  static __always_inline coh_cmd_t cmd_for_outer_acquire(coh_cmd_t cmd) {
    if(coh::is_fetch_write(cmd) || coh::is_evict(cmd) || coh::is_writeback(cmd))
      return coh::cmd_for_write();
    else
      return coh::cmd_for_read();
  }

  static __always_inline std::pair<bool, coh_cmd_t> access_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) {
    if constexpr (!isL1){
      if(coh::is_release(cmd))     return std::make_pair(false, coh::cmd_for_null()); // for inclusive cache, release is always hit and exclusive/modified
      if(coh::is_fetch_write(cmd)) return std::make_pair(true,  coh::cmd_for_probe_release(cmd.id));
      if(meta->is_shared())        return std::make_pair(false, coh::cmd_for_null());
      return std::make_pair(true, coh::cmd_for_probe_downgrade(cmd.id));
    } else return std::make_pair(false, coh::cmd_for_null());
  }

  static __always_inline std::tuple<bool, bool, coh_cmd_t> access_need_promote(coh_cmd_t cmd, const CMMetadataBase *meta) {
    if(coh::is_write(cmd)) {
      if(!meta->allow_write())       return std::make_tuple(true,  false, coh::cmd_for_write());
      else if(!meta->is_modified())  return std::make_tuple(false, true,  coh::cmd_for_null()); // promote locally
    }
    return std::make_tuple(false, false, coh::cmd_for_null());
  }

  static __always_inline void meta_after_fetch(coh_cmd_t outer_cmd, CMMetadataBase *meta, uint64_t addr) {
    meta->init(addr);
    if(coh::is_fetch_read(outer_cmd)||coh::is_prefetch(outer_cmd)) meta->to_shared(-1);
    else {
      assert(coh::is_fetch_write(outer_cmd) && meta->allow_write());
      meta->to_modified(-1);
    }
  }

  static __always_inline void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase *meta_inner) {
    int32_t id = cmd.id;
    if(coh::is_fetch_read(cmd) || coh::is_prefetch(cmd)) {
      meta->to_shared(id);
      meta_inner->to_shared(-1);
    } else {
      assert(coh::is_fetch_write(cmd));
      meta->to_modified(id);
      meta_inner->to_modified(-1);
    }
  }

  static __always_inline std::pair<bool, coh_cmd_t> probe_need_sync(coh_cmd_t outer_cmd, const CMMetadataBase *meta) {
    if constexpr (!isL1) {
      if(coh::is_evict(outer_cmd))
        return std::make_pair(true, coh::cmd_for_probe_release());
      else {
        if(meta && meta->is_shared())
          return std::make_pair(false, coh::cmd_for_null());
        else if(coh::is_downgrade(outer_cmd))
          return std::make_pair(true, coh::cmd_for_probe_downgrade());
        else
          return std::make_pair(true, coh::cmd_for_probe_writeback());
      }
    } else return std::make_pair(false, coh::cmd_for_null());
  }

  static __always_inline void meta_after_probe(coh_cmd_t outer_cmd, CMMetadataBase *meta, CMMetadataBase* meta_outer, int32_t inner_id, bool writeback) {
    MIPolicy<isL1, uncached, Outer>::meta_after_probe(outer_cmd, meta, meta_outer, inner_id, writeback);
    if(meta) {
      if(coh::is_evict(outer_cmd))
        meta->to_invalid();
      else if(coh::is_downgrade(outer_cmd)) {
        meta->get_outer_meta()->to_shared(-1);
        meta->to_shared(-1);
        meta->to_clean();
      }
    }
  }

  static __always_inline std::pair<bool, coh_cmd_t> flush_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) {
    assert(uncached);
    if constexpr (!isL1){
      if(meta) {
        if(coh::is_evict(cmd))     return std::make_pair(true,  coh::cmd_for_probe_release());
        else if(meta->is_shared()) return std::make_pair(false, coh::cmd_for_null());
        else                       return std::make_pair(true,  coh::cmd_for_probe_writeback());
      } else                       return std::make_pair(false, coh::cmd_for_null());
    } else return std::make_pair(false, coh::cmd_for_null());
  }
};

#endif
