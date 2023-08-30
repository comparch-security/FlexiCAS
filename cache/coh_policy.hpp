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

// coherence command decoder
class CohCMDDecoderBase {
public:
  // message type
  virtual bool is_acquire(coh_cmd_t cmd) const = 0;
  virtual bool is_release(coh_cmd_t cmd) const = 0;
  virtual bool is_probe(coh_cmd_t cmd) const = 0;
  virtual bool is_flush(coh_cmd_t cmd) const = 0;

  // action type
  virtual bool is_fetch_read(coh_cmd_t cmd) const = 0;
  virtual bool is_fetch_write(coh_cmd_t cmd) const = 0;
  virtual bool is_evict(coh_cmd_t cmd) const = 0;
  virtual bool is_writeback(coh_cmd_t cmd) const = 0;
};

class CohPolicyBase {
public:

  // acquire
  std::pair<bool, coh_cmd_t> acquire_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const = 0;
  std::pair<bool, coh_cmd_t> acquire_need_promote(coh_cmd_t cmd, const CMMetadataBase *meta) const = 0;
  void meta_after_sync(coh_cmd_t cmd, CMMetadataBase *meta) const = 0; // after probe back from inner
  void meta_after_fetch(coh_cmd_t cmd, CMMetadataBase *meta, uint64_t addr) const = 0; // after fetch from outer
  void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta) const = 0; // after grant to inner

  // probe
  std::pair<bool, coh_cmd_t> probe_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const = 0;
  std::pair<bool, coh_cmd_t> probe_need_probe(coh_cmd_t cmd, const CMMetadataBase *meta, uint32_t coh_id) const = 0;
  void meta_after_probe(coh_cmd_t cmd, CMMetadataBase *meta) const = 0;

  // writeback due to conflict, probe, flush
  std::pair<bool, coh_cmd_t> writeback_need_sync(const CMMetadataBase *meta) const = 0;
  std::pair<bool, coh_cmd_t> writeback_need_writeback(const CMMetadataBase *meta) const = 0;
  void meta_after_writeback(coh_cmd_t cmd, CMMetadataBase *meta) const = 0;

  // release from inner
  void meta_after_release(coh_cmd_t cmd, CMMetadataBase *meta) const = 0;

  // flush
  std::pair<bool, coh_cmd_t> flush_need_sync() const = 0;
};



#endif
