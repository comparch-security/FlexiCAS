#ifndef CM_CACHE_MI_HPP
#define CM_CACHE_MI_HPP

#include "cache/coh_policy.hpp"

class MetadataMIBase : public CMMetadataBase
{
private:
  virtual void to_shared(int32_t coh_id) override {}
  virtual void to_owned(int32_t coh_id) override {}
  virtual void to_exclusive(int32_t coh_id) override {}
};

typedef MetadataMIBase MetadataMI;

template <int AW, int IW, int TOfst>
using MetadataMIBroadcast = MetadataBroadcast<AW, IW, TOfst, MetadataMIBase>;

template<bool isL1, bool uncached, typename Outer> requires C_DERIVE<Outer, CohPolicyBase>
struct MIPolicy : public CohPolicyBase
{
  constexpr static __always_inline bool is_uncached() { return uncached; }

  constexpr static __always_inline bool sync_need_lock() { return !(uncached || isL1); }

  static __always_inline coh_cmd_t cmd_for_outer_acquire(coh_cmd_t cmd) { return coh::cmd_for_write(); }

  static __always_inline std::pair<bool, coh_cmd_t> access_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) {
    if constexpr (isL1) return std::make_pair(false, coh::cmd_for_null());
    else                return std::make_pair(true,  coh::cmd_for_probe_release(cmd.id));
  }

  static __always_inline std::tuple<bool, bool, coh_cmd_t> access_need_promote(coh_cmd_t cmd, const CMMetadataBase *meta) {
    return std::make_tuple(false, false, coh::cmd_for_null());
  }

  static __always_inline void meta_after_fetch(coh_cmd_t outer_cmd, CMMetadataBase *meta, uint64_t addr) {
    meta->init(addr);
    assert(coh::is_fetch_write(outer_cmd) && meta->allow_write());
    meta->to_modified(-1);
  }

  static __always_inline void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase *meta_inner) {
    meta->to_modified(cmd.id);
    meta_inner->to_modified(-1);
  }

  static __always_inline std::pair<bool, coh_cmd_t> probe_need_sync(coh_cmd_t outer_cmd, const CMMetadataBase *meta) {
    if constexpr (!isL1) {
      assert(coh::is_probe(outer_cmd));
      if(coh::is_evict(outer_cmd) || coh::is_downgrade(outer_cmd))
        return std::make_pair(true, coh::cmd_for_probe_release());
      else
        return std::make_pair(true, coh::cmd_for_probe_writeback());
    } else return std::make_pair(false, coh::cmd_for_null());
  }

  static __always_inline void meta_after_probe(coh_cmd_t outer_cmd, CMMetadataBase *meta, CMMetadataBase* meta_outer, int32_t inner_id, bool writeback) {
    CohPolicyBase::meta_after_probe(outer_cmd, meta, meta_outer, inner_id, writeback);
    if(meta) {
      if(coh::is_evict(outer_cmd) || coh::is_downgrade(outer_cmd)) meta->to_invalid();
    }
  }

  static __always_inline std::pair<bool, coh_cmd_t> writeback_need_writeback(const CMMetadataBase *meta) {
    if(meta->is_dirty())
      return std::make_pair(true, coh::cmd_for_release());
    else {
      if constexpr (!uncached)
        return Outer::inner_need_release();
      else
        return std::make_pair(false, coh::cmd_for_null());
    }
  }

  static __always_inline std::pair<bool, coh_cmd_t> flush_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) {
    assert(uncached);
    if constexpr (!isL1){
      if(meta){
        if(coh::is_evict(cmd)) return std::make_pair(true,  coh::cmd_for_probe_release());
        else                   return std::make_pair(true,  coh::cmd_for_probe_writeback());
      } else                   return std::make_pair(false, coh::cmd_for_null());
    } else return std::make_pair(false, coh::cmd_for_null());
  }

  static __always_inline std::pair<bool, coh_cmd_t> writeback_need_sync(const CMMetadataBase *meta) {
    if constexpr (isL1) return std::make_pair(false, coh::cmd_for_null());
    else                return std::make_pair(true,  coh::cmd_for_probe_release());
  }

};

#endif
