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

class CoherentCacheBase;

// functions default on MSI
class CohPolicyBase {

protected:
  CacheBase *cache;        // reverse pointer for the cache parent
  const uint32_t acquire_msg, release_msg, probe_msg, flush_msg;
  const uint32_t fetch_read_act, fetch_write_act, evict_act, writeback_act;
  CohPolicyBase *outer;    // the outer policy need for command translation

public:
  // constructor
  CohPolicyBase(uint32_t a, uint32_t r, uint32_t p, uint32_t f, uint32_t fr, uint32_t fw, uint32_t ev, uint32_t wb)
    : acquire_msg(a), release_msg(r), probe_msg(p), flush_msg(f), fetch_read_act(fr), fetch_write_act(fw), evict_act(ev), writeback_act(wb) {}
  virtual ~CohPolicyBase() {}

  friend CoherentCacheBase; // deferred assignment for cache

  inline void connect(CohPolicyBase *policy) { outer = policy ? policy : this; } // memory does not use policy and returns nullptr

  // message type
  inline bool is_acquire(coh_cmd_t cmd) const     { return cmd.msg == acquire_msg;     }
  inline bool is_release(coh_cmd_t cmd) const     { return cmd.msg == release_msg;     }
  inline bool is_probe(coh_cmd_t cmd) const       { return cmd.msg == probe_msg;       }
  inline bool is_flush(coh_cmd_t cmd) const       { return cmd.msg == flush_msg;       }

  // action type
  inline bool is_fetch_read(coh_cmd_t cmd) const  { return cmd.act == fetch_read_act;  }
  inline bool is_fetch_write(coh_cmd_t cmd) const { return cmd.act == fetch_write_act; }
  inline bool is_evict(coh_cmd_t cmd) const       { return cmd.act == evict_act;       }
  inline bool is_outer_evict(coh_cmd_t cmd) const { return outer->is_evict(cmd);       }
  inline bool is_writeback(coh_cmd_t cmd) const   { return cmd.act == writeback_act;  }

  // generate command
  inline coh_cmd_t cmd_for_read()              const { return {-1, acquire_msg, fetch_read_act }; }
  inline coh_cmd_t cmd_for_write()             const { return {-1, acquire_msg, fetch_write_act}; }
  inline coh_cmd_t cmd_for_flush()             const { return {-1, flush_msg,   evict_act      }; }
  inline coh_cmd_t cmd_for_writeback()         const { return {-1, flush_msg,   writeback_act  }; }
  inline coh_cmd_t cmd_for_release()           const { return {-1, release_msg, evict_act      }; }
  inline coh_cmd_t cmd_for_release_writeback() const { return {-1, release_msg, writeback_act  }; }
  inline coh_cmd_t cmd_for_null()              const { return {-1, 0,           0              }; }

  virtual coh_cmd_t cmd_for_outer_acquire(coh_cmd_t cmd) const = 0;
  virtual coh_cmd_t cmd_for_outer_flush(coh_cmd_t cmd) const = 0;

  // acquire
  virtual std::pair<bool, coh_cmd_t> acquire_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const = 0;
  virtual std::pair<bool, coh_cmd_t> acquire_need_promote(coh_cmd_t cmd, const CMMetadataBase *meta) const = 0;
  virtual void meta_after_fetch(coh_cmd_t outer_cmd, CMMetadataBase *meta, uint64_t addr) const { // after fetch from outer
    assert(outer->is_acquire(outer_cmd));
    if(meta){ // exclusive snooping cache use nullptr as meta when acquire outer
      assert(!meta->is_dirty());
      meta->init(addr);
      if(outer->is_fetch_read(outer_cmd)) meta->to_shared(-1);
      else {
        assert(outer->is_fetch_write(outer_cmd));
        meta->to_modified(-1);
      }
    }
  }

  virtual void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta) const { // after grant to inner
    assert(is_acquire(cmd));
    int32_t id = cmd.id;
    if(meta){
      if(is_fetch_read(cmd)) meta->to_shared(id);
      else {
        assert(is_fetch_write(cmd));
        meta->to_modified(id);
      }
    }
  }

  // probe
  virtual std::pair<bool, coh_cmd_t> probe_need_sync(coh_cmd_t outer_cmd, const CMMetadataBase *meta) const = 0;
  virtual std::pair<bool, coh_cmd_t> probe_need_probe(coh_cmd_t cmd, const CMMetadataBase *meta, int32_t target_inner_id) const = 0;
  virtual std::pair<bool, coh_cmd_t> probe_need_writeback(coh_cmd_t outer_cmd, CMMetadataBase *meta) = 0;
  virtual void meta_after_probe(coh_cmd_t outer_cmd, CMMetadataBase *meta, CMMetadataBase* meta_outer, int32_t inner_id) const {
    assert(outer->is_probe(outer_cmd));
    // meta and meta_outer may be nullptr
    if(meta_outer) outer->meta_after_probe_ack(outer_cmd, meta_outer, inner_id); // meta_outer needed to be inited
    if(meta){
      if(outer->is_evict(outer_cmd)) meta->to_invalid();
      else {
        assert(outer->is_writeback(outer_cmd));
        meta->to_shared(-1);
      }
    }
  }
  virtual void meta_after_probe_ack(coh_cmd_t cmd, CMMetadataBase *meta, int32_t inner_id) const{
    assert(is_probe(cmd));
    if(is_evict(cmd)) meta->sync(inner_id);
  }

  // writeback due to conflict, probe, flush
  virtual std::pair<bool, coh_cmd_t> writeback_need_sync(const CMMetadataBase *meta) const = 0;
  virtual std::pair<bool, coh_cmd_t> writeback_need_writeback(const CMMetadataBase *meta) const {
    if(meta->is_dirty())
      return std::make_pair(true, outer->cmd_for_release());
    else
      return outer->inner_need_release();
  }

  virtual void meta_after_writeback(coh_cmd_t outer_cmd, CMMetadataBase *meta) const {
    if(meta) meta->to_clean(); // flush may send out writeback request with null meta
  }
  virtual void meta_after_evict(CMMetadataBase *meta) const{
    assert(!meta->is_dirty());
    meta->to_invalid();
  }

  // release from inner
  virtual void meta_after_release(CMMetadataBase *meta) const { meta->to_dirty();}

  // flush
  virtual std::pair<bool, coh_cmd_t> flush_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const = 0;
  virtual void meta_after_flush(coh_cmd_t cmd, CMMetadataBase *meta) const  {
    if(meta && is_evict(cmd)) meta->to_invalid();
  }

  virtual std::pair<bool, coh_cmd_t> inner_need_release(){
    // here we suppose inner meta is clean and inner cache is asking outer cache if it need to be released
    return std::make_pair(false, cmd_for_null());
  }

protected:
  inline std::pair<bool, coh_cmd_t> need_sync(const CMMetadataBase *meta, int32_t coh_id) const {
    if(meta && meta->is_shared()) return std::make_pair(false, cmd_for_null());
    // for all other potential states (M, O, E), the inner cache holds the latest copy
    else                  return std::make_pair(true, coh_cmd_t{coh_id, probe_msg, writeback_act});
  }
};

class ExclusivePolicySupportBase
{
public:  
  virtual void meta_after_release(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase* directory_meta, uint64_t addr, bool dirty) = 0;

  virtual std::pair<bool, coh_cmd_t> release_need_probe(coh_cmd_t cmd, CMMetadataBase* meta) = 0;

};
#endif
