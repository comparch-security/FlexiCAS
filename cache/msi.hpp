#ifndef CM_CACHE_MSI_HPP
#define CM_CACHE_MSI_HPP

#include "cache/coherence.hpp"

// MSI protocol
class CohPolicyMSI {
public:
  static uint32_t cmd_probe_to_share();
  static uint32_t cmd_probe_to_invalid();
  static bool is_probe_to_share(uint32_t cmd);
  static bool is_probe_to_invalid(uint32_t cmd);
};

// metadata supporting MSI coherency
// AW    : address width
// TOfst : tag offset
template <int AW, int TOfst>
class MetadataMSI : public CMMetadataBase
{
protected:
  uint64_t     tag   : AW-TOfst;
  unsigned int state : 2; // 0: invalid, 1: shared, 2:modify
  unsigned int dirty : 1; // 0: clean, 1: dirty

  static const uint64_t mask = (1ull << (AW-TOfst)) - 1;

public:
  MetadataMSI() : tag(0), state(0), dirty(0) {}
  virtual ~MetadataMSI() {}

  virtual bool match(uint64_t addr) { return ((addr >> TOfst) & mask) == tag; }
  virtual void reset() { tag = 0; state = 0; dirty = 0; }
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

// uncached MSI outer port:
//   no support for reverse probe as if there is no internal cache
//   or the interl cache does not participate in the coherence communication
template<typename MT, typename DT>
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
      MT *meta = this->cache->access(ai, s, w); // oddly here, `this->' is required by the g++

      if(meta->is_dirty()) { // dirty, writeback
        meta_outer->to_dirty();
        if(data_outer) data_outer->copy(this->cache->get_data(ai, s, w));
        meta->to_clean();
      }

      if(CohPolicyMSI::is_probe_to_share(cmd))
        meta->to_shared();
      else if(CohPolicyMSI::is_probe_to_invalid(cmd))
        meta->to_invalid();
    }
  }
};

#endif
