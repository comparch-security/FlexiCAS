#ifndef CM_RSA_CACHE_HPP_
#define CM_RSA_CACHE_HPP_

#include "cache/dynamic_random.hpp"
#include "rsa/history.hpp"
#include "rsa/monitor.hpp"

// support an indexer bit in the metadata for sequential associative cache
template<typename MT> requires C_DERIVE<MT, CMMetadataBase>
class MetadataWithIndexer : public MT
{
protected:
  uint8_t indexer = 0;
public:
  __always_inline void set_indexer(uint8_t index)   { indexer = index;  }
  __always_inline uint8_t get_indexer()             { return indexer;   }
};

template<typename MT>
using SeqAssMetadata = MetadataWithIndexer<MT>;

template<int IW, int NW, bool EF, bool DUO, bool EnMT>
class SeqAssReplaceLRU : public ReplaceLRU<IW, NW, EF, DUO, EnMT>
{
  typedef ReplaceFuncBase<EF, NW, EnMT> RPT;
  using RPT::used_map;
public:
  virtual void invalid(uint32_t s, uint32_t w, bool flush = false) override {
    RPT::invalid(s, w, flush);
    if(flush && used_map[s][w] == 0) { // reinsert the way to avoid prefetch-flush attack
      this->set_alloc_map(s, w);
      this->access(s, w, true, false);
    }
  }
};

template<int IW, int NW, typename MT, typename DT, typename IDX, typename RPC, typename DLY,
         bool EnMon, int PTSize, int MSHR = 4>
class SeqAssCache : public CacheSkewed<IW, NW, 1, MT, DT, IDX, RPC, DLY, EnMon, false, MSHR>
{
  typedef CacheSkewed<IW, NW, 1, MT, DT, IDX, RPC, DLY, EnMon, false, MSHR> CacheT;
  static const bool EnLoadBalance = true;
protected:
  using CacheBase::arrays;
  using CacheT::replacer;

  static constexpr uint32_t nset = 1ul << IW;
  IDX indexers[3]; // sequential associative (SA) cache needs two indexers, and one for remap
  IDX *sa_indexer[2];
  std::vector<uint32_t> s_evict;  // recording the number of evictions in each set
  std::vector<bool> s_attack;
  static const uint32_t s_evict_max = 7;
  static const uint32_t HighTH = 1;
  uint32_t decay = 0; // set index for decay
  uint32_t c_high = 0; // record the number of cache set with high conflict rate
  std::vector<uint8_t> ptable;
  CMHasher phash;
  CacheRemap<IW/2, 1, 1, MT, DT, IndexSkewed<IW/2, 6, 1>, ReplaceFIFO<IW/2, 1, false, false, false>, void, false> vbuf;

  IDX *remap_indexer; // the indexer for remap
  uint64_t index_addr_cnt[3] = {0, 0, 0};
  uint8_t remap = 0;
  uint8_t remap_p = 0;

  __always_inline int index_predict(uint64_t addr) {
    return ptable[phash(addr) % PTSize];
  }

  __always_inline void index_train(uint64_t addr, int sel) {
    ptable[phash(addr) % PTSize] = sel;
  }

  __always_inline void check_high_cnt() {
    uint32_t cnt = 0;
    for(uint32_t i=0; i<nset; i++)
      if(high_conflict_level(0, i))
        cnt++;
    assert(c_high == cnt);
  }

public:
  SeqAssCache(std::string name = "SeqAssCache")
    : CacheT(name), sa_indexer{&indexers[0], &indexers[1]}, s_evict(nset, 0), s_attack(nset, false), ptable(PTSize, 0),
      vbuf("vbuf"), remap_indexer(&indexers[2]) {}

  void vbuf_write(uint64_t addr, const CMMetadataBase * meta, const CMDataBase *data) {
    uint32_t ai, s, w;
    vbuf.replace(addr, &ai, &s, &w, XactPrio::acquire);
    auto [v_meta, v_data] = vbuf.access_line(ai, s, w);
    v_meta->copy(meta); v_meta->init(addr); v_meta->to_modified(-1);
    if constexpr (!C_VOID<DT>) v_data->copy(data);
    vbuf.replace_read(ai, s, w, false);
    vbuf.hook_read(addr, ai, s, w, false, v_meta, v_data, nullptr);
  }

  bool vbuf_read(uint64_t addr, CMMetadataBase * meta, CMDataBase *data) {
    uint32_t ai, s, w;
    bool hit = vbuf.hit(addr, &ai, &s, &w, XactPrio::acquire, 0);
    if(hit) {
      auto [v_meta, v_data] = vbuf.access_line(ai, s, w);
      meta->copy(v_meta); meta->init(addr);
      if constexpr (!C_VOID<DT>) data->copy(v_data);
    }
    return hit;
  }

  void vbuf_rekey() { vbuf.rotate_indexer(); }

  __always_inline bool high_conflict_level(uint32_t ai, uint32_t s) const {
    return s_evict[s] > HighTH;
  }

  __always_inline void update_conflict_level(uint64_t addr, uint32_t s, uint8_t type) {
    bool prev_high = high_conflict_level(0, s);
    if(type == 1 && s_evict[s] < HighTH + 2) { // prefetch
      s_evict[s] = HighTH + 2;
    }
    if(s_evict[s] < s_evict_max) s_evict[s]++;
    if(type == 2) { // vbuf hit
      s_evict[s] = s_evict_max;
      s_attack[s] = true;
      if(remap != 2) this->monitor_magic_func(addr, EvictDistAttackEvent, &s);
    }
    if(type == 3) { //flush
      s_evict[s] = s_evict_max;
      s_attack[s] = true;
      if(remap != 2) this->monitor_magic_func(addr, EvictDistAttackEvent, &s);
    }
    if(!prev_high && high_conflict_level(0, s)) {
      c_high++;
      this->monitor_magic_func(addr, EvictDistConflictEvent, &c_high);
    }
  }

  __always_inline void seqass_conflict_decay(int32_t s) {
    if(s<0) {
      decay = (decay + 1) % nset;
      bool prev_high = high_conflict_level(0, decay);
      s_evict[decay] = s_evict[decay] > 1 ? s_evict[decay] - 1 : 0;
      if(!high_conflict_level(0, decay)) {
        if(prev_high) c_high--;
        s_attack[decay] = false;
      }
    } else if (!s_attack[s]) {
      bool prev_high = high_conflict_level(0, s);
      s_evict[s] = s_evict[s] > 1 ? s_evict[s] - 1 : 0;
      if(!high_conflict_level(0, s) && prev_high) c_high--;
    }
  }

  virtual bool hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, uint16_t prio, bool check) override {
    *ai = 0; // P=1

    auto sel = index_predict(addr);

    IDX *p_index[3]; int p_sel[3];
    if(remap && sel == remap_p) {
      p_index[0] = index_addr_cnt[sel] > index_addr_cnt[2] ? sa_indexer[sel] : remap_indexer;   p_sel[0] = sel;
      p_index[1] = index_addr_cnt[sel] > index_addr_cnt[2] ? remap_indexer   : sa_indexer[sel]; p_sel[1] = sel;
      p_index[2] = sa_indexer[1-sel]; p_sel[2] = 1-sel;
    } else {
      p_index[0] = sa_indexer[sel];   p_sel[0] = sel;
      p_index[1] = sa_indexer[1-sel]; p_sel[1] = 1-sel;
      p_index[2] = remap_indexer;     p_sel[2] = remap_p;
    }

    for(int i=0; i<2 || (i==2 && remap); i++) {
      *s = p_index[i]->index(addr, 0);
      if(arrays[0]->hit(addr, *s, w)) {
        if(prio && check && i==0) this->monitor_magic_func(addr, EvictDistPredHitEvent, nullptr);
        if(prio) index_train(addr, p_sel[i]); // train the index predictor
        return true;
      }
    }

    if(prio == 0) // general query, need to check vbuf
      return vbuf.hit(addr, ai, s, w, 0, false);

    *ai = 1; // may be needed in the future for exclusive SA cache
    return false;
  }

  bool seqass_replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, uint8_t *ii, uint16_t prio) {
    static uint8_t dummy = 0;

    dummy++;
    *ai = 0; // P=1

    // collect information  // ToDo: whether to add attack
    uint32_t si[2]     = {sa_indexer[0]->index(addr, 0), sa_indexer[1]->index(addr, 0)}; // set index
    uint32_t ev_cnt[2] = {s_evict[si[0]], s_evict[si[1]]}; // number of evictions

    *ii = dummy%2;
    if constexpr (EnLoadBalance) {
      if(ev_cnt[0] > ev_cnt[1]) *ii = 1; // insert to the less conflicted set
      if(ev_cnt[0] < ev_cnt[1]) *ii = 0;
    }
    *s = si[*ii];

    index_train(addr, *ii);
    replacer[0].replace(*s, w);
    return true;
  }

  __always_inline bool seqass_search(uint64_t addr, uint32_t *s, uint32_t *w, uint8_t ii, uint32_t s_s, std::set<uint32_t>& vset) {
    *s = sa_indexer[ii]->index(addr, 0); // get the new cache set
    //if(vset.count(*s)) return false; // self-relocation, or loop
    if(s_s == *s ) return false; // if self-relocation
    replacer[0].replace(*s, w); return true;
  }

  __always_inline void seqass_relocate_invalidate(uint64_t addr, uint32_t s, uint32_t w) {
    replacer[0].invalid(s, w);
  }

  __always_inline void seqass_relocate_update(uint64_t addr, uint32_t s, uint32_t w) {
    replacer[0].access(s, w, true, true);
    this->monitor_magic_func(addr, EvictDistRelocEvent, &s);
    seqass_conflict_decay(s);
  }

  virtual void replace_read(uint32_t ai, uint32_t s, uint32_t w, bool prefetch, bool genre = false) override {
    replacer[ai].access(s, w, true, prefetch && !s_attack[s]); // stop repeated ct-prefetch on the same target
  }

  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint32_t evict, bool writeback, const CMMetadataBase * meta, const CMDataBase *data, uint64_t *delay) override {
    if(ai < 1 && hit && evict) seqass_conflict_decay(-1);                // 08-30: decay when evict
    if(ai < 1 && hit && evict==2) update_conflict_level(addr, s, 3);     // set the cache set with high conflict when flush
    CacheT::hook_manage(addr, ai, s, w, hit, evict, writeback, meta, data, delay);
  }

  void remap_start() {
    remap = 1;
    // replace indexer
    auto indexer = sa_indexer[remap_p];
    sa_indexer[remap_p] = remap_indexer;
    remap_indexer = indexer;

    auto cnt = index_addr_cnt[remap_p];
    index_addr_cnt[remap_p] = index_addr_cnt[2];
    index_addr_cnt[2] = cnt;
  }

  void remap_forced() {
    remap = 2;
  }

  void remap_end() {
    remap = 0;
    remap_indexer->reseed();
    index_addr_cnt[2] = 0;
    remap_p = (remap_p + 1) % 2;
  }

  void remap_add_addr(uint64_t addr, uint32_t s) {
    if(sa_indexer[0]->index(addr, 0) == s) index_addr_cnt[0]++;
    if(sa_indexer[1]->index(addr, 0) == s) index_addr_cnt[1]++;
    if(remap_indexer->index(addr, 0) == s) index_addr_cnt[2]++;
  }

  void remap_remove_addr(uint64_t addr, uint32_t s) {
    if(sa_indexer[0]->index(addr, 0) == s && index_addr_cnt[0] > 0) index_addr_cnt[0]--;
    if(sa_indexer[1]->index(addr, 0) == s && index_addr_cnt[1] > 0) index_addr_cnt[1]--;
    if(remap_indexer->index(addr, 0) == s && index_addr_cnt[2] > 0) index_addr_cnt[2]--;
  }

  uint64_t remap_addr_cnt() const {
    return index_addr_cnt[2];
  }

  void print_remap_addr_cnt() const {
    std::cout << "replace the " << (uint32_t)(remap_p) << "th index: " << index_addr_cnt[0] << ", " << index_addr_cnt[1] << ", " << index_addr_cnt[2];
  }

  std::tuple<bool, uint64_t, CMMetadataBase *, CMDataBase *, CMMetadataBase *, CMDataBase *, uint32_t, uint32_t>
  remap_require(uint32_t ai, uint32_t s, uint32_t w) {
    auto [meta, data] = this->access_line(ai, s, w);
    if(meta->is_valid()) {
      auto addr = meta->addr(s);
      auto ii = static_cast<MT *>(meta)->get_indexer();
      auto new_s = sa_indexer[remap_p]->index(addr, 0);
      uint32_t new_w;
      if(ii == remap_p && remap_indexer->index(addr, 0) == s && new_s != s) {
        replacer[0].replace(new_s, &new_w);
        auto [t_meta, t_data] = this->access_line(ai, new_s, new_w);
        return std::make_tuple(true, addr, meta, data, t_meta, t_data, new_s, new_w);
      }
    }
    return std::make_tuple(false, 0, meta, data, nullptr, nullptr, 0, 0);
  }

  virtual bool query_coloc(uint64_t addrA, uint64_t addrB) override {
    for(int i=0; i<2; i++)
      for(int j=0; j<2; j++)
        if(sa_indexer[i]->index(addrA, 0) == sa_indexer[j]->index(addrB, 0))
          return true;
    if(remap && remap_indexer->index(addrA, 0) == remap_indexer->index(addrB, 0))
      return true;
    return false;
  }

  virtual void query_fill_loc(LocInfo *loc, uint64_t addr) override {
    for(int i=0; i<2; i++){
      loc->insert(LocIdx(i, sa_indexer[i]->index(addr, 0)), LocRange(0, NW-1));
    }
    if(remap)
      loc->insert(LocIdx(2, remap_indexer->index(addr, 0)), LocRange(0, NW-1));
  }
};

template<typename Policy, bool EnMT, typename MT, typename CT>
class SeqAssInnerCohPortUncached : public InnerCohPortUncached<Policy, EnMT>
{
  typedef InnerCohPortUncached<Policy, EnMT> PortT;
  typedef InnerCohPortBase BaseT;
  static const bool EnRelocate = true;
  static const bool EnVBuf = true;
  static const bool AlwaysReloc = false;
protected:
  using BaseT::outer;

  std::tuple<bool, uint64_t, CMMetadataBase *, CMDataBase *, int32_t, uint32_t, uint32_t> relocate_record;
  std::set<uint32_t> visited_set;

  __always_inline std::tuple<bool, uint64_t, CMMetadataBase *, CMDataBase *, int32_t, uint32_t, uint32_t>
  seqass_evict(CMMetadataBase *meta, CMDataBase *data, int32_t ai, uint32_t s, uint32_t w, std::set<uint32_t>& vset, uint64_t *delay) {
    CT *cache = static_cast<CT *>(BaseT::cache);

    if(!meta->is_valid()) return std::make_tuple(false, 0, nullptr, nullptr, 0, 0, 0);

    int32_t d_ai = 0; uint32_t d_s, d_w;
    auto ii = 1ul - static_cast<MT *>(meta)->get_indexer();
    auto addr = meta->addr(s);

    cache->remap_remove_addr(addr, s); // remove the cache block from the remap counting

    if constexpr (!EnRelocate) {
      vbuf_evict(meta, data, ai, s, w, delay);
      return std::make_tuple(false, 0, nullptr, nullptr, 0, 0, 0);
    }

    if((AlwaysReloc && !vset.empty()) || (!AlwaysReloc && !cache->high_conflict_level(ai, s))) {
      vbuf_evict(meta, data, ai, s, w, delay);
      return std::make_tuple(false, 0, nullptr, nullptr, 0, 0, 0);
    }

    vset.insert(s);
    if(!cache->seqass_search(addr, &d_s, &d_w, ii, s, vset)) { // self-relocation or loop
      vbuf_evict(meta, data, ai, s, w, delay);
      return std::make_tuple(false, 0, nullptr, nullptr, 0, 0, 0);
    }

    // do the first relocation, move to a buffer
    auto buf_meta = cache->meta_copy_buffer();
    auto buf_data = cache->data_copy_buffer();
    cache->relocate(addr, meta, buf_meta, data, buf_data);
    cache->seqass_relocate_invalidate(addr, s, w);
    static_cast<MT *>(buf_meta)->set_indexer(ii);
    return std::make_tuple(true, addr, buf_meta, buf_data, d_ai, d_s, d_w);
  }

  void seqass_relocate(uint64_t s_addr, CMMetadataBase *s_meta, CMDataBase *s_data, int32_t d_ai, uint32_t d_s, uint32_t d_w, uint64_t *delay) {
    CT *cache = static_cast<CT *>(BaseT::cache);
    auto [d_meta, d_data] = cache->access_line(d_ai, d_s, d_w);
    // do the relocation
    cache->relocate(s_addr, s_meta, d_meta, s_data, d_data);
    static_cast<MT *>(d_meta)->set_indexer(static_cast<MT *>(s_meta)->get_indexer());
    cache->seqass_relocate_update(s_addr, d_s, d_w);

    // release the original buffer
    cache->meta_return_buffer(s_meta);
    cache->data_return_buffer(s_data);
  }

  virtual void evict(CMMetadataBase *meta, CMDataBase *data, int32_t ai, uint32_t s, uint32_t w, uint64_t *delay) override {
    static_cast<CT *>(BaseT::cache)->remap_remove_addr(meta->addr(s), s); // remove the cache block from the remap counting
    PortT::evict(meta, data, ai, s, w, delay);
  }

  virtual void vbuf_evict(CMMetadataBase *meta, CMDataBase *data, int32_t ai, uint32_t s, uint32_t w, uint64_t *delay) {
    // evict a block due to conflict
    CT *cache = static_cast<CT *>(BaseT::cache);
    auto addr = meta->addr(s);
    //assert(cache->hit(addr));
    auto sync = Policy::writeback_need_sync(meta);
    if(sync.first) {
      if constexpr (EnMT && Policy::sync_need_lock()) cache->set_mt_state(ai, s, XactPrio::sync);
      auto [phit, pwb] = this->probe_req(addr, meta, data, sync.second, delay); // sync if necessary
      if(pwb){
        cache->replace_write(ai, s, w, false);
        cache->hook_write(addr, ai, s, w, true, meta, data, delay); // a write occurred during the probe
      }
      if constexpr (EnMT && Policy::sync_need_lock()) cache->reset_mt_state(ai, s, XactPrio::sync);
    }

    if constexpr (EnVBuf)
      cache->vbuf_write(addr, meta, data); // store the block to vbuf
    auto writeback = Policy::writeback_need_writeback(meta);
    if(writeback.first) outer->writeback_req(addr, meta, data, writeback.second, delay); // writeback if dirty
    Policy::meta_after_evict(meta);
    cache->replace_manage(ai, s, w, true, 1);
    cache->hook_manage(addr, ai, s, w, true, 1, writeback.first, meta, data, delay);
  }

  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, bool>
  access_line(uint64_t addr, coh_cmd_t cmd, uint16_t prio, uint64_t *delay) override { // common function for access a line in the cache
    CT *cache = static_cast<CT *>(BaseT::cache);
    uint32_t ai, s, w; uint8_t ii = 2;

    bool hit = cache->hit(addr, &ai, &s, &w, prio, prio==XactPrio::acquire && !coh::is_prefetch(cmd));
    if(!hit) cache->seqass_replace(addr, &ai, &s, &w, &ii, prio);
    auto [meta, data] = cache->access_line(ai, s, w);
    relocate_record = std::make_tuple(false, 0, nullptr, nullptr, 0, 0, 0);

    if(hit) {
      auto sync = Policy::access_need_sync(cmd, meta);
      if(sync.first) {
        auto [phit, pwb] = this->probe_req(addr, meta, data, sync.second, delay); // sync if necessary
        if(pwb) {
          cache->replace_write(ai, s, w, false);
          cache->hook_write(addr, ai, s, w, true, meta, data, delay);
        }
      }
      auto [promote, promote_local, promote_cmd] = Policy::access_need_promote(cmd, meta);
      if(promote_local) meta->to_modified(-1);
    } else { // miss
      auto has_conflict = meta->is_valid();
      uint8_t update_type = coh::is_prefetch(cmd) ? 1 : 0;
      visited_set.clear();
      relocate_record = seqass_evict(meta, data, ai, s, w, visited_set, delay);
      if(cache->vbuf_read(addr, meta, data)) {
        Policy::meta_after_fetch(Policy::cmd_for_outer_acquire(cmd), meta, addr); // fetch from vbuf
        update_type = 2;
        has_conflict = true;
      } else
        outer->acquire_req(addr, meta, data, Policy::cmd_for_outer_acquire(cmd), delay); // fetch the missing block
      cache->remap_add_addr(addr, s); // add the newly fetched cache block to remap counting
      static_cast<MT *>(meta)->set_indexer(ii);
      if(has_conflict || coh::is_prefetch(cmd)) cache->update_conflict_level(addr, s, update_type);
    }
    return std::make_tuple(meta, data, ai, s, w, hit);
  }

  virtual void extra_operations(uint64_t *delay) override {
    CT *cache = static_cast<CT *>(BaseT::cache);
    auto [relocate, buf_addr, buf_meta, buf_data, d_ai, d_s, d_w] = relocate_record;
    while(relocate) {
      auto [t_meta, t_data] = cache->access_line(d_ai, d_s, d_w);
      auto [nxt_relocate, nxt_addr, nxt_meta, nxt_data, nxt_ai, nxt_s, nxt_w] = seqass_evict(t_meta, t_data, d_ai, d_s, d_w, visited_set, delay);
      seqass_relocate(buf_addr, buf_meta, buf_data, d_ai, d_s, d_w, delay);
      cache->remap_add_addr(buf_addr, d_s); // readd the relocated cache block
      relocate = nxt_relocate;
      buf_addr = nxt_addr;
      buf_meta = nxt_meta;
      buf_data = nxt_data;
      d_ai = nxt_ai;
      d_s = nxt_s;
      d_w = nxt_w;
    }    
  }

  virtual void finish_resp(uint64_t addr, coh_cmd_t outer_cmd) override {
    CT *cache = static_cast<CT *>(BaseT::cache);
    bool remap_flag = false;
    cache->monitors->magic_func(0, MAGIC_ID_REMAP_ASK, &remap_flag);
    if(remap_flag) remap();
  }

  int64_t remap_wait_cnt = 0;
  
  void remap(){
    CT *cache = static_cast<CT *>(BaseT::cache);
    if(!remap_wait_cnt) {
      cache->remap_start();
      std::cout << "Remap: ";
      cache->print_remap_addr_cnt();
      std::cout << std::endl;
    }
    auto [np, ns, nw] = cache->size();
    remap_wait_cnt++;

    if(cache->remap_addr_cnt() > 0.005 * np * ns * nw && remap_wait_cnt < 16 * np * ns * nw) return;  // wait until most addresses are relocated

    cache->remap_forced();
    std::cout << "Remap: ";
    cache->print_remap_addr_cnt();
    std::cout << std::endl;
    std::cout << "remap waited for " << remap_wait_cnt << " accesses." << std::endl;

    for(int32_t ai = 0; ai < np; ai++)
      for(int32_t s = 0; s < ns; s++)
        for(int32_t w = 0; w < nw; w++) {
          auto [relocate, addr, meta, data, t_meta, t_data, new_s, new_w] = cache->remap_require(ai, s, w);
          if(relocate) {
            if(t_meta->is_valid()) {
              cache->remap_remove_addr(t_meta->addr(new_s), new_s);
              vbuf_evict(t_meta, t_data, ai, new_s, new_w, nullptr);
            }
            cache->remap_remove_addr(addr, s);
            cache->relocate(addr, meta, t_meta, data, t_data);
            static_cast<MT *>(t_meta)->set_indexer(static_cast<MT *>(meta)->get_indexer());
            cache->seqass_relocate_invalidate(addr, s, w);
            cache->seqass_relocate_update(addr, new_s, new_w);
            cache->remap_add_addr(addr, new_s);
          }
        }

    cache->vbuf_rekey();
    cache->remap_end();
    cache->monitors->magic_func(0, MAGIC_ID_REMAP_END, nullptr);
    remap_wait_cnt = 0;
    std::cout << "A remap is finished!" << std::endl;
  }
};

#endif
