#ifndef CM_CACHE_COH_POLICY_HPP
#define CM_CACHE_COH_POLICY_HPP

#include <utility>
#include <tuple>
#include <cassert>
#include <cstdint>
#include "cache/metadata.hpp"

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

namespace coh {

  const uint32_t acquire_msg = 1;
  const uint32_t release_msg = 2;
  const uint32_t probe_msg   = 3;
  const uint32_t flush_msg   = 4;
  const uint32_t finish_msg  = 5;
  const uint32_t fetch_read_act  = 0;
  const uint32_t fetch_write_act = 1;
  const uint32_t evict_act       = 2;
  const uint32_t writeback_act   = 3;
  const uint32_t downgrade_act   = 4;
  const uint32_t prefetch_act    = 5;

  // message type
  constexpr inline bool is_acquire(coh_cmd_t cmd)        { return cmd.msg == acquire_msg;     }
  constexpr inline bool is_release(coh_cmd_t cmd)        { return cmd.msg == release_msg;     }
  constexpr inline bool is_probe(coh_cmd_t cmd)          { return cmd.msg == probe_msg;       }
  constexpr inline bool is_flush(coh_cmd_t cmd)          { return cmd.msg == flush_msg;       }
  constexpr inline bool is_finish(coh_cmd_t cmd)         { return cmd.msg == finish_msg;      }

  // action type
  constexpr inline bool is_fetch_read(coh_cmd_t cmd)     { return cmd.act == fetch_read_act;  }
  constexpr inline bool is_fetch_write(coh_cmd_t cmd)    { return cmd.act == fetch_write_act; }
  constexpr inline bool is_evict(coh_cmd_t cmd)          { return cmd.act == evict_act;       }
  constexpr inline bool is_writeback(coh_cmd_t cmd)      { return cmd.act == writeback_act;   }
  constexpr inline bool is_downgrade(coh_cmd_t cmd)      { return cmd.act == downgrade_act;   }
  constexpr inline bool is_write(coh_cmd_t cmd)          { return cmd.act == fetch_write_act || cmd.act == evict_act || cmd.act == writeback_act; }
  constexpr inline bool is_prefetch(coh_cmd_t cmd)       { return cmd.act == prefetch_act;    }

  // generate command
  constexpr inline coh_cmd_t cmd_for_read()              { return {-1, acquire_msg, fetch_read_act }; }
  constexpr inline coh_cmd_t cmd_for_write()             { return {-1, acquire_msg, fetch_write_act}; }
  constexpr inline coh_cmd_t cmd_for_prefetch()          { return {-1, acquire_msg, prefetch_act   }; }
  constexpr inline coh_cmd_t cmd_for_flush()             { return {-1, flush_msg,   evict_act      }; }
  constexpr inline coh_cmd_t cmd_for_writeback()         { return {-1, flush_msg,   writeback_act  }; }
  constexpr inline coh_cmd_t cmd_for_release()           { return {-1, release_msg, evict_act      }; }
  constexpr inline coh_cmd_t cmd_for_release_writeback() { return {-1, release_msg, writeback_act  }; }
  constexpr inline coh_cmd_t cmd_for_null()              { return {-1, 0,           0              }; }
  constexpr inline coh_cmd_t cmd_for_probe_writeback()   { return {-1, probe_msg,   writeback_act  }; }
  constexpr inline coh_cmd_t cmd_for_probe_release()     { return {-1, probe_msg,   evict_act      }; }
  constexpr inline coh_cmd_t cmd_for_probe_downgrade()   { return {-1, probe_msg,   downgrade_act  }; }
  inline coh_cmd_t cmd_for_probe_writeback(int32_t id)   { return {id, probe_msg,   writeback_act  }; }
  inline coh_cmd_t cmd_for_probe_release(int32_t id)     { return {id, probe_msg,   evict_act      }; }
  inline coh_cmd_t cmd_for_probe_downgrade(int32_t id)   { return {id, probe_msg,   downgrade_act  }; }
  inline coh_cmd_t cmd_for_finish(int32_t id)            { return {id, finish_msg,  0              }; }
}

class CacheBase;

// functions default on MI
struct CohPolicyBase {
  // static __always_inline coh_cmd_t cmd_for_outer_acquire(coh_cmd_t cmd);
  // acquire
  // static __always_inline std::pair<bool, coh_cmd_t> access_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta);
  // static __always_inline std::tuple<bool, bool, coh_cmd_t> access_need_promote(coh_cmd_t cmd, const CMMetadataBase *meta);

  // update meta after fetching from outer cache
  // static __always_inline void meta_after_fetch(coh_cmd_t outer_cmd, CMMetadataBase *meta, uint64_t addr);

  // update meta after grant to inner
  // static __always_inline void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase *meta_inner);

  // probe
  // static __always_inline std::pair<bool, coh_cmd_t> probe_need_sync(coh_cmd_t outer_cmd, const CMMetadataBase *meta);

  static __always_inline std::pair<bool, coh_cmd_t> probe_need_probe(coh_cmd_t cmd, const CMMetadataBase *meta, int32_t target_inner_id) {
    assert(coh::is_probe(cmd));
    if(meta) {
      if((coh::is_evict(cmd) && meta->evict_need_probe(target_inner_id, cmd.id)) || meta->writeback_need_probe(target_inner_id, cmd.id) ) {
        cmd.id = -1;
        return std::make_pair(true, cmd);
      } else
        return std::make_pair(false, coh::cmd_for_null());
    }
    else{
      cmd.id = -1;
      return std::make_pair(true, cmd);
    }
  }

  static __always_inline bool probe_need_writeback(coh_cmd_t outer_cmd, CMMetadataBase *meta) {
    assert(coh::is_probe(outer_cmd));
    return meta->is_dirty();
  }

  static __always_inline void meta_after_probe(coh_cmd_t outer_cmd, CMMetadataBase *meta, CMMetadataBase* meta_outer, int32_t inner_id, bool writeback) {
    if(meta_outer) { // clean sharer if evict or miss
      if(writeback) {
        if(!meta_outer->is_valid()) {
          assert(meta);
          meta_outer->to_shared(-1);
          meta_outer->get_outer_meta()->copy(meta->get_outer_meta());
        }
        meta_outer->to_dirty();
      }
      if(coh::is_evict(outer_cmd) || !meta) meta_outer->sync(inner_id);
    }
  }

  // // writeback due to conflict, probe, flush
  // static __always_inline std::pair<bool, coh_cmd_t> writeback_need_sync(const CMMetadataBase *meta) {
  //   return std::make_pair(true, coh::cmd_for_probe_release());
  // }

  // static __always_inline std::pair<bool, coh_cmd_t> writeback_need_writeback(const CMMetadataBase *meta);

  static __always_inline void meta_after_writeback(coh_cmd_t outer_cmd, CMMetadataBase *meta) {
    if(meta) meta->to_clean(); // flush may send out writeback request with null meta
  }

  static __always_inline void meta_after_evict(CMMetadataBase *meta) {
    assert(!meta->is_dirty());
    meta->to_invalid();
  }

  // release from inner
  static __always_inline std::pair<bool, coh_cmd_t> release_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta, const CMMetadataBase* meta_inner) {
    return std::make_pair(false, coh::cmd_for_null());
  }
  
  static __always_inline void meta_after_release(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase* meta_inner) {
    meta->to_dirty();
    if(meta_inner && coh::is_evict(cmd))
      meta_inner->to_invalid();
  }

  // flush
  // static __always_inline std::tuple<bool, bool, coh_cmd_t> flush_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta);

  static __always_inline void meta_after_flush(coh_cmd_t cmd, CMMetadataBase *meta, CacheBase *) {
    if(meta && coh::is_evict(cmd)) meta->to_invalid();
  }

  static __always_inline std::pair<bool, coh_cmd_t> inner_need_release() {
    // here we suppose inner meta is clean and inner cache is asking outer cache if it need to be released
    return std::make_pair(false, coh::cmd_for_null());
  }

};

#endif
