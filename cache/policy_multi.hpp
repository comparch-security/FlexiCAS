#ifndef CM_CACHE_POLICY_MULTI_HPP
#define CM_CACHE_POLICY_MULTI_HPP

#include "cache/mesi.hpp"

class CohPolicyMultiThreadSupport
{

protected:
  static const uint32_t acquire_msg = 1;
  static const uint32_t acquire_ack_act = 5; 

public:
  virtual ~CohPolicyMultiThreadSupport() {}

  constexpr coh_cmd_t cmd_for_acquire_ack() const { return {-1, acquire_msg, acquire_ack_act}; } 

  virtual bool acquire_need_unlock(coh_cmd_t cmd) const { return cmd.id == -1; }

  virtual std::pair<bool, coh_cmd_t> acquire_need_ack(bool uncached) const { 
    if(uncached) return std::make_pair(false, coh_cmd_t{-1, 0, 0});
    else         return std::make_pair(true, cmd_for_acquire_ack());
  }
};

template<typename MT, bool isL1, bool isLLC> requires C_DERIVE<MT, MetadataBroadcastBase>
class MSIMultiThreadPolicy : public MSIPolicy<MT, isL1, isLLC>, public CohPolicyMultiThreadSupport
{
public:
  virtual ~MSIMultiThreadPolicy() {}
};

template<typename MT, bool isL1, bool isLLC> requires C_DERIVE<MT, MetadataBroadcastBase> && !isL1
class MESIMultiThreadPolicy : public MESIPolicy<MT, isL1, isLLC>, public CohPolicyMultiThreadSupport
{
public:
  virtual ~MESIMultiThreadPolicy() {}
};


#endif