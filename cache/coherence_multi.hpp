#ifndef CM_CACHE_COHERENCE_MULTI_HPP
#define CM_CACHE_COHERENCE_MULTI_HPP
#include "cache/coherence.hpp"
#include "cache/cache_multi.hpp"
#include "cache/policy_multi.hpp"
#include "util/concept_macro.hpp"


/////////////////////////////////
// Priority of transactions:
// transactions with higher priority can pre-empt transactions with lower priority on the same cache set
struct XactPrio{
  static const uint16_t acquire       = 0x0001;
  static const uint16_t flush         = 0x0001;
  static const uint16_t read          = 0x0001;
  static const uint16_t write         = 0x0001;
  static const uint16_t probe         = 0x0010; // acquire miss, requiring lower cahce which back-probe this cache
  static const uint16_t evict         = 0x0100;
  static const uint16_t release       = 0x1000; // acquire hit but need back probe and writeback from inner
};

struct addr_info{
  uint32_t ai;
  uint32_t s;
  uint32_t w;
};

struct info{
  bool valid;
  addr_info loc;
};

/////////////////////////////////
// database for store inner acquire address
class InnerAddressDataMap
{
protected:
  std::vector<std::mutex *>  mtx; // mutex for protecting record
  std::vector<std::unordered_map<uint64_t, addr_info> > map;
public:

  InnerAddressDataMap() {}

  void add(int64_t id, uint64_t addr, addr_info loc){
    std::unique_lock lk(*mtx[id]);
    map[id][addr] = loc;
  }
  void erase(int64_t id, uint64_t addr){
    std::unique_lock lk(*mtx[id]);
    map[id].erase(addr);
  }
  virtual std::pair<bool, addr_info> query(int64_t id, uint64_t addr){
    addr_info info;
    bool count;
    std::unique_lock lk(*mtx[id]);
    if(map[id].count(addr)){
      count = true;
      info = map[id][addr];
    }else{
      count = false;
    }
    return std::make_pair(count, info);
  }
  virtual void resize(uint32_t size){
    map.resize(map.size()+1);
    std::mutex* m = new std::mutex();
    mtx.emplace_back(m);
  }

  virtual ~InnerAddressDataMap(){
    for(auto m : mtx) delete m;
  }
};


/////////////////////////////////
// Multi-thread support for inner ports
class InnerCohPortMultiThreadSupport
{
public:
  InnerCohPortMultiThreadSupport() {}
  virtual ~InnerCohPortMultiThreadSupport() {}

protected:
  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, bool>
          access_line_multithread(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) = 0;
};

// common behavior for cached outer ports
template <class OPUC, typename IT, typename CT> 
  requires C_DERIVE<IT, InnerCohPortMultiThreadSupport, InnerCohPortBase>
class OuterCohPortMultiThreadT : public OPUC
{
protected:
  using OuterCohPortBase::cache;
  using OuterCohPortBase::coh_id;
  using OuterCohPortBase::inner;
  using OuterCohPortBase::coh;
  using OPUC::writeback_req;

public:
  OuterCohPortMultiThreadT(policy_ptr policy) : OPUC(policy) {}
  virtual ~OuterCohPortMultiThreadT() {}

  virtual void finish_req(uint64_t addr){
    assert(!this->is_uncached());
    OuterCohPortBase::coh->finish_resp(addr, this->policy->cmd_for_finish(coh_id));
  }
  
  virtual std::pair<bool,bool> probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, coh_cmd_t outer_cmd, uint64_t *delay){
    uint32_t ai, s, w;
    auto cache = static_cast<CT *>(OuterCohPortBase::cache);
    bool writeback = false;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    bool hit = cache->hit(addr, &ai, &s, &w, XactPrio::probe);
    if(hit){
      std::tie(meta, data) = cache->access_line(ai, s, w);
      meta->lock();
      /** It is possible that higher priority behaviors have caused the meta to change, so need check again */
      if(!meta->is_valid() || meta->addr(s) != addr){
        meta->unlock();
        hit = false;
      }else{
        auto sync = OPUC::policy->probe_need_sync(outer_cmd, meta);
        if(sync.first){
          auto [phit, pwb] = inner->probe_req(addr, meta, data, sync.second, delay);
          if(pwb) cache->hook_write(addr, ai, s, w, true, true, meta, data, delay);
        }
        // writeback if dirty
        if((writeback = OPUC::policy->probe_need_writeback(outer_cmd, meta))) {
          if(data_outer) data_outer->copy(data);
        }
        OPUC::policy->meta_after_probe(outer_cmd, meta, meta_outer, coh_id, writeback); // alway update meta

        meta->unlock();
      }

      cache->reset_mt_state(ai, s, XactPrio::probe);
    }
    cache->hook_manage(addr, ai, s, w, hit, OPUC::policy->is_outer_evict(outer_cmd), writeback, meta, data, delay);
    return std::make_pair(hit, writeback);
  }

};

template <typename IT, typename CT> 
  requires C_DERIVE<IT, InnerCohPortBase, InnerCohPortMultiThreadSupport>
using OuterCohMultiThreadPort = OuterCohPortMultiThreadT<OuterCohPortUncached<true>, IT, CT>;

template <typename CT, typename CPT>
  requires C_DERIVE<CPT, CohPolicyBase, CohPolicyMultiThreadSupport>
class InnerCohPortMultiThreadUncached : public InnerCohPortUncached<true>, public InnerCohPortMultiThreadSupport
{
protected:
  InnerAddressDataMap* database;
  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, bool>
  access_line_multithread(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) { 
    uint32_t ai, s, w;
    auto cache = static_cast<CT *>(InnerCohPortUncached<true>::cache);
    /** true indicates that replace is desired */
    bool hit = cache->hit(addr, &ai, &s, &w, XactPrio::acquire, true);
    auto [meta, data] = cache->access_line(ai, s, w);
    meta->lock();
    if(hit){
      auto sync = policy->access_need_sync(cmd, meta);
      if(sync.first) {
        auto [phit, pwb] = probe_req(addr, meta, data, sync.second, delay); // sync if necessary
        if(pwb) cache->hook_write(addr, ai, s, w, true, true, meta, data, delay); // a write occurred during the probe
      }
      auto [promote, promote_local, promote_cmd] = policy->access_need_promote(cmd, meta);
      if(promote) { outer->acquire_req(addr, meta, data, promote_cmd, delay); hit = false; } // promote permission if needed
      else if(promote_local) {
        meta->to_modified(-1);
      }
    } else{
      if(meta->is_valid()) evict(meta, data, ai, s, w, delay);
      outer->acquire_req(addr, meta, data, policy->cmd_for_outer_acquire(cmd), delay); // fetch the missing block
    }
    return std::make_tuple(meta, data, ai, s, w, hit);
  }
  virtual void evict(CMMetadataBase *meta, CMDataBase *data, uint32_t ai, uint32_t s, uint32_t w, uint64_t *delay) {
    auto cache = static_cast<CT *>(InnerCohPortUncached<true>::cache);
    auto addr = meta->addr(s);
    auto sync = policy->writeback_need_sync(meta);
    if(sync.first) {
      auto [phit, pwb] = probe_req(addr, meta, data, sync.second, delay); // sync if necessary
      if(pwb) cache->hook_write(addr, ai, s, w, true, true, meta, data, delay); // a write occurred during the probe
    }
    auto writeback = policy->writeback_need_writeback(meta, outer->is_uncached());
    if(writeback.first){
      cache->set_mt_state(ai, s, XactPrio::evict);

      auto writeback_r = policy->writeback_need_writeback(meta, outer->is_uncached());
      if(writeback_r.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty

      cache->reset_mt_state(ai, s, XactPrio::evict);
    }
    policy->meta_after_evict(meta);
    cache->hook_manage(addr, ai, s, w, true, true, writeback.first, meta, data, delay);
  }

  virtual void write_line(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay){
    uint32_t ai, s, w;
    auto cache = static_cast<CT *>(InnerCohPortUncached<true>::cache);
    bool hit = cache->hit(addr, &ai, &s, &w, XactPrio::release);
    if(hit){
      auto [meta, data] = cache->access_line(ai, s, w);
      if(data_inner) data->copy(data_inner);
      policy->meta_after_release(cmd, meta, meta_inner);
      assert(meta_inner); // assume meta_inner is valid for all writebacks
      cache->hook_write(addr, ai, s, w, hit, true, meta, data, delay);

      cache->reset_mt_state(ai, s, XactPrio::release);
    }
  }

  virtual void flush_line(uint64_t addr, coh_cmd_t cmd, uint64_t *delay){
    uint32_t ai, s, w;
    CMMetadataBase *meta = nullptr;
    CMDataBase *data = nullptr;
    std::mutex* mtx;
    std::mutex* wmtx;
    std::condition_variable* cv;
    std::vector<uint32_t>* status;
    auto cache = static_cast<CT *>(InnerCohPortUncached<true>::cache);
    bool hit = cache->hit(addr, &ai, &s, &w, XactPrio::flush);
    if(hit) std::tie(meta, data) = cache->access_line(ai, s, w);

    auto [flush, probe, probe_cmd] = policy->flush_need_sync(cmd, meta, outer->is_uncached());
    if(!flush) {
      if(hit){
        cache->reset_mt_state(ai, s, XactPrio::flush);
      }
      // do not handle flush at this level, and send it to the outer cache
      outer->writeback_req(addr, nullptr, nullptr, policy->cmd_for_flush(), delay);
      return;
    }

    if(!hit) return;

    if(probe) {
      auto [phit, pwb] = probe_req(addr, meta, data, probe_cmd, delay); // sync if necessary
      if(pwb) cache->hook_write(addr, ai, s, w, true, true, meta, data, delay); // a write occurred during the probe
    }

    auto writeback = policy->writeback_need_writeback(meta, outer->is_uncached());
    if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty

    policy->meta_after_flush(cmd, meta);
    cache->hook_manage(addr, ai, s, w, hit, policy->is_evict(cmd), writeback.first, meta, data, delay);

    cache->reset_mt_state(ai, s, XactPrio::flush);
  }

public:
  InnerCohPortMultiThreadUncached(policy_ptr policy) : InnerCohPortUncached(policy), InnerCohPortMultiThreadSupport() {
    database = new InnerAddressDataMap();
  }

  virtual ~InnerCohPortMultiThreadUncached() { delete database; } 

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay){
    auto p_policy = std::static_pointer_cast<CPT>(policy);
    auto [meta, data, ai, s, w, hit] = access_line_multithread(addr, cmd, delay);
    if(meta->is_valid() && meta->addr(s) == addr){
      policy->meta_after_grant(cmd, meta, meta_inner);
      if(data_inner) data_inner->copy(this->cache->get_data(ai, s, w));
      cache->hook_read(addr, ai, s, w, hit, meta, data, delay);
    }

    /** After the upper-level cache modifies the meta, an acquire_ack request is sent to the lower-level cache */
    outer->finish_req(addr);

    /**
     * If the upper-level cache is uncached, the lower-level cache can modify the state  
     * of set(unlock) without waiting for the upper-level cache to issue an ack request
     * (in fact, uncached cache does not issue an ack request to lower-level cache)
     */
    bool unlock = p_policy->acquire_need_unlock(cmd);
    if(unlock){
      meta->unlock();
      cache->reset_mt_state(ai, s, XactPrio::acquire);
    }else{
      /** store relevant locks in the database and wait for the upper-level cache to issue an ack request */
      database->add(cmd.id, addr, addr_info{ai, s, w});
    }
  }
  
  virtual void finish_resp(uint64_t addr, coh_cmd_t cmd){
    /** query whether the information of this address exists in the database */
    auto info = database->query(cmd.id, addr);
    if(info.first){
      auto [ai, s, w] = info.second;
      cache->access(ai,s,w)->unlock();
      cache->reset_mt_state(ai, s, XactPrio::acquire);
      database->erase(cmd.id, addr);
    }
  }

  virtual std::pair<uint32_t, policy_ptr> connect(CohClientBase *c, bool uncached = false) {
    auto conn = InnerCohPortUncached<true>::connect(c, uncached);
    database->resize(coh.size());
    return conn;
  }

};

template<class IPUC>
class InnerCohPortMultiThreadT : public IPUC
{
protected:
  using IPUC::coh;
public:
  InnerCohPortMultiThreadT(policy_ptr policy) : IPUC(policy) {}
  virtual ~InnerCohPortMultiThreadT() {}

  virtual std::pair<bool, bool> probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) {
    bool hit = false, writeback = false;
    for(uint32_t i=0; i<coh.size(); i++) {
      auto probe = IPUC::policy->probe_need_probe(cmd, meta, i);
      if(probe.first) {
        auto [phit, pwb] = coh[i]->probe_resp(addr, meta, data, probe.second, delay);
        hit       |= phit;
        writeback |= pwb;
      }
    }
    return std::make_pair(hit, writeback);
  }
};

template <typename CT, typename CPT>
  requires C_DERIVE<CPT, CohPolicyBase, CohPolicyMultiThreadSupport>
using InnerCohMultiThreadPort = InnerCohPortMultiThreadT<InnerCohPortMultiThreadUncached<CT, CPT> >;

template <typename CT, typename CPT>
  requires C_DERIVE<CPT, CohPolicyBase, CohPolicyMultiThreadSupport>
class CoreMultiThreadInterface : public InnerCohPortMultiThreadUncached<CT, CPT>, public CoreInterfaceBase
{
  typedef InnerCohPortMultiThreadUncached<CT, CPT> InnerT;

protected:
  using InnerT::cache;
  using InnerT::outer;
  using InnerT::access_line_multithread;
  using InnerT::policy;
  using InnerT::flush_line;
public:
  CoreMultiThreadInterface(policy_ptr policy) : InnerT(policy) {}
  virtual ~CoreMultiThreadInterface() {}

  virtual const CMDataBase *read(uint64_t addr, uint64_t *delay) {
    auto policy = std::static_pointer_cast<CPT>(InnerT::policy);
    addr = normalize(addr);
    auto cmd = policy->cmd_for_read();
    auto [meta, data, ai, s, w, hit] = access_line_multithread(addr, cmd, delay);

    cache->hook_read(addr, ai, s, w, hit, meta, data, delay);

    /** Uncached cache will not issue an ack request */
    auto ack = policy->acquire_need_ack(outer->is_uncached());
    outer->finish_req(addr);

    meta->unlock();
    cache->reset_mt_state(ai, s, XactPrio::read);

    return data;
  }

  virtual void write(uint64_t addr, const CMDataBase *m_data, uint64_t *delay) {
    auto policy = std::static_pointer_cast<CPT>(InnerT::policy);
    addr = normalize(addr);
    auto cmd = policy->cmd_for_write();
    auto [meta, data, ai, s, w, hit] = access_line_multithread(addr, cmd, delay);

    meta->to_dirty();
    if(data) data->copy(m_data);
    cache->hook_write(addr, ai, s, w, hit, false, meta, data, delay);

    auto ack = policy->acquire_need_ack(outer->is_uncached());
    outer->finish_req(addr);

    meta->unlock();
    cache->reset_mt_state(ai, s, XactPrio::read);
  }

  // flush a cache block from the whole cache hierarchy, (clflush in x86-64)
  virtual void flush(uint64_t addr, uint64_t *delay)     { addr = normalize(addr); flush_line(addr, policy->cmd_for_flush(), delay); }

  // if the block is dirty, write it back to memory, while leave the block cache in shared state (clwb in x86-64)
  virtual void writeback(uint64_t addr, uint64_t *delay) { addr = normalize(addr); flush_line(addr, policy->cmd_for_writeback(), delay); }

  // writeback and invalidate all dirty cache blocks, sync with NVM (wbinvd in x86-64)
  virtual void writeback_invalidate(uint64_t *delay) {
    assert(nullptr == "Error: L1.writeback_invalidate() is not implemented yet!");
  }
  virtual void flush_cache(uint64_t *delay) {
    auto [npar, nset, nway] = cache->size();
    for(int ipar=0; ipar<npar; ipar++)
      for(int iset=0; iset < nset; iset++)
        for(int iway=0; iway < nway; iway++) {
          auto [meta, data] = cache->access_line(ipar, iset, iway);
          if(meta->is_valid())
            flush_line(meta->addr(iset), policy->cmd_for_flush(), delay);
        }
  }

  virtual void query_loc(uint64_t addr, std::list<LocInfo> *locs){
    addr = normalize(addr);
    outer->query_loc_req(addr, locs);
    locs->push_front(cache->query_loc(addr));
  }

private:
  // hide and prohibit calling these functions
  virtual uint32_t connect(CohClientBase *c) { return 0;}
  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay, int32_t inner_inner_id = 0) {}
  virtual void writeback_resp(uint64_t addr, CMDataBase *data, coh_cmd_t cmd, uint64_t *delay) {}
};


#endif
