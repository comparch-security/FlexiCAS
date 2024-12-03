#ifndef CM_CACHE_EXCLUSIVE_HPP
#define CM_CACHE_EXCLUSIVE_HPP

#include "cache/msi.hpp"
#include "cache/coherence.hpp"

template<bool isL1, bool uncached, typename Outer, bool EnDir = false> requires (!isL1)
struct ExclusiveMSIPolicy : public MSIPolicy<false, uncached, Outer>    // always not L1
{
  static __always_inline std::pair<bool, coh_cmd_t> access_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) {
    if constexpr (!isL1){
      if(coh::is_fetch_write(cmd))    return std::make_pair(true, coh::cmd_for_probe_release(cmd.id));
      else                            return std::make_pair(true, coh::cmd_for_probe_downgrade(cmd.id));
    } else return std::make_pair(false, coh::cmd_for_null());
  }

  static __always_inline void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase *meta_inner) { // after grant to inner
    int32_t id = cmd.id;
    if constexpr (EnDir) {
      if(coh::is_fetch_read(cmd) || coh::is_prefetch(cmd)) {
        meta->to_shared(id);
        meta_inner->to_shared(-1);
      } else {
        meta->to_modified(id);
        meta_inner->to_modified(-1);
      }
    } else {
      if(coh::is_fetch_read(cmd) || coh::is_prefetch(cmd)) meta_inner->to_shared(-1);
      else                                                 meta_inner->to_modified(-1);

      if(id != -1) meta->to_invalid();
      else         meta->to_shared(-1); // as the inner does not exist, state is shared
    }
    assert(!meta_inner->is_dirty());
  }

  static __always_inline std::pair<bool, coh_cmd_t> writeback_need_sync(const CMMetadataBase *meta) {
    // for exclusive cache, no sync is needed for normal way, always sync for extended way
    if constexpr (isL1) return std::make_pair(false, coh::cmd_for_null());
    else                return meta->is_extend() ? std::make_pair(true, coh::cmd_for_probe_release()) : std::make_pair(false, coh::cmd_for_null());
  }

  static __always_inline void meta_after_release(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase* meta_inner) {
    if(cmd.id == -1) MSIPolicy<false, uncached, Outer>::meta_after_release(cmd, meta, meta_inner);
    else {
      meta->get_outer_meta()->copy(meta_inner);
      meta_inner->to_invalid();
      if constexpr (!EnDir) // need to validate meta when using the snoopying protocol
        meta->to_shared(-1);
    }
  }

  static __always_inline std::pair<bool, coh_cmd_t> release_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta, const CMMetadataBase* meta_inner) {
    // if the inner cache is not exclusive (M/O/E), probe to see whether there are other copies
    if constexpr (isL1) return std::make_pair(false, coh::cmd_for_null());
    else                return std::make_pair(!meta_inner->allow_write(), coh::cmd_for_probe_writeback(cmd.id));
  }

  static __always_inline std::pair<bool, coh_cmd_t> inner_need_release() {
    return std::make_pair(true, coh::cmd_for_release());
  }

  static __always_inline std::pair<bool, coh_cmd_t> flush_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) {
    assert(uncached);
    if constexpr (!isL1){
      if(coh::is_evict(cmd))             return std::make_pair(true,  coh::cmd_for_probe_release());
      else if(meta && meta->is_shared()) return std::make_pair(false, coh::cmd_for_null());
      else                               return std::make_pair(true,  coh::cmd_for_probe_writeback());
    } else return std::make_pair(false, coh::cmd_for_null());
  }
};

template<bool isL1, bool uncached, typename Outer, bool EnDir = true> requires (EnDir)
struct ExclusiveMESIPolicy : public ExclusiveMSIPolicy<isL1, uncached, Outer, EnDir>
{
  static __always_inline void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase *meta_inner) { // after grant to inner
    int32_t id = cmd.id;
    if(id != -1) { // inner cache is cached
      if(coh::is_fetch_read(cmd) || coh::is_prefetch(cmd)) {
        meta->to_shared(id);
        if(static_cast<MetadataDirectoryBase *>(meta)->is_exclusive_sharer(id)) { // add the support for exclusive
          meta->to_exclusive(id);
          meta_inner->to_exclusive(-1);
        } else
          meta_inner->to_shared(-1);
      } else {
        assert(coh::is_fetch_write(cmd));
        meta->to_modified(id);
        meta_inner->to_modified(-1);
      }
    } else { // acquire from an uncached inner, still cache it in normal way as the inner does not exist
      if(coh::is_fetch_read(cmd) || coh::is_prefetch(cmd)) meta_inner->to_shared(-1);
      else                                                 meta_inner->to_modified(-1);
      meta->to_shared(-1); // as the inner does not exist, state is shared
    }
    assert(!meta_inner->is_dirty());
  }

};

// Skewed Exclusive Cache
// IW: index width, NW: number of ways, DW: Directory Ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type, DRPC: directory replacer type(if use direcotry)
// EnMon: whether to enable monitoring
// EnDir: whether to enable use directory
template<int IW, int NW, int DW, int P, typename MT, typename DT, typename IDX, typename RPC, typename DRPC, typename DLY, bool EnMon, bool EnDir>
  requires (!EnDir || DW > 0)
class CacheSkewedExclusive : public CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon>
{
  typedef CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon> CacheT;
  using CacheT::indexer;
  using CacheT::loc_random;
  using CacheT::replacer;
  using CacheMonitorSupport::monitors;
protected:
  DRPC ext_replacer[P];

public:
  CacheSkewedExclusive(std::string name) : CacheT(name, 0, (EnDir ? DW : 0)) {}

  virtual bool replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, uint16_t prio, unsigned int genre = 0) override {
    this->replace_choose_set(addr, ai, s, genre);
    if constexpr (!EnDir) replacer[*ai].replace(*s, w);
    else if(0 == genre)   replacer[*ai].replace(*s, w);
    else                { ext_replacer[*ai].replace(*s, w); *w += NW; }
    return true; // ToDo: support multithread
  }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay) override {
    if(ai < P) {
      if constexpr (EnMon || !C_VOID<DLY>) monitors->hook_read(addr, ai, s, w,
                                                               (w >= NW ? ext_replacer[ai].eviction_rank(s, w-NW) : replacer[ai].eviction_rank(s, w)),
                                                               hit, meta, data, delay);
    } else {
      if constexpr (EnMon || !C_VOID<DLY>) monitors->hook_read(addr, -1, -1, -1, -1, hit, meta, data, delay);
    }
  }

  virtual void replace_read(uint32_t ai, uint32_t s, uint32_t w, bool prefetch, bool genre = false) override {
    if(ai < P) {
      if(w >= NW) ext_replacer[ai].access(s, w-NW, true, prefetch);
      else        replacer[ai].access(s, w, true, prefetch);
    }
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay) override {
    if(ai < P) {
      if constexpr (EnMon || !C_VOID<DLY>) monitors->hook_write(addr, ai, s, w,
                                                                (w >= NW ? ext_replacer[ai].eviction_rank(s, w-NW) : replacer[ai].eviction_rank(s, w)),
                                                                hit, meta, data, delay);
    } else {
      if constexpr (EnMon || !C_VOID<DLY>) monitors->hook_write(addr, -1, -1, -1, -1, hit, meta, data, delay);
    }
  }

  virtual void replace_write(uint32_t ai, uint32_t s, uint32_t w, bool demand_acc, bool genre = false) override {
    if(ai < P) {
      if(w >= NW) ext_replacer[ai].access(s, w-NW, demand_acc, false);
      else        replacer[ai].access(s, w, demand_acc, false);
    } 
  }

  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint32_t evict, bool writeback, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay) override {
    if(ai < P){
      if constexpr (EnMon || !C_VOID<DLY>) monitors->hook_manage(addr, ai, s, w,
                                                                (w >= NW ? ext_replacer[ai].eviction_rank(s, w-NW) : replacer[ai].eviction_rank(s, w)),
                                                                hit, evict, writeback, meta, data, delay);
    } else {
      if constexpr (EnMon || !C_VOID<DLY>) monitors->hook_manage(addr, -1, -1, -1, -1, hit, evict, writeback, meta, data, delay);
    }
  }

  virtual void replace_manage(uint32_t ai, uint32_t s, uint32_t w, bool hit, uint32_t evict, bool genre = false) override {
    if(ai < P){
      if(hit && evict) {
        if(w >= NW) ext_replacer[ai].invalid(s, w-NW);
        else        replacer[ai].invalid(s, w, evict == 2);
      }
    } 
  }
};

// Norm Exclusive Cache
// IW: index width, NW: number of ways
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type,
// EnMon: whether to enable monitoring
// EnDir: whether to enable use directory
template<int IW, int NW, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon>
using CacheNormExclusiveBroadcast = CacheSkewedExclusive<IW, NW, 0, 1, MT, DT, IDX, RPC, ReplaceRandom<1,1,true,true,false>, DLY, EnMon, false>;

template<typename Policy, bool EnMT>
class ExclusiveInnerCohPortUncachedBroadcast : public InnerCohPortUncached<Policy, EnMT>
{
protected:
  typedef InnerCohPortUncached<Policy, EnMT> BaseT;
  using InnerCohPortBase::cache;
  using InnerCohPortBase::outer;

public:
  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) override {
    auto [meta, data, ai, s, w, hit] = access_line(addr, cmd, XactPrio::acquire, delay);
    bool act_as_prefetch = coh::is_prefetch(cmd) && Policy::is_uncached(); // only tweak replace priority at the LLC accoridng to [Guo2022-MICRO]

    if (data_inner && data) data_inner->copy(data);
    Policy::meta_after_grant(cmd, meta, meta_inner);
    if(!act_as_prefetch || !hit) cache->replace_read(ai, s, w, act_as_prefetch);
    cache->hook_read(addr, ai, s, w, hit, meta, data, delay);

    cache->meta_return_buffer(meta);
    cache->data_return_buffer(data);

    this->finish_record(addr, coh::cmd_for_finish(cmd.id), !hit, meta, ai, s);
    if(cmd.id == -1) this->finish_resp(addr, coh::cmd_for_finish(cmd.id));
  }


protected:
  bool fetch_line(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) { // fetch a missing line
    bool probe_hit = false;
    bool probe_writeback = false;
    auto sync = Policy::access_need_sync(cmd, meta);
    if(sync.first) {
      std::tie(probe_hit, probe_writeback) = this->probe_req(addr, meta, data, sync.second, delay); // sync if necessary
      if(probe_writeback) { // always writeback the line as it likely(always??) dirty
        assert(meta->is_dirty());
        outer->writeback_req(addr, meta, data, coh::cmd_for_release_writeback(), delay);
      }
    }

    if(!probe_writeback) // need to fetch it from outer
      outer->acquire_req(addr, meta, data, coh::is_prefetch(cmd) ? cmd : Policy::cmd_for_outer_acquire(cmd), delay);

    if(probe_hit && !coh::is_write(cmd)) // manually maintain the coherence if there are other inner copies, must be share
      meta->get_outer_meta()->to_shared(-1);

    return probe_hit;
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, bool>
  access_line(uint64_t addr, coh_cmd_t cmd, uint16_t prio, uint64_t *delay) override { // common function for access a line in the cache
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;

    bool hit = cache->hit(addr, &ai, &s, &w, prio, true);
    if(hit) {
      std::tie(meta, data) = cache->access_line(ai, s, w);
      auto [promote, promote_local, promote_cmd] = Policy::access_need_promote(cmd, meta);
      if(promote) { // promote permission if needed
        outer->acquire_req(addr, meta, data, promote_cmd, delay);
        hit = false;
      }
      else if(promote_local) meta->to_modified(-1);
      if(cmd.id != -1 && coh::is_acquire(cmd) && meta->is_dirty()) // writeback the dirty data as the dirty bit would be lost
        outer->writeback_req(addr, meta, data, coh::cmd_for_release_writeback(), delay);
      return std::make_tuple(meta, data, ai, s, w, hit);
    } else { // miss
      meta = cache->meta_copy_buffer(); meta->init(addr); meta->get_outer_meta()->to_invalid();
      data = cache->data_copy_buffer();
      auto probe_hit = fetch_line(addr, meta, data, cmd, delay);
      if(cmd.id == -1 && !probe_hit) { // need to reserve a place in the normal way if there no cached copy
        uint32_t mai, ms, mw;
        cache->replace(addr, &mai, &ms, &mw, XactPrio::acquire);
        auto [mmeta, mdata] = cache->access_line(mai, ms, mw);
        if(mmeta->is_valid()) this->evict(mmeta, mdata, mai, ms, mw, delay);
        mmeta->init(addr); mmeta->copy(meta); meta->to_invalid();
        if(mdata) mdata->copy(data);
        cache->replace_write(mai, ms, mw, true);
        cache->hook_write(addr, mai, ms, mw, false, mmeta, mdata, delay); // a write occurred during the probe
        cache->meta_return_buffer(meta);
        cache->data_return_buffer(data);
        return std::make_tuple(mmeta, mdata, mai, ms, mw, hit);
      } else {
        return std::make_tuple(meta, data, ai, s, w, hit);
      }
    }
  }

  virtual void write_line(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) override {
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;

    if(cmd.id == -1) { // request from an uncached inner
      BaseT::write_line(addr, data_inner, meta_inner, cmd, delay);
      cache->meta_return_buffer(meta);
      cache->data_return_buffer(data);
      return;
    }

    bool probe_hit = false;
    bool probe_writeback = false;
    bool hit = cache->hit(addr, &ai, &s, &w, XactPrio::release, true); // do we really need to lock even we know it should not?
    assert(!hit); // must not hit

    // check whether there are other copies
    auto sync = Policy::release_need_sync(cmd, meta, meta_inner);
    if(sync.first) {
      meta = cache->meta_copy_buffer();
      data = cache->data_copy_buffer();
      std::tie(probe_hit, probe_writeback) = this->probe_req(addr, meta, data, sync.second, delay); // sync if necessary
      assert(!probe_writeback); // there should be no probe writeback
      cache->meta_return_buffer(meta);
      cache->data_return_buffer(data);
    }

    if(!probe_hit) { // exclusive cache handles a release only when there is no other sharer
      cache->replace(addr, &ai, &s, &w, XactPrio::release);
      std::tie(meta, data) = cache->access_line(ai, s, w);
      if(meta->is_valid()) this->evict(meta, data, ai, s, w, delay);
      if(data_inner && data) data->copy(data_inner);
      meta->init(addr); Policy::meta_after_release(cmd, meta, meta_inner);
      cache->replace_write(ai, s, w, true);
      cache->hook_write(addr, ai, s, w, false, meta, data, delay);
    }
  }

  virtual void flush_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) override {
    if constexpr (!Policy::is_uncached()) {
      outer->writeback_req(addr, nullptr, nullptr, coh::cmd_for_flush(), delay);
    } else {
      auto [hit, meta, data, ai, s, w] = this->check_hit_or_replace(addr, XactPrio::flush, false, delay);
      auto [probe, probe_cmd] = Policy::flush_need_sync(cmd, meta);

      if(!hit) {
        meta = cache->meta_copy_buffer(); meta->init(addr); meta->get_outer_meta()->to_invalid();
        data = cache->data_copy_buffer();
      }

      if(probe) {
        auto [phit, pwb] = this->probe_req(addr, meta, data, probe_cmd, delay); // sync if necessary
        if(pwb){
          cache->replace_write(ai, s, w, false);
          cache->hook_write(addr, ai, s, w, true, meta, data, delay); // a write occurred during the probe
        }
      }

      auto writeback = Policy::writeback_need_writeback(meta);
      if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty

      Policy::meta_after_flush(cmd, meta, cache);
      cache->replace_manage(ai, s, w, hit, (coh::is_evict(cmd) ? 2 : 0));
      cache->hook_manage(addr, ai, s, w, hit, (coh::is_evict(cmd) ? 2 : 0), writeback.first, meta, data, delay);

      if(!hit) {
        cache->meta_return_buffer(meta);
        cache->data_return_buffer(data);
      }
    }
  }

};

template<typename Policy, bool EnMT = false>
using ExclusiveInnerCohPortBroadcast = InnerCohPortT<ExclusiveInnerCohPortUncachedBroadcast, Policy, EnMT>;

// Norm Exclusive Cache with Extened Directory
// Assume extended directory meta is always supported when directory is used
// IW: index width, NW: number of ways, DW: Extended Directory Ways
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type, DRPC: directory replacer type(if use direcotry)
// EnMon: whether to enable monitoring
template<int IW, int NW, int DW, typename MT, typename DT, typename IDX, typename RPC, typename DRPC, typename DLY, bool EnMon>
using CacheNormExclusiveDirectory = CacheSkewedExclusive<IW, NW, DW, 1, MT, DT, IDX, RPC, DRPC, DLY, EnMon, true>;

template<typename Policy, bool EnMT>
class ExclusiveInnerCohPortUncachedDirectory : public InnerCohPortUncached<Policy, EnMT>
{
protected:
  typedef InnerCohPortUncached<Policy, EnMT> BaseT;
  using InnerCohPortBase::cache;
  using InnerCohPortBase::outer;

public:
  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) override {
    auto [meta, data, ai, s, w, hit] = access_line(addr, cmd, XactPrio::acquire, delay);
    bool act_as_prefetch = coh::is_prefetch(cmd) && Policy::is_uncached(); // only tweak replace priority at the LLC accoridng to [Guo2022-MICRO]

    if (data_inner && data) data_inner->copy(data);
    Policy::meta_after_grant(cmd, meta, meta_inner);
    if(!act_as_prefetch || !hit) cache->replace_read(ai, s, w, act_as_prefetch);
    cache->hook_read(addr, ai, s, w, hit, meta, data, delay);

    // difficult to know when data is borrowed from buffer, just return it.
    cache->data_return_buffer(data);

    this->finish_record(addr, coh::cmd_for_finish(cmd.id), !hit, meta, ai, s);
    if(cmd.id == -1) this->finish_resp(addr, coh::cmd_for_finish(cmd.id));
  }

protected:
  std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t>
  replace_line_ext(uint64_t addr, uint16_t prio, uint64_t *delay) {
    uint32_t ai, s, w;
    cache->replace(addr, &ai, &s, &w, prio, true);
    auto [meta, data] = cache->access_line(ai, s, w);
    data = cache->data_copy_buffer();
    if(meta->is_valid()) this->evict(meta, data, ai, s, w, delay);
    cache->data_return_buffer(data);
    return std::make_tuple(meta, nullptr, ai, s, w);
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, bool>
  access_line(uint64_t addr, coh_cmd_t cmd, uint16_t prio, uint64_t *delay) override { // common function for access a line in the cache
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;

    bool hit = cache->hit(addr, &ai, &s, &w, prio, true);
    if(hit) {
      std::tie(meta, data) = cache->access_line(ai, s, w);
      if(!meta->is_extend()) { // hit on normal way
        auto [promote, promote_local, promote_cmd] = Policy::access_need_promote(cmd, meta);
        if(promote) { // promote permission if needed
          outer->acquire_req(addr, meta, data, promote_cmd, delay);
          hit = false;
        }
        else if(promote_local) meta->to_modified(-1);
        if(cmd.id == -1) // request from an uncached inner
          return std::make_tuple(meta, data, ai, s, w, hit);
        else { // normal request, move it to an extended way
          if(meta->is_dirty()) // writeback the dirty data as the dirty bit would be lost
            outer->writeback_req(addr, meta, data, coh::cmd_for_release_writeback(), delay);
          auto [mmeta, mdata, mai, ms, mw] = replace_line_ext(addr, prio, delay);
          mmeta->init(addr); mmeta->copy(meta); meta->to_invalid();
          return std::make_tuple(mmeta, data, mai, ms, mw, hit);
        }
      } else { // hit on extended way
        bool phit = false;
        bool pwb = false;
        data = cache->data_copy_buffer();
        auto sync = Policy::access_need_sync(cmd, meta);
        if(sync.first) {
          std::tie(phit, pwb) = this->probe_req(addr, meta, data, sync.second, delay); // sync if necessary
          if(pwb) { // a write occurred during the probe, always write it back as it likely (always??) dirty
            assert(meta->is_dirty());
            outer->writeback_req(addr, meta, data, coh::cmd_for_release_writeback(), delay);
          }
        }
        if(!pwb) { // still get it from outer
          outer->acquire_req(addr, meta, data, coh::is_prefetch(cmd) ? cmd : Policy::cmd_for_outer_acquire(cmd), delay);
          hit = false;
        } else {
          auto [promote, promote_local, promote_cmd] = Policy::access_need_promote(cmd, meta);
          if(promote) { // get from inner but lack of permission
            outer->acquire_req(addr, meta, data, promote_cmd, delay);
            hit = false;
          }
          else if(promote_local) meta->to_modified(-1);
        }
        return std::make_tuple(meta, data, ai, s, w, hit);
      }
    } else { // miss
      if(cmd.id == -1) { // request from an uncached inner, fetch to a normal way
        cache->replace(addr, &ai, &s, &w, prio);
        std::tie(meta, data) = cache->access_line(ai, s, w);
        if(meta->is_valid()) this->evict(meta, data, ai, s, w, delay);
      } else { // normal request, fetch to an extended way
        std::tie(meta, data, ai, s, w) = replace_line_ext(addr, prio, delay);
        data = cache->data_copy_buffer();
      }
      outer->acquire_req(addr, meta, data, coh::is_prefetch(cmd) ? cmd : Policy::cmd_for_outer_acquire(cmd), delay); // fetch the missing block
      return std::make_tuple(meta, data, ai, s, w, hit);
    }
  }

  virtual void write_line(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) override {
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    bool hit;

    if(cmd.id == -1) { // request from an uncached inner
      std::tie(meta, data, ai, s, w, hit) = access_line(addr, cmd, XactPrio::release, delay);
      if(data_inner) data->copy(data_inner);
      Policy::meta_after_release(cmd, meta, meta_inner);
      if(meta->is_extend()) outer->writeback_req(addr, meta, data, coh::cmd_for_release_writeback(), delay); // writeback if dirty
      assert(meta_inner); // assume meta_inner is valid for all writebacks
      cache->replace_write(ai, s, w, true);
      cache->hook_write(addr, ai, s, w, hit, meta, data, delay);
      cache->data_return_buffer(data); // return it anyway
    } else {
      bool phit = false;
      bool pwb = false;
      hit = cache->hit(addr, &ai, &s, &w, XactPrio::release, true); assert(hit);
      std::tie(meta, data) = cache->access_line(ai, s, w); assert(meta->is_extend());
      data = cache->data_copy_buffer();
      auto sync = Policy::access_need_sync(cmd, meta);
      if(sync.first) {
        std::tie(phit, pwb) = this->probe_req(addr, meta, data, sync.second, delay); // sync if necessary
        assert(!pwb); // there should be no probe writeback
      }
      if(data_inner) data->copy(data_inner);
      if(!phit) { // exclusive cache handles a release only when there is no other sharer
        // move it to normal meta
        uint32_t mai, ms, mw;
        cache->replace(addr, &mai, &ms, &mw, XactPrio::release);
        auto [mmeta, mdata] = cache->access_line(mai, ms, mw);
        if(mmeta->is_valid()) this->evict(mmeta, mdata, mai, ms, mw, delay);
        mmeta->init(addr); mmeta->copy(meta); meta->to_invalid();
        if(data_inner && mdata) mdata->copy(data);
        Policy::meta_after_release(cmd, mmeta, meta_inner);
        cache->replace_write(mai, ms, mw, true);
        cache->hook_write(addr, mai, ms, mw, true, mmeta, mdata, delay);
      }
      cache->data_return_buffer(data);
    }
  }

  virtual void flush_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) override {
    if constexpr (!Policy::is_uncached()) {
      outer->writeback_req(addr, nullptr, nullptr, coh::cmd_for_flush(), delay);
    } else {
      auto [hit, meta, data, ai, s, w] = this->check_hit_or_replace(addr, XactPrio::flush, false, delay);
      auto [probe, probe_cmd] = Policy::flush_need_sync(cmd, meta);
      if(!hit) return;

      if(meta->is_extend()) data = cache->data_copy_buffer();

      if(probe) {
        auto [phit, pwb] = this->probe_req(addr, meta, data, probe_cmd, delay); // sync if necessary
        if(pwb){
          cache->replace_write(ai, s, w, false);
          cache->hook_write(addr, ai, s, w, true, meta, data, delay); // a write occurred during the probe
        }
      }

      auto writeback = Policy::writeback_need_writeback(meta);
      if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty

      Policy::meta_after_flush(cmd, meta, cache);
      cache->replace_manage(ai, s, w, hit, (coh::is_evict(cmd) ? 2 : 0));
      cache->hook_manage(addr, ai, s, w, hit, (coh::is_evict(cmd) ? 2 : 0), writeback.first, meta, data, delay);

      if(meta->is_extend()) cache->data_return_buffer(data);
    }
  }

};

template<typename Policy, bool EnMT = false>
using ExclusiveInnerCohPortDirectory = InnerCohPortT<ExclusiveInnerCohPortUncachedDirectory, Policy, EnMT>;

template<template <typename, bool> class OPUC, typename Policy, bool EnMT> requires C_DERIVE<OPUC<Policy, EnMT>, OuterCohPortBase >
class ExclusiveOuterCohPortBroadcastT : public OPUC<Policy, EnMT>
{
protected:
  using OuterCohPortBase::cache;
  using OuterCohPortBase::inner;
  using OuterCohPortBase::coh_id;
public:

  virtual std::pair<bool, bool> probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, coh_cmd_t outer_cmd, uint64_t *delay) override {
    uint32_t ai, s, w;
    bool writeback = false;
    bool hit = cache->hit(addr, &ai, &s, &w, XactPrio::probe, true);
    bool probe_hit = false;
    bool probe_writeback = false;
    CMMetadataBase* meta = nullptr;
    CMDataBase* data = nullptr;

    if(hit) {
      std::tie(meta, data) = cache->access_line(ai, s, w); // need c++17 for auto type infer
    } else {
      auto sync = Policy::probe_need_sync(outer_cmd, meta);
      meta = cache->meta_copy_buffer(); meta->init(addr); meta->get_outer_meta()->to_invalid();
      data = cache->data_copy_buffer();
      std::tie(probe_hit, probe_writeback) = inner->probe_req(addr, meta, data, sync.second, delay);
      if(probe_writeback){
        cache->replace_write(ai, s, w, false);
        cache->hook_write(addr, ai, s, w, true, meta, data, delay);
      }
    }

    if(hit || probe_writeback) {
      if((writeback = Policy::probe_need_writeback(outer_cmd, meta)))
        if(data_outer) data_outer->copy(data);
    }

    Policy::meta_after_probe(outer_cmd, meta, meta_outer, coh_id, writeback);
    cache->replace_manage(ai, s, w, hit, (coh::is_evict(outer_cmd) ? 1 : 0));
    cache->hook_manage(addr, ai, s, w, hit, (coh::is_evict(outer_cmd) ? 1 : 0), writeback, meta, data, delay);

    if(!hit) {
      cache->meta_return_buffer(meta);
      cache->data_return_buffer(data);
    }

    return std::make_pair(hit||probe_hit, writeback);
  }

};

template<typename Policy, bool EnMT = false>
using ExclusiveOuterCohPortBroadcast = ExclusiveOuterCohPortBroadcastT<OuterCohPort, Policy, EnMT>;

template<template <typename, bool> class OPUC, typename Policy, bool EnMT> requires C_DERIVE<OPUC<Policy, EnMT>, OuterCohPortBase>
class ExclusiveOuterCohPortDirectoryT : public OPUC<Policy, EnMT>
{
protected:
  using OuterCohPortBase::cache;
  using OuterCohPortBase::inner;
  using OuterCohPortBase::coh_id;
public:

  virtual std::pair<bool, bool> probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, coh_cmd_t outer_cmd, uint64_t *delay) override {
    uint32_t ai, s, w;
    bool writeback = false;
    bool hit = cache->hit(addr, &ai, &s, &w, XactPrio::probe, true);
    bool probe_hit = false;
    bool probe_writeback = false;
    CMMetadataBase* meta = nullptr;
    CMDataBase* data = nullptr;

    // ToDo: flush may not hit (hit on inner cache when broadcast)
    if(hit) {
      std::tie(meta, data) = cache->access_line(ai, s, w); // need c++17 for auto type infer
      if(meta->is_extend()) {
        auto sync = Policy::probe_need_sync(outer_cmd, meta);
        if(sync.first) {
          data = cache->data_copy_buffer();
          std::tie(probe_hit, probe_writeback) = inner->probe_req(addr, meta, data, sync.second, delay);
          if(probe_writeback){
            cache->replace_write(ai, s, w, false);
            cache->hook_write(addr, ai, s, w, true, meta, data, delay);
          }
        }
      }

      if(writeback = Policy::probe_need_writeback(outer_cmd, meta))
        if(data_outer) data_outer->copy(data);
    }

    Policy::meta_after_probe(outer_cmd, meta, meta_outer, coh_id, writeback);
    cache->replace_manage(ai, s, w, hit, (coh::is_evict(outer_cmd) ? 1 : 0));
    cache->hook_manage(addr, ai, s, w, hit, (coh::is_evict(outer_cmd) ? 1 : 0), writeback, meta, data, delay);

    if(hit && meta->is_extend()) {
      cache->data_return_buffer(data);
    }

    return std::make_pair(hit||probe_hit, writeback);
  }

};

template<typename Policy, bool EnMT = false>
using ExclusiveOuterCohPortDirectory = ExclusiveOuterCohPortDirectoryT<OuterCohPort, Policy, EnMT>;

#endif
