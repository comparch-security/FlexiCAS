#ifndef CM_CACHE_MESI_HPP
#define CM_CACHE_MESI_HPP

// support only directory based outer

#include "cache/msi.hpp"

template <typename BT> requires C_DERIVE(BT, MetadataDirectoryBase)
class MetadataMESIBase : public BT
{
public:
  MetadataMESIBase(): BT() {}
  virtual ~MetadataMESIBase() {}

private:
  virtual void to_owned(int32_t coh_id) {}
};

template <int AW, int IW, int TOfst>
using MetadataMESIDirectory = MetadataDirectory<AW, IW, TOfst, MetadataMESIBase<MetadataDirectoryBase> >;

template<typename MT, bool isL1, bool isLLC> requires C_DERIVE(MT, MetadataDirectoryBase) && !isL1
class MESIPolicy : public MSIPolicy<MT, false, isLLC>
{
  typedef MSIPolicy<MT, false, isLLC> PolicyT;
protected:
  using CohPolicyBase::is_fetch_read;
  using CohPolicyBase::is_fetch_write;

public:
  virtual ~MESIPolicy() {
    MT().to_exclusive(-1); // type check to make sure MT has a public to_exclusive() implementation
  }

  virtual void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase *meta_inner) const {
    int32_t id = cmd.id;
    if(is_fetch_read(cmd)) {
      meta->to_shared(id);
      if(static_cast<MetadataDirectoryBase *>(meta)->is_exclusive_sharer(id)) { // add the support for exclusive
        meta->to_exclusive(id);
        meta_inner->to_exclusive(-1);
      } else
        meta_inner->to_shared(-1);
    } else {
      assert(is_fetch_write(cmd));
      meta->to_modified(id);
      meta_inner->to_modified(-1);
    }
  }

};

#endif
