#ifndef CM_CACHE_MESI_HPP
#define CM_CACHE_MESI_HPP

// support only directory based outer

#include "cache/msi.hpp"

template <typename BT>
  requires C_SAME(BT, MetadataBroadcastBase) || C_SAME(BT, MetadataDirectoryBase)
class MetadataMESIBase : public BT
{
public:
  MetadataMESIBase(): BT() {}
  virtual ~MetadataMESIBase() {}

private:
  virtual void to_owned(int32_t coh_id) {}
};

template<typename MT, bool isLLC> requires C_DERIVE(MT, MetadataDirectoryBase)
class MESIPolicy : public MSIPolicy<MT, false, isLLC>
{
public:
  MESIPolicy() : MSIPolicy() {}
  virtual ~MESIPolicy() {}

  virtual void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase *meta_inner) const {
    assert(is_acquire(cmd));
    int32_t id = cmd.id;
    if(meta){
      if(is_fetch_read(cmd)) {
        meta->to_shared(id);
        if(meta->is_exclusive_sharer(id)) { // add the support for exclusive
          meta->to_exclusive(id);
          if(meta_inner) meta_inner->to_exclusive(-1);
        } else
          if(meta_inner) meta_inner->to_shared(-1);
      } else {
        assert(is_fetch_write(cmd));
        meta->to_modified(id);
        if(meta_inner) meta_inner->to_modified(-1);
      }
    }
  }

};

template<typename MT, bool isLLC> requires C_DERIVE(MT, MetadataBroadcastBase)
using class ExclusiveMESIPolicy = ExclusiveMSIPolicy<MT, false, isLLC>

#endif
