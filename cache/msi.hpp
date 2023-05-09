#ifndef CM_CACHE_MSI_HPP
#define CM_CACHE_MSI_HPP

#include "cache/coherence.hpp"

/////////////////////////////////
// Define a group of static (singlton) class for interprating the command of actions
// for different coherence policies

// MSI protocol
class CohPolicyMSI {
public:
  static uint32_t cmd_probe_to_share();
  static uint32_t cmd_probe_to_invalid();
  static bool is_probe_to_share(uint32_t cmd);
  static bool is_probe_to_invalid(uint32_t cmd);
};

/////////////////////////////////
// Implement the MSI protocol

// Outer port for MSI, uncached, no support for reverse probe as if there is no internal cache
// or the interl cache does not participate in the coherence communication
template<typename MT, typename DT>
class OuterPortMSIUncached : public OuterCohPortBase
{
public:
  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd) {
    coh->acquire_resp(addr, meta, data, cmd);
  }
  virtual void writeback_req(uint64_t addr, CMDataBase *data) {
    coh->writeback_resp(addr, data);
  }
};

// Outer port for MSI, cached, support reverse probes
template<typename MT, typename DT>
class OuterPortMSI : public OuterPortMSIUncached<MT, DT>
{
public:
  virtual void probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, uint32_t cmd) {
    uint32_t ai, s, w;
    if(this->cache->hit(addr, &ai, &s, &w)) {
      MT *meta = this->cache->access(ai, s, w); // oddly here, `this->' is required by the g++

      if(meta->is_dirty()) { // dirty, writeback
        meta_outer->to_dirty();
        if(data_outer) data_outer->copy(this->cache->get_data(ai, s, w));
        meta->to_clean();
      }

      if(CohPolicyMSI::is_probe_to_share(cmd))
        meta->to_shared();
      else if(CohPolicyMSI::is_probe_to_invalid(cmd))
        meta->to_invalid();
    }
  }
};

#endif
