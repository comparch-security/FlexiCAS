#ifndef CM_CACHE_MESI_HPP
#define CM_CACHE_MESI_HPP

// support only directory based outer

#include "cache/msi.hpp"

template <typename BT> requires C_DERIVE<BT, MetadataDirectoryBase>
class MetadataMESIBase : public BT
{
  virtual void to_owned(int32_t coh_id) override {}
};

template <int AW, int IW, int TOfst>
using MetadataMESIDirectory = MetadataDirectory<AW, IW, TOfst, MetadataMESIBase<MetadataDirectoryBase> >;

template<bool isL1, bool uncached, typename Outer> requires (!isL1)
struct MESIPolicy : public MSIPolicy<false, uncached, Outer>
{
  static __always_inline void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase *meta_inner) {
    int32_t id = cmd.id;
    if(coh::is_fetch_read(cmd) || coh::is_prefetch(cmd)) {
      meta->to_shared(id);
      if(static_cast<MetadataDirectoryBase *>(meta)->is_exclusive_sharer(id)) { // add the support for exclusive
        meta->to_exclusive(id);
        meta_inner->to_exclusive(-1);
      } else
        meta_inner->to_shared(-1);
    } else {
      assert(coh::is_fetch_write(cmd));
      meta->to_modified(id);
      meta_inner->to_modified(-1);
    }
  }

};

#endif
