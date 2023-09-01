#ifndef CM_CACHE_COH_POLICY_HPP
#define CM_CACHE_COH_POLICY_HPP

#include <utility>
#include <cassert>
#include "cache/cache.hpp"

// generice coherence command type
// support up to 64K coherent entities, 256 message types and 256 action types
struct coh_cmd_t {
  // definition of command:
  // [31  :   16] [15 : 8] [7 :0]
  // coherence-id msg-type action
  int32_t id     : 16; // -1 for nobody
  uint32_t msg   : 8;
  uint32_t act   : 8;
};

// functions default on MSI
class CohPolicyBase {

protected:
  const uint32_t acquire_msg, release_msg, probe_msg, flush_msg;
  const uint32_t fetch_read_act, fetch_write_act, evict_act, writeback_act;

public:
  // constructor
  CohPolicyBase(uint32_t a, uint32_t r, uint32_t p, uint32_t f, uint32_t fr, uint32_t fw, uint32_t ev, uint32_t wb)
    : acquire_msg(a), release_msg(r), probe_msg(p), flush_msg(f), fetch_read_act(fr), fetch_write_act(fw), evict_act(ev), writeback_act(wb) {}
  virtual ~CohPolicyBase() {}

  // message type
  inline bool is_acquire(coh_cmd_t cmd) const     { return cmd.msg == acquire_msg;     }
  inline bool is_release(coh_cmd_t cmd) const     { return cmd.msg == release_msg;     }
  inline bool is_probe(coh_cmd_t cmd) const       { return cmd.msg == probe_msg;       }
  inline bool is_flush(coh_cmd_t cmd) const       { return cmd.msg == flush_msg;       }

  // action type
  inline bool is_fetch_read(coh_cmd_t cmd) const  { return cmd.act == fetch_read_act;  }
  inline bool is_fetch_write(coh_cmd_t cmd) const { return cmd.act == fetch_write_act; }
  inline bool is_evict(coh_cmd_t cmd) const       { return cmd.act == evict_act;       }
  inline bool is_writeback(coh_cmd_t cmd) const   { return cmd.act == write_back_act;  }

  // generate command for core interface
  inline coh_cmd_t cmd_for_read()      const { return {-1, acquire_msg, fetch_read_act}; }
  inline coh_cmd_t cmd_for_write()     const { return {-1, acquire_msg, fetch_write_act}; }
  inline coh_cmd_t cmd_for_flush()     const { return {-1, flush_msg,   evict_act}; }
  inline coh_cmd_t cmd_for_writeback() const { return {-1, flush_msg,   writeback_act}; }

  // acquire
  virtual std::pair<bool, coh_cmd_t> acquire_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const = 0;
  virtual std::pair<bool, coh_cmd_t> acquire_need_promote(coh_cmd_t cmd, const CMMetadataBase *meta) const = 0;
  virtual void meta_after_fetch(coh_cmd_t cmd, CMMetadataBase *meta, uint64_t addr) const = 0; // after fetch from outer
  virtual void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta) const = 0; // after grant to inner

  // probe
  virtual std::pair<bool, coh_cmd_t> probe_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const = 0;
  virtual std::pair<bool, coh_cmd_t> probe_need_probe(coh_cmd_t cmd, const CMMetadataBase *meta, uint32_t target_inner_id) const = 0;
  virtual void meta_after_probe(coh_cmd_t cmd, CMMetadataBase *meta) const = 0;

  // writeback due to conflict, probe, flush
  virtual std::pair<bool, coh_cmd_t> writeback_need_sync(const CMMetadataBase *meta) const = 0;
  virtual std::pair<bool, coh_cmd_t> writeback_need_writeback(const CMMetadataBase *meta) const {
    if(meta->is_dirty()) return std::make_pair(true, {-1, release_msg, evict_act});
    else                 return std::make_pair(false, {-1, 0, 0});
  }
  virtual void meta_after_writeback(coh_cmd_t cmd, CMMetadataBase *meta) const = 0;

  // release from inner
  virtual void meta_after_release(CMMetadataBase *meta) const { meta->to_dirty();}

  // flush
  virtual std::pair<bool, coh_cmd_t> flush_need_sync() const = 0;

private:
  inline std::pair<bool, coh_cmd_t> need_sync(const CMMetadataBase *meta, int32_t coh_id) const {
    if(meta->is_shared()) return std::make_pair(false, {-1, 0, 0});
    // for all other potential states (M, O, E), the inner cache holds the latest copy
    else                  return std::make_pair(true, {coh_id, probe_msg, writeback_act});
  }
};

#endif
