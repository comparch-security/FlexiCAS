#ifndef CM_CACHE_COH_POLICY_HPP
#define CM_CACHE_COH_POLICY_HPP

#include <utility>
#include "cache/cache.hpp"

// generice coherence command type
// support up to 64K coherent entities, 256 message types and 256 action types
struct coh_cmd_t {
  uint32_t id    : 16;
  uint32_t msg   : 8;
  uint32_t act   : 8;
};

class PolicyBase {
public:

  // acquire
  std::pair<bool, coh_cmd_t> local_acquire_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const = 0;
  std::pair<bool, coh_cmd_t> local_acquire_need_promote(coh_cmd_t cmd, const CMMetadataBase *meta) const = 0;
  void local_meta_after_sync(coh_cmd_t cmd, CMMetadataBase *meta) const = 0; // after probe back from inner
  void local_meta_after_fetch(coh_cmd_t cmd, CMMetadataBase *meta, uint64_t addr) const = 0; // after fetch from outer
  void local_meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta) const = 0; // after grant to inner

  // probe
  std::pair<bool, coh_cmd_t> local_probe_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const = 0;
  void local_meta_after_probe(coh_cmd_t cmd, CMMetadataBase *meta) const = 0;

  // writeback due to conflict, probe, flush
  std::pair<bool, coh_cmd_t> local_writeback_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const = 0;
  void local_meta_after_writeback(coh_cmd_t cmd, CMMetadataBase *meta) const = 0;

  // release from inner
  void local_meta_after_release(coh_cmd_t cmd, CMMetadataBase *meta) const = 0;

  coh_cmd_t cmd_attach_id(coh_cmd_t cmd, const uint32_t new_id) const {
    cmd.id = new_id; return cmd;
  }

};



#endif
