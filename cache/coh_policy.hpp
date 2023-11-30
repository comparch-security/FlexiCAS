#ifndef CM_CACHE_COH_POLICY_HPP
#define CM_CACHE_COH_POLICY_HPP

#include <utility>
#include <tuple>
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
  CohPolicyBase *outer;    // the outer policy need for command translation

  static const uint32_t acquire_msg = 1;
  static const uint32_t release_msg = 2;
  static const uint32_t probe_msg   = 3;
  static const uint32_t flush_msg   = 4;
  static const uint32_t fetch_read_act  = 0;
  static const uint32_t fetch_write_act = 1;
  static const uint32_t evict_act       = 2;
  static const uint32_t writeback_act   = 3;
  static const uint32_t downgrade_act   = 4;

public:
  virtual ~CohPolicyBase() {}

  friend CoherentCacheBase; // deferred assignment for cache

  void connect(CohPolicyBase *policy) { outer = policy ? policy : this; } // memory does not use policy and returns nullptr

  // message type
  bool is_acquire(coh_cmd_t cmd) const     { return cmd.msg == acquire_msg;     }
  bool is_release(coh_cmd_t cmd) const     { return cmd.msg == release_msg;     }
  bool is_probe(coh_cmd_t cmd) const       { return cmd.msg == probe_msg;       }
  bool is_flush(coh_cmd_t cmd) const       { return cmd.msg == flush_msg;       }

  // action type
  bool is_fetch_read(coh_cmd_t cmd) const      { return cmd.act == fetch_read_act;  }
  bool is_fetch_write(coh_cmd_t cmd) const     { return cmd.act == fetch_write_act; }
  bool is_evict(coh_cmd_t cmd) const           { return cmd.act == evict_act;       }
  bool is_outer_evict(coh_cmd_t cmd) const     { return outer->is_evict(cmd);       }
  bool is_writeback(coh_cmd_t cmd) const       { return cmd.act == writeback_act;   }
  bool is_outer_writeback(coh_cmd_t cmd) const { return outer->is_writeback(cmd);   }
  bool is_downgrade(coh_cmd_t cmd) const       { return cmd.act == downgrade_act;   }
  bool is_write(coh_cmd_t cmd) const           { return cmd.act == fetch_write_act || cmd.act == evict_act || cmd.act == writeback_act; }

  // generate command
  constexpr coh_cmd_t cmd_for_read()             const { return {-1, acquire_msg, fetch_read_act }; }
  constexpr coh_cmd_t cmd_for_write()            const { return {-1, acquire_msg, fetch_write_act}; }
  constexpr coh_cmd_t cmd_for_flush()            const { return {-1, flush_msg,   evict_act      }; }
  constexpr coh_cmd_t cmd_for_writeback()        const { return {-1, flush_msg,   writeback_act  }; }
  constexpr coh_cmd_t cmd_for_release()          const { return {-1, release_msg, evict_act      }; }
  constexpr coh_cmd_t cmd_for_null()             const { return {-1, 0,           0              }; }
  constexpr coh_cmd_t cmd_for_probe_writeback()  const { return {-1, probe_msg,   writeback_act  }; }
  constexpr coh_cmd_t cmd_for_probe_release()    const { return {-1, probe_msg,   evict_act      }; }
  constexpr coh_cmd_t cmd_for_probe_downgrade()  const { return {-1, probe_msg,   downgrade_act  }; }
  coh_cmd_t cmd_for_probe_writeback(int32_t id)  const { return {id, probe_msg,   writeback_act  }; }
  coh_cmd_t cmd_for_probe_release(int32_t id)    const { return {id, probe_msg,   evict_act      }; }
  coh_cmd_t cmd_for_probe_downgrade(int32_t id)  const { return {id, probe_msg,   downgrade_act  }; }

  virtual coh_cmd_t cmd_for_outer_acquire(coh_cmd_t cmd) const = 0;

  coh_cmd_t cmd_for_outer_flush(coh_cmd_t cmd) const {
    if(is_evict(cmd)) return outer->cmd_for_flush();
    else              return outer->cmd_for_writeback();
  }

  // acquire
  virtual std::pair<bool, coh_cmd_t> access_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const = 0;
  virtual std::pair<bool, coh_cmd_t> access_need_promote(coh_cmd_t cmd, const CMMetadataBase *meta) const = 0;

  // update meta after fetching from outer cache
  virtual void meta_after_fetch(coh_cmd_t outer_cmd, CMMetadataBase *meta, uint64_t addr) const = 0;

  // update meta after grant to inner
  virtual void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase *meta_inner) const = 0;

  // probe
  virtual std::pair<bool, coh_cmd_t> probe_need_sync(coh_cmd_t outer_cmd, const CMMetadataBase *meta) const = 0;

  std::pair<bool, coh_cmd_t> probe_need_probe(coh_cmd_t cmd, const CMMetadataBase *meta, int32_t target_inner_id) const {
    assert(is_probe(cmd));
    if(meta) {
      if((is_evict(cmd) && meta->evict_need_probe(target_inner_id, cmd.id)) || meta->writeback_need_probe(target_inner_id, cmd.id) ) {
        cmd.id = -1;
        return std::make_pair(true, cmd);
      } else
        return std::make_pair(false, cmd_for_null());
    }
    else{
      cmd.id = -1;
      return std::make_pair(true, cmd);
    }
  }

  bool probe_need_writeback(coh_cmd_t outer_cmd, CMMetadataBase *meta){
    assert(outer->is_probe(outer_cmd));
    return meta->is_dirty();
  }

  virtual void meta_after_probe(coh_cmd_t outer_cmd, CMMetadataBase *meta, CMMetadataBase* meta_outer, int32_t inner_id, bool writeback) const {
    if(meta_outer) { // clean sharer if evict or miss
      if(writeback) {
        if(!meta_outer->is_valid()) {
          assert(meta);
          meta_outer->to_shared(-1);
          meta_outer->get_outer_meta()->copy(meta->get_outer_meta());
        }
        meta_outer->to_dirty();
      }
      if(outer->is_evict(outer_cmd) || !meta) meta_outer->sync(inner_id);
    }
  }

  // writeback due to conflict, probe, flush
  virtual std::pair<bool, coh_cmd_t> writeback_need_sync(const CMMetadataBase *meta) const {
    return std::make_pair(true, cmd_for_probe_release());
  }

  std::pair<bool, coh_cmd_t> writeback_need_writeback(const CMMetadataBase *meta, bool uncached) const {
    if(meta->is_dirty())
      return std::make_pair(true, outer->cmd_for_release());
    else if(!uncached)
      return outer->inner_need_release();
    else
      return std::make_pair(false, cmd_for_null());
  }

  void meta_after_writeback(coh_cmd_t outer_cmd, CMMetadataBase *meta) const {
    if(meta) meta->to_clean(); // flush may send out writeback request with null meta
  }

  void meta_after_evict(CMMetadataBase *meta) const{
    assert(!meta->is_dirty());
    meta->to_invalid();
  }

  // release from inner
  virtual std::pair<bool, coh_cmd_t> release_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta, const CMMetadataBase* meta_inner) const {
    return std::make_pair(false, cmd_for_null());
  }
  
  virtual void meta_after_release(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase* meta_inner) const {
    meta->to_dirty();
    if(meta_inner) {
      assert(is_release(cmd) && is_evict(cmd));
      meta_inner->to_invalid();
    }
  }

  // flush
  virtual std::tuple<bool, bool, coh_cmd_t> flush_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta, bool uncached) const = 0;

  void meta_after_flush(coh_cmd_t cmd, CMMetadataBase *meta) const  {
    if(meta && is_evict(cmd)) meta->to_invalid();
  }

  virtual std::pair<bool, coh_cmd_t> inner_need_release(){
    // here we suppose inner meta is clean and inner cache is asking outer cache if it need to be released
    return std::make_pair(false, cmd_for_null());
  }

};

#endif
