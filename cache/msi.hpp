#ifndef CM_CACHE_MSI_HPP
#define CM_CACHE_MSI_HPP

#include "cache/exclusive.hpp"

// metadata supporting MSI coherency
class MetadataMSIBase : public CMMetadataBase
{
protected:
  unsigned int state     : 2; // 0: invalid, 1: shared, 2:modify
  unsigned int dirty     : 1; // 0: clean, 1: dirty
  unsigned int extend    : 1; // 0: cache meta, 1: extend directory meta
public:
  MetadataMSIBase() : state(0), dirty(0), extend(0) {}
  virtual ~MetadataMSIBase() {}

  virtual void to_invalid() { state = 0; }
  virtual void to_shared(int32_t coh_id) { state = 1; }
  virtual void to_modified(int32_t coh_id) { state = 2; }
  virtual void to_dirty() { dirty = 1; }
  virtual void to_clean() { dirty = 0; }
  virtual void to_extend() {}
  virtual bool is_valid() const { return state; }
  virtual bool is_shared() const { return state == 1; }
  virtual bool is_modified() const {return state == 2; }
  virtual bool is_dirty() const { return dirty; }
  virtual bool is_extend() const { return extend; }

  virtual void copy(const CMMetadataBase *m_meta) {
    auto meta = static_cast<const MetadataMSIBase *>(m_meta);
    state  = meta->state;
    dirty  = meta->dirty;
    extend = meta->extend;
  }

private:
  virtual void to_owned(int32_t coh_id) {}
  virtual void to_exclusive(int32_t coh_id) {}
};

template <int AW, int IW, int TOfst, typename ST>
using MetadataMSI = MetadataMixer<AW, IW, TOfst, MetadataMSIBase, ST>;


// Directory Metadata with match function
// AW    : address width
// IW    : index width
// TOfst : tag offset
// ST    : Coherence support
template <int AW, int IW, int TOfst, typename ST>
class MetadataMSIDirectory : public MetadataMSI<AW, IW, TOfst, ST>
{
public:

  virtual void to_extend(){
    this->extend = 1;
  }

  virtual void to_invalid() {
    this->state = 0;
    this->extend = 0;
    this->clean_sharer(); 
  }
  virtual void to_shared(int32_t coh_id)  {
    this->state = 1; 
    if(coh_id != -1){
      this->add_sharer(coh_id); 
      to_extend();
    }
  }
  virtual void to_modified(int32_t coh_id) {
    this->state = 2;
    if(coh_id != -1){
      this->add_sharer(coh_id); 
      to_extend();
    } 
  }
  virtual void sync(int32_t coh_id){
    if(coh_id != -1){
      this->delete_sharer(coh_id);
    }
  }
};


template<typename MT, bool isL1, bool isLLC> requires C_DERIVE2(MT, MetadataMSIBase, MetadataCoherenceSupport)
class MSIPolicy : public CohPolicyBase
{
public:
  MSIPolicy() : CohPolicyBase(1, 2, 3, 4, 0, 1, 2, 3) {}
  virtual ~MSIPolicy() {}

  virtual coh_cmd_t cmd_for_outer_acquire(coh_cmd_t cmd) const {
    assert(is_acquire(cmd));
    if(is_fetch_write(cmd)) return outer->cmd_for_write();
    else                    return outer->cmd_for_read();
  }

  virtual coh_cmd_t cmd_for_outer_flush(coh_cmd_t cmd) const {
    assert(is_flush(cmd));
    if(is_evict(cmd)) return outer->cmd_for_flush();
    else              return outer->cmd_for_writeback();
  }

  virtual std::pair<bool, coh_cmd_t> acquire_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const {
    if constexpr (!isL1) {
      assert(is_acquire(cmd));
      if(is_fetch_write(cmd)) return std::make_pair(true, coh_cmd_t{cmd.id, probe_msg, evict_act});
      else                    return need_sync(meta, cmd.id);
    } else return std::make_pair(false, cmd_for_null());
  }

  virtual std::pair<bool, coh_cmd_t> acquire_need_promote(coh_cmd_t cmd, const CMMetadataBase *meta) const {
    if constexpr (!isLLC) {
      assert(is_acquire(cmd));
      if(is_fetch_write(cmd) && !meta->is_modified())
        return std::make_pair(true, outer->cmd_for_write());
      else
        return std::make_pair(false, cmd_for_null());
    } else return std::make_pair(false, cmd_for_null());
  }

  virtual std::pair<bool, coh_cmd_t> probe_need_sync(coh_cmd_t outer_cmd, const CMMetadataBase *meta) const {
    if constexpr (!isL1) {
      assert(outer->is_probe(outer_cmd));
      if(outer->is_evict(outer_cmd)) return std::make_pair(true, coh_cmd_t{-1, probe_msg, evict_act});
      else                           return need_sync(meta, -1);
    } else return std::make_pair(false, cmd_for_null());
  }

  virtual std::pair<bool, coh_cmd_t> probe_need_probe(coh_cmd_t cmd, const CMMetadataBase *meta, int32_t target_inner_id) const {
    assert(is_probe(cmd));
    if(meta){
      auto meta_msi = static_cast<const MT *>(meta);
      if((is_evict(cmd)     && meta_msi->evict_need_probe(target_inner_id, cmd.id))     ||
        (is_writeback(cmd) && meta_msi->writeback_need_probe(target_inner_id, cmd.id)) ) {
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
  virtual std::pair<bool, coh_cmd_t> probe_need_writeback(coh_cmd_t outer_cmd, CMMetadataBase *meta){
    assert(outer->is_probe(outer_cmd));
    if(meta)
      if(meta->is_dirty()) return std::make_pair(true , outer->cmd_for_release_writeback());
      else                 return std::make_pair(false, outer->cmd_for_null());
    else                   return std::make_pair(false, outer->cmd_for_null());
  }
  

  virtual std::pair<bool, coh_cmd_t> writeback_need_sync(const CMMetadataBase *meta) const {
    if constexpr (!isL1) return std::make_pair(true, coh_cmd_t{-1, probe_msg, evict_act});
    else                 return std::make_pair(false, cmd_for_null());
  }

  virtual std::pair<bool, coh_cmd_t> flush_need_sync(coh_cmd_t cmd, const CMMetadataBase *meta) const {
    if constexpr (isLLC) {
      assert(is_flush(cmd));
      if(is_evict(cmd)) return std::make_pair(true, coh_cmd_t{-1, probe_msg, evict_act});
      else              return need_sync(meta, -1);
    } else
      return std::make_pair(false, cmd_for_null());
  }

};

template<typename MT, bool isLLC> requires C_DERIVE2(MT, MetadataMSIBase, MetadataCoherenceSupport)
class ExclusiveMSIPolicy : public MSIPolicy<MT, false, isLLC>, public ExclusivePolicySupportBase    // always not L1
{
public:
  virtual void meta_after_release(coh_cmd_t cmd, CMMetadataBase *mmeta, CMMetadataBase* meta, uint64_t addr, bool dirty){
    // meta transfer from directory (if use directory coherence protocol) to cache
    mmeta->init(addr);
    mmeta->to_shared(-1); 
    if(meta) { 
      static_cast<MT*>(mmeta)->set_sharer(static_cast<MT*>(meta)->get_sharer());  
      assert(!meta->is_dirty());
      meta->to_invalid(); 
    }
    if(dirty) mmeta->to_dirty();
  } 
  virtual std::pair<bool, coh_cmd_t> release_need_probe(coh_cmd_t cmd, CMMetadataBase* meta) {
    assert(this->is_release(cmd));
    return std::make_pair(true, coh_cmd_t{cmd.id, this->probe_msg, this->evict_act});
  }

  virtual bool need_writeback(const CMMetadataBase* meta) { return true; }

  virtual std::pair<bool, coh_cmd_t> inner_need_release(){
    return std::make_pair(true, this->cmd_for_release());
  }

  virtual void meta_after_probe_ack(coh_cmd_t cmd, CMMetadataBase *meta, int32_t inner_id) const{
    assert(this->is_probe(cmd));
    if(this->is_evict(cmd)) meta->sync(inner_id);
  }
};

#endif
