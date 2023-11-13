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
using class MESIPolicy = MSIPolicy<MT, false, isLLC>

template<typename MT, bool isLLC> requires C_DERIVE(MT, MetadataBroadcastBase)
using class ExclusiveMESIPolicy = ExclusiveMSIPolicy<MT, false, isLLC>

#endif
