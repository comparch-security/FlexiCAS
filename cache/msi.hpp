#ifndef CM_CACHE_MSI_HPP
#define CM_CACHE_MSI_HPP

#include <cassert>
#include <type_traits>
#include "cache/coherence.hpp"

namespace // file visibility
{
  // MSI protocol
  class Policy {
  public:
    static uint32_t cmd_probe_to_share();
    static uint32_t cmd_probe_to_invalid();
    static bool is_probe_to_share(uint32_t cmd);
    static bool is_probe_to_invalid(uint32_t cmd);
    static bool need_sync(uint32_t cmd, CMMetadataBase *meta);
    static uint32_t cmd_for_sync(uint32_t cmd);
    static void meta_after_probe(uint32_t cmd, CMMetadataBase *meta);
    static void meta_after_grant(uint32_t cmd, CMMetadataBase *meta);
    static void meta_after_acquire(uint32_t cmd, CMMetadataBase *meta);
    static uint32_t cmd_for_evict();
  };

}

// metadata supporting MSI coherency
class MetadataMSIBase : public CMMetadataBase
{
protected:
  unsigned int state : 2; // 0: invalid, 1: shared, 2:modify
  unsigned int dirty : 1; // 0: clean, 1: dirty
public:
  MetadataMSIBase() : state(0), dirty(0) {}
  virtual ~MetadataMSIBase() {}

  virtual void to_invalid() { state = 0; }
  virtual void to_shared() { state = 1; }
  virtual void to_modified() { state = 2; }
  virtual void to_dirty() { dirty = 1; }
  virtual void to_clean() { dirty = 0; }
  virtual bool is_valid() const { return state; }
  virtual bool is_shared() const { return state == 1; }
  virtual bool is_modified() const {return state == 2; }
  virtual bool is_dirty() const { return dirty; }
};

// Metadata with match function
// AW    : address width
// TOfst : tag offset
template <int AW, int TOfst>
class MetadataMSI : public MetadataMSIBase
{
protected:
  uint64_t     tag   : AW-TOfst;
  static const uint64_t mask = (1ull << (AW-TOfst)) - 1;

public:
  MetadataMSI() : tag(0) {}
  virtual ~MetadataMSI() {}

  virtual bool match(uint64_t addr) { return ((addr >> TOfst) & mask) == tag; }
  virtual void reset() { tag = 0; state = 0; dirty = 0; }
};


// uncached MSI outer port:
//   no support for reverse probe as if there is no internal cache
//   or the interl cache does not participate in the coherence communication
template<typename MT, typename DT,
         typename = typename std::enable_if<std::is_base_of<MetadataMSIBase, MT>::value>::type, // MT <- MetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
class OuterPortMSIUncached : public OuterCohPortBase
{
public:
  virtual void acquire_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd) {
    coh->acquire_resp(addr, meta, data, cmd);
  }
  virtual void writeback_req(uint64_t addr, CMDataBase *data) {
    coh->writeback_resp(addr, data);
  }
};

// full MSI Outer port
template<typename MT, typename DT>
class OuterPortMSI : public OuterPortMSIUncached<MT, DT>
{
public:
  virtual void probe_resp(uint64_t addr, CMMetadataBase *meta_outer, CMDataBase *data_outer, uint32_t cmd) {
    uint32_t ai, s, w;
    if(this->cache->hit(addr, &ai, &s, &w)) {
      auto meta = this->cache->access(ai, s, w); // oddly here, `this->' is required by the g++ 11.3.0 @wsong83
      CMDataBase *data = std::is_void<DT>::value ? nullptr : this->cache->get_data(ai, s, w);

      // sync if necessary
      if(this->inner && Policy::need_sync(cmd, meta)) this->inner->probe_req(addr, meta, data, Policy::cmd_for_sync(cmd));

      // writeback if dirty
      if(meta->is_dirty()) { // dirty, writeback
        meta_outer->to_dirty();
        if(!std::is_void<DT>::value) data_outer->copy(data);
        meta->to_clean();
      }

      // update meta
      Policy::meta_after_probe(cmd, meta);
    }
  }
};

// uncached MSI inner port:
//   no support for reverse probe as if there is no internal cache
//   or the interl cache does not participate in the coherence communication
template<typename MT, typename DT,
         typename = typename std::enable_if<std::is_base_of<MetadataMSIBase, MT>::value>::type, // MT <- MetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
class InnerPortMSIUncached : public InnerCohPortBase
{
public:
  virtual void acquire_resp(uint64_t addr, CMMetadataBase *meta_inner, CMDataBase *data_inner, uint32_t cmd) {
    uint32_t ai, s, w;
    CMMetadataBase *meta;
    CMDataBase *data;
    if(this->cache->hit(addr, &ai, &s, &w)) { // hit
      meta = this->cache->access(ai, s, w);
      if(!std::is_void<DT>::value) data = this->cache->get_data(ai, s, w);

      // sync if necessary
      if(Policy::need_sync(cmd, meta)) probe_req(addr, meta, data, Policy::cmd_for_sync(cmd));
    } else { // miss
      assert(outer);

      // get the way to be replaced
      this->cache->replace(addr, &ai, &s, &w);
      meta = this->cache->access(ai, s, w);
      if(!std::is_void<DT>::value) data = this->cache->get_data(ai, s, w);

      // sync if necessary
      if(Policy::need_sync(Policy::cmd_for_evict(), meta)) probe_req(addr, meta, data, Policy::cmd_for_sync(Policy::cmd_for_evict()));

      // writeback if dirty
      if(meta->is_dirty()) outer->writeback_req(addr, meta, data);

      // fetch the missing block
      outer->acquire_req(addr, meta, data, cmd);
    }
    // grant
    if(!std::is_void<DT>::value) data_inner->copy(this->cache->get_data(ai, s, w));
    Policy::meta_after_acquire(cmd, meta);
    Policy::meta_after_grant(cmd, meta_inner);
  }
};

#endif
