#ifndef CM_CACHE_EXCLUSIVE_HPP
#define CM_CACHE_EXCLUSIVE_HPP

#include "cache/coherence.hpp"
#include "cache/msi.hpp"

template<typename MT, bool EnDir, bool isLLC> requires C_DERIVE(MT, CMMetadataBase)
class ExclusiveMSIPolicy : public MSIPolicy<MT, false, isLLC>    // always not L1
{
  typedef MSIPolicy<MT, false, isLLC> PolicyT;
protected:
  using CohPolicyBase::is_fetch_read;
  using CohPolicyBase::is_fetch_write;
  using CohPolicyBase::is_evict;
  using CohPolicyBase::cmd_for_null;
  using CohPolicyBase::cmd_for_probe_release;
  using CohPolicyBase::cmd_for_probe_writeback;
  using CohPolicyBase::cmd_for_probe_downgrade;
  using CohPolicyBase::cmd_for_release;
public:

  virtual std::pair<bool, coh_cmd_t> access_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const {
    if(is_fetch_write(cmd))    return std::make_pair(true, cmd_for_probe_release(cmd.id));
    else                       return std::make_pair(true, cmd_for_probe_downgrade(cmd.id));
  }

  virtual void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase *meta_inner) const { // after grant to inner
    int32_t id = cmd.id;
    if constexpr (EnDir) {
      if(is_fetch_read(cmd)) {
        meta->to_shared(id);
        meta_inner->to_shared(-1);
      } else {
        meta->to_modified(id);
        meta_inner->to_modified(-1);
      }
    } else if(id != -1) {
      meta_inner->copy(meta->get_outer_meta()); // delegate all permision to inner
      meta->to_invalid();
    } else { // acquire from an uncached inner, still cache it in normal way as the inner does not exist
      if(is_fetch_read(cmd)) meta_inner->to_shared(-1);
      else                   meta_inner->to_modified(-1);
      meta->to_shared(-1); // as the inner does not exist, state is shared
      assert(!meta_inner->is_dirty());
    }
  }

  virtual std::pair<bool, coh_cmd_t> writeback_need_sync(const CMMetadataBase *meta) const {
    // for exclusive cache, no sync is needed for normal way, always sync for extended way
    return meta->is_extend() ? std::make_pair(true, cmd_for_probe_release()) : std::make_pair(false, cmd_for_null());
  }

  virtual void meta_after_release(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase* meta_inner) const {
    if(cmd.id == -1) CohPolicyBase::meta_after_release(cmd, meta, meta_inner);
    else {
      meta->get_outer_meta()->copy(meta_inner);
      meta_inner->to_invalid();
      if constexpr (!EnDir) // need to validate meta when using the snoopying protocol
        meta->to_shared(-1);
    }
  }

  virtual std::pair<bool, coh_cmd_t> release_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta, const CMMetadataBase* meta_inner) const {
    // if the inner cache is not exclusive (M/O/E), probe to see whether there are other copies
    return std::make_pair(!meta_inner->allow_write(), cmd_for_probe_writeback(cmd.id));
  }

  virtual std::pair<bool, coh_cmd_t> inner_need_release(){
    return std::make_pair(true, cmd_for_release());
  }

  virtual std::tuple<bool, bool, coh_cmd_t> flush_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta, bool uncached) const {
    if (isLLC || uncached) {
      if(is_evict(cmd)) return std::make_tuple(true, true, cmd_for_probe_release());
      else if(meta && meta->is_shared())
        return std::make_tuple(true, false, cmd_for_null());
      else
        return std::make_tuple(true, true, cmd_for_probe_writeback());
    } else
      return std::make_tuple(false, false, cmd_for_null());
  }
};

template<typename MT, bool isLLC> requires C_DERIVE(MT, MetadataDirectoryBase)
class ExclusiveMESIPolicy : public ExclusiveMSIPolicy<MT, true, isLLC>
{
  typedef ExclusiveMSIPolicy<MT, true, isLLC> PolicyT;
protected:
  using CohPolicyBase::is_fetch_read;
  using CohPolicyBase::is_fetch_write;
public:

  virtual void meta_after_grant(coh_cmd_t cmd, CMMetadataBase *meta, CMMetadataBase *meta_inner) const { // after grant to inner
    int32_t id = cmd.id;
    if(id != -1) { // inner cache is cached
      if(is_fetch_read(cmd)) {
        meta->to_shared(id);
        if(static_cast<MetadataDirectoryBase *>(meta)->is_exclusive_sharer(id)) { // add the support for exclusive
          meta->to_exclusive(id);
          meta_inner->to_exclusive(-1);
        } else
          meta_inner->to_shared(-1);
      } else {
        assert(is_fetch_write(cmd));
        meta->to_modified(id);
        meta_inner->to_modified(-1);
      }
    } else { // acquire from an uncached inner, still cache it in normal way as the inner does not exist
      if(is_fetch_read(cmd)) meta_inner->to_shared(-1);
      else                   meta_inner->to_modified(-1);
      meta->to_shared(-1); // as the inner does not exist, state is shared
      assert(!meta_inner->is_dirty());
    }
  }

};

// Skewed Exclusive Cache
// IW: index width, NW: number of ways, DW: Directory Ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type, DRPC: directory replacer type(if use direcotry)
// EnMon: whether to enable monitoring
// EnDir: whether to enable use directory
template<int IW, int NW, int DW, int P, typename MT, typename DT, typename IDX, typename RPC, typename DRPC, typename DLY, bool EnMon, bool EnDir>
  requires !EnDir || DW > 0
class CacheSkewedExclusive : public CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon>
{
  typedef CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon> CacheSkewedT;
protected:
  DRPC ext_replacer[P];
public:
  CacheSkewedExclusive(std::string name = "") : CacheSkewedT(name, 0, (EnDir ? DW : 0)) {}

  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, unsigned int genre = 0) {
    if constexpr (EnDir) CacheSkewedT::replace(addr, ai, s, w, 0);
    else {
      if(0 == genre) CacheSkewedT::replace(addr, ai, s, w);
      else {
        if constexpr (P==1) *ai = 0;
        else                *ai = (cm_get_random_uint32() % P);
        *s = CacheSkewedT::indexer.index(addr, *ai);
        ext_replacer[*ai].replace(*s, w);
        *w += NW;
      }
    }
  }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, const CMDataBase *data, uint64_t *delay) {
    if(ai < P) {
      if(w >= NW) ext_replacer[ai].access(s, w-NW, false);
      else        CacheSkewedT::replacer[ai].access(s, w, false);
      if constexpr (EnMon || !C_VOID(DLY)) CacheSkewedT::monitors->hook_read(addr, ai, s, w, hit, data, delay);
    } else {
      if constexpr (EnMon || !C_VOID(DLY)) CacheSkewedT::monitors->hook_read(addr, -1, -1, -1, hit, data, delay);
    }
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool is_release, const CMDataBase *data, uint64_t *delay) {
    if(ai < P) {
      if(w >= NW) ext_replacer[ai].access(s, w-NW, is_release);
      else        CacheSkewedT::replacer[ai].access(s, w, is_release);
      if constexpr (EnMon || !C_VOID(DLY)) CacheSkewedT::monitors->hook_write(addr, ai, s, w, hit, data, delay);
    } else {
      if constexpr (EnMon || !C_VOID(DLY)) CacheSkewedT::monitors->hook_write(addr, -1, -1, -1, hit, data, delay);
    }
  }

  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, const CMDataBase *data, uint64_t *delay) {
    if(ai < P){
      if(hit && evict) {
        if(w >= NW) ext_replacer[ai].invalid(s, w-NW);
        else        CacheSkewedT::replacer[ai].invalid(s, w);
      }
      if constexpr (EnMon || !C_VOID(DLY)) CacheSkewedT::monitors->hook_manage(addr, ai, s, w, hit, evict, writeback, data, delay);
    } else {
      if constexpr (EnMon || !C_VOID(DLY)) CacheSkewedT::monitors->hook_manage(addr, -1, -1, -1, hit, evict, writeback, data, delay);
    }
  }

  virtual ~CacheSkewedExclusive(){}
};

// Norm Exclusive Cache
// IW: index width, NW: number of ways
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type,
// EnMon: whether to enable monitoring
// EnDir: whether to enable use directory
template<int IW, int NW, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon>
using CacheNormExclusiveBroadcast = CacheSkewedExclusive<IW, NW, 0, 1, MT, DT, IDX, RPC, ReplaceRandom<1,1>, DLY, EnMon, false>;

class ExclusiveInnerPortUncachedBroadcast : public InnerCohPortUncached
{
protected:
  using InnerCohPortBase::cache;
public:
  ExclusiveInnerPortUncachedBroadcast(CohPolicyBase *policy) : InnerCohPortUncached(policy) {}
  virtual ~ExclusiveInnerPortUncachedBroadcast() {}

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) {
    auto [meta, data, ai, s, w, hit] = access_line(addr, cmd, delay);

    if (data_inner && data) data_inner->copy(data);
    policy->meta_after_grant(cmd, meta, meta_inner);
    cache->hook_read(addr, ai, s, w, hit, data, delay);

    if(!hit) {
      cache->meta_return_buffer(meta);
      cache->data_return_buffer(data);
    }
  }


protected:
  bool fetch_line(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) { // fetch a missing line
    bool probe_hit = false;
    bool probe_writeback = false;
    auto sync = policy->access_need_sync(cmd, meta);
    if(sync.first) {
      std::tie(probe_hit, probe_writeback) = probe_req(addr, meta, data, sync.second, delay); // sync if necessary
      if(probe_writeback) {
        // writeback the line as there will be no place to hold the dirty line
        outer->writeback_req(addr, meta, data, policy->cmd_for_outer_flush(cmd), delay);
      }
    }
    if(!probe_writeback)
      outer->acquire_req(addr, meta, data, policy->cmd_for_outer_acquire(cmd), delay);
    return probe_hit;
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, bool>
  access_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) { // common function for access a line in the cache
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;

    bool hit = cache->hit(addr, &ai, &s, &w);
    if(hit) {
      std::tie(meta, data) = cache->access_line(ai, s, w);
      auto promote = policy->access_need_promote(cmd, meta);
      if(promote.first) { // promote permission if needed
        outer->acquire_req(addr, meta, data, promote.second, delay);
        hit = false;
      }
      return std::make_tuple(meta, data, ai, s, w, hit);
    } else { // miss
      meta = cache->meta_copy_buffer(); meta->init(addr); meta->get_outer_meta()->to_invalid();
      data = cache->data_copy_buffer();
      auto probe_hit = fetch_line(addr, meta, data, cmd, delay);
      if(cmd.id == -1 && !probe_hit) { // need to reserve a place in the normal way if there no cached copy
        auto [mmeta, mdata, mai, ms, mw] = InnerCohPortUncached::replace_line(addr, delay);
        mmeta->init(addr); mmeta->copy(meta); meta->to_invalid();
        if(mdata) mdata->copy(data);
        cache->hook_write(addr, mai, ms, mw, false, false, data, delay); // a write occurred during the probe
        return std::make_tuple(mmeta, mdata, mai, ms, mw, hit);
      } else {
        return std::make_tuple(meta, data, ai, s, w, hit);
      }
    }
  }

  virtual void write_line(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;

    if(cmd.id == -1) { // request from an uncached inner
      InnerCohPortUncached::write_line(addr, data_inner, meta_inner, cmd, delay);
      cache->meta_return_buffer(meta);
      cache->data_return_buffer(data);
      return;
    }

    bool probe_hit = false;
    assert(!cache->hit(addr, &ai, &s, &w)); // must not hit

    // check whether there are other copies
    auto sync = policy->release_need_sync(cmd, meta, meta_inner);
    if(sync.first) {
      auto [probe_hit, pwb] = probe_req(addr, meta, data, sync.second, delay); // sync if necessary
      assert(!pwb); // there should be no probe writeback
    }

    if(!probe_hit) { // exclusive cache handles a release only when there is no other sharer
      std::tie(meta, data, ai, s, w) = InnerCohPortUncached::replace_line(addr, delay);
      if(data_inner && data) data->copy(data_inner);
      meta->init(addr); policy->meta_after_release(cmd, meta, meta_inner);
      cache->hook_write(addr, ai, s, w, false, true, data, delay);
    }
  }

  virtual void flush_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay){
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    bool hit = cache->hit(addr, &ai, &s, &w);
    if(hit) std::tie(meta, data) = cache->access_line(ai, s, w);

    auto [flush, probe, probe_cmd] = policy->flush_need_sync(cmd, meta, outer->is_uncached());
    if(!flush) return;

    if(!hit) {
      meta = cache->meta_copy_buffer(); meta->init(addr); meta->get_outer_meta()->to_invalid();
      data = cache->data_copy_buffer();
    }

    if(probe) {
      auto [phit, pwb] = probe_req(addr, meta, data, probe_cmd, delay); // sync if necessary
      if(pwb) cache->hook_write(addr, ai, s, w, true, true, data, delay); // a write occurred during the probe
    }

    auto writeback = policy->writeback_need_writeback(meta, outer->is_uncached());
    if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty

    policy->meta_after_flush(cmd, meta);
    cache->hook_manage(addr, ai, s, w, hit, policy->is_evict(cmd), writeback.first, data, delay);

    if(!hit) {
      cache->meta_return_buffer(meta);
      cache->data_return_buffer(data);
    }
  }

};

typedef InnerCohPortT<ExclusiveInnerPortUncachedBroadcast> ExclusiveInnerPortBroadcast;

// Norm Exclusive Cache with Extened Directory
// Assume extended directory meta is always supported when directory is used
// IW: index width, NW: number of ways, DW: Extended Directory Ways
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type, DRPC: directory replacer type(if use direcotry)
// EnMon: whether to enable monitoring
template<int IW, int NW, int DW, typename MT, typename DT, typename IDX, typename RPC, typename DRPC, typename DLY, bool EnMon>
using CacheNormExclusiveDirectory = CacheSkewedExclusive<IW, NW, DW, 1, MT, DT, IDX, RPC, DRPC, DLY, EnMon, true>;

class ExclusiveInnerPortUncachedDirectory : public InnerCohPortUncached
{
protected:
  using InnerCohPortBase::cache;
public:
  ExclusiveInnerPortUncachedDirectory(CohPolicyBase *policy) : InnerCohPortUncached(policy) {}
  virtual ~ExclusiveInnerPortUncachedDirectory() {}

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t outer_cmd, uint64_t *delay) {
    auto [meta, data, ai, s, w, hit] = access_line(addr, outer_cmd, delay);

    if (data_inner && data) data_inner->copy(data);
    policy->meta_after_grant(outer_cmd, meta, meta_inner);
    cache->hook_read(addr, ai, s, w, hit, data, delay);

    // difficult to know when data is borrowed from buffer, just return it.
    cache->data_return_buffer(data);
  }

protected:
  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t>
  replace_line_ext(uint64_t addr, uint64_t *delay) {
    uint32_t ai, s, w;
    cache->replace(addr, &ai, &s, &w, true);
    auto [meta, data] = cache->access_line(ai, s, w);
    data = cache->data_copy_buffer();
    if(meta->is_valid()) evict(meta, data, ai, s, w, delay);
    cache->data_return_buffer(data);
    return std::make_tuple(meta, nullptr, ai, s, w);
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, bool>
  access_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) { // common function for access a line in the cache
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;

    bool hit = cache->hit(addr, &ai, &s, &w);
    if(hit) {
      std::tie(meta, data) = cache->access_line(ai, s, w);
      if(!meta->is_extend()) { // hit on normal way
        auto promote = policy->access_need_promote(cmd, meta);
        if(promote.first) { // promote permission if needed
          outer->acquire_req(addr, meta, data, promote.second, delay);
          hit = false;
        }
        if(cmd.id == -1) // request from an uncached inner
          return std::make_tuple(meta, data, ai, s, w, hit);
        else { // normal request, move it to an extended way
          auto [mmeta, mdata, mai, ms, mw] = replace_line_ext(addr, delay);
          mmeta->init(addr); mmeta->copy(meta); meta->to_invalid();
          return std::make_tuple(mmeta, data, mai, ms, mw, hit);
        }
      } else { // hit on extended way
        bool phit = false;
        bool pwb = false;
        data = cache->data_copy_buffer();
        auto sync = policy->access_need_sync(cmd, meta);
        if(sync.first) {
          std::tie(phit, pwb) = probe_req(addr, meta, data, sync.second, delay); // sync if necessary
          if(pwb) cache->hook_write(addr, ai, s, w, true, true, data, delay); // a write occurred during the probe
        }
        if(!pwb) { // still get it from outer
          outer->acquire_req(addr, meta, data, policy->cmd_for_outer_acquire(cmd), delay);
          hit = false;
        } else {
          auto promote = policy->access_need_promote(cmd, meta);
          if(promote.first) { // get from inner but lack of permission
            outer->acquire_req(addr, meta, data, promote.second, delay);
            hit = false;
          }
        }
        return std::make_tuple(meta, data, ai, s, w, hit);
      }
    } else { // miss
      if(cmd.id == -1) // request from an uncached inner, fetch to a normal way
        std::tie(meta, data, ai, s, w) = InnerCohPortUncached::replace_line(addr, delay);
      else { // normal request, fetch to an extended way
        std::tie(meta, data, ai, s, w) = replace_line_ext(addr, delay);
        data = cache->data_copy_buffer();
      }
      outer->acquire_req(addr, meta, data, policy->cmd_for_outer_acquire(cmd), delay); // fetch the missing block
      return std::make_tuple(meta, data, ai, s, w, hit);
    }
  }

  virtual void write_line(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    bool hit;

    if(cmd.id == -1) { // request from an uncached inner
      std::tie(meta, data, ai, s, w, hit) = access_line(addr, cmd, delay);
      if(data_inner) data->copy(data_inner);
      policy->meta_after_release(cmd, meta, meta_inner);
      if(meta->is_extend()) outer->writeback_req(addr, meta, data, policy->cmd_for_outer_flush(cmd), delay); // writeback if dirty
      assert(meta_inner); // assume meta_inner is valid for all writebacks
      cache->hook_write(addr, ai, s, w, hit, true, data, delay);
      cache->data_return_buffer(data); // return it anyway
    } else {
      bool phit = false;
      bool pwb = false;
      hit = cache->hit(addr, &ai, &s, &w); assert(hit);
      std::tie(meta, data) = cache->access_line(ai, s, w); assert(meta->is_extend());
      data = cache->data_copy_buffer();
      auto sync = policy->access_need_sync(cmd, meta);
      if(sync.first) {
        std::tie(phit, pwb) = probe_req(addr, meta, data, sync.second, delay); // sync if necessary
        assert(!pwb); // there should be no probe writeback
      }
      if(data_inner) data->copy(data_inner);
      if(!phit) { // exclusive cache handles a release only when there is no other sharer
        // move it to normal meta
        auto [mmeta, mdata, mai, ms, mw] = InnerCohPortUncached::replace_line(addr, delay);
        mmeta->init(addr); mmeta->copy(meta); meta->to_invalid();
        if(data_inner && mdata) mdata->copy(data);
        policy->meta_after_release(cmd, mmeta, meta_inner);
        cache->hook_write(addr, mai, ms, mw, true, true, mdata, delay);
      }
      cache->data_return_buffer(data);
    }
  }

  virtual void flush_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay){
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    bool hit = cache->hit(addr, &ai, &s, &w);
    if(hit) std::tie(meta, data) = cache->access_line(ai, s, w);

    auto [flush, probe, probe_cmd] = policy->flush_need_sync(cmd, meta, outer->is_uncached());
    if(!flush || !hit) return;
    if(meta->is_extend()) data = cache->data_copy_buffer();

    if(probe) {
      auto [phit, pwb] = probe_req(addr, meta, data, probe_cmd, delay); // sync if necessary
      if(pwb) cache->hook_write(addr, ai, s, w, true, true, data, delay); // a write occurred during the probe
    }

    auto writeback = policy->writeback_need_writeback(meta, outer->is_uncached());
    if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty

    policy->meta_after_flush(cmd, meta);
    cache->hook_manage(addr, ai, s, w, hit, policy->is_evict(cmd), writeback.first, data, delay);

    if(meta->is_extend()) cache->data_return_buffer(data);
  }

};

typedef InnerCohPortT<ExclusiveInnerPortUncachedDirectory> ExclusiveInnerPortDirectory;


template<class OPUC> requires C_DERIVE(OPUC, OuterCohPortUncached)
class ExclusiveOuterCohPortBroadcastT : public OPUC
{
protected:
  using OPUC::cache;
  using OPUC::inner;
  using OPUC::policy;
public:
  ExclusiveOuterCohPortBroadcastT(CohPolicyBase *policy) : OPUC(policy) {}
  virtual ~ExclusiveOuterCohPortBroadcastT() {}

  virtual std::pair<bool, bool> probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, coh_cmd_t outer_cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    bool writeback = false;
    bool hit = cache->hit(addr, &ai, &s, &w);
    bool probe_hit = false;
    bool probe_writeback = false;
    CMMetadataBase* meta = nullptr;
    CMDataBase* data = nullptr;

    if(hit) {
      std::tie(meta, data) = cache->access_line(ai, s, w); // need c++17 for auto type infer
    } else {
      auto sync = policy->probe_need_sync(outer_cmd, meta);
      meta = cache->meta_copy_buffer(); meta->init(addr); meta->get_outer_meta()->to_invalid();
      data = cache->data_copy_buffer();
      std::tie(probe_hit, probe_writeback) = inner->probe_req(addr, meta, data, sync.second, delay);
      if(probe_writeback) cache->hook_write(addr, ai, s, w, true, true, data, delay);
    }

    if(hit || probe_writeback) {
      if(writeback = policy->probe_need_writeback(outer_cmd, meta))
        if(data_outer) data_outer->copy(data);
    }

    policy->meta_after_probe(outer_cmd, meta, meta_outer, OPUC::coh_id, writeback);
    cache->hook_manage(addr, ai, s, w, hit, policy->is_outer_evict(outer_cmd), writeback, data, delay);

    if(!hit) {
      cache->meta_return_buffer(meta);
      cache->data_return_buffer(data);
    }

    return std::make_pair(hit||probe_hit, writeback);
  }

};

typedef ExclusiveOuterCohPortBroadcastT<OuterCohPortUncached> ExclusiveOuterCohPortBroadcast;


template<class OPUC> requires C_DERIVE(OPUC, OuterCohPortUncached)
class ExclusiveOuterCohPortDirectoryT : public OPUC
{
protected:
  using OPUC::cache;
  using OPUC::inner;
  using OPUC::policy;
public:
  ExclusiveOuterCohPortDirectoryT(CohPolicyBase *policy) : OPUC(policy) {}
  virtual ~ExclusiveOuterCohPortDirectoryT() {}

  virtual std::pair<bool, bool> probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, coh_cmd_t outer_cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    bool writeback = false;
    bool hit = cache->hit(addr, &ai, &s, &w);
    bool probe_hit = false;
    bool probe_writeback = false;
    CMMetadataBase* meta = nullptr;
    CMDataBase* data = nullptr;

    // ToDo: flush may not hit (hit on inner cache when broadcast)
    if(hit) {
      std::tie(meta, data) = cache->access_line(ai, s, w); // need c++17 for auto type infer
      if(meta->is_extend()) {
        auto sync = policy->probe_need_sync(outer_cmd, meta);
        if(sync.first) {
          data = cache->data_copy_buffer();
          std::tie(probe_hit, probe_writeback) = inner->probe_req(addr, meta, data, sync.second, delay);
          if(probe_writeback) cache->hook_write(addr, ai, s, w, true, true, data, delay);
        }
      }

      if(writeback = policy->probe_need_writeback(outer_cmd, meta))
        if(data_outer) data_outer->copy(data);
    }

    policy->meta_after_probe(outer_cmd, meta, meta_outer, OPUC::coh_id, writeback);
    cache->hook_manage(addr, ai, s, w, hit, policy->is_outer_evict(outer_cmd), writeback, data, delay);

    if(hit && meta->is_extend()) {
      cache->data_return_buffer(data);
    }

    return std::make_pair(hit||probe_hit, writeback);
  }

};

typedef ExclusiveOuterCohPortDirectoryT<OuterCohPortUncached> ExclusiveOuterCohPortDirectory;

template<typename CT>
using ExclusiveL2CacheBroadcast = CoherentCacheNorm<CT, ExclusiveOuterCohPortBroadcast, ExclusiveInnerPortBroadcast>;

template<typename CT>
using ExclusiveLLCBroadcast = CoherentCacheNorm<CT, OuterCohPortUncached, ExclusiveInnerPortBroadcast>;

template<typename CT>
using ExclusiveL2CacheDirectory = CoherentCacheNorm<CT, ExclusiveOuterCohPortDirectory, ExclusiveInnerPortDirectory>;

template<typename CT>
using ExclusiveLLCDirectory = CoherentCacheNorm<CT, OuterCohPortUncached, ExclusiveInnerPortDirectory>;

#endif
