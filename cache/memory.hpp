#ifndef CM_CACHE_MEMORY_HPP
#define CM_CACHE_MEMORY_HPP

#include "cache/coherence.hpp"
#include "cache/mi.hpp"
#include <sys/mman.h>
#include <unordered_map>

template<typename DT, typename DLY, bool EnMon = false>
  requires C_DERIVE_OR_VOID(DT, CMDataBase) && C_DERIVE_OR_VOID(DLY, DelayBase)
  class SimpleMemoryModel : public InnerCohPortUncached, public CacheMonitorSupport
{
protected:
  const uint32_t id;                    // a unique id to identify this memory
  const std::string name;
  std::unordered_map<uint64_t, char *> pages;
  DLY *timer;      // delay estimator

  void allocate(uint64_t ppn) {
    char *page = static_cast<char *>(mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, 0, 0));
    assert(page != MAP_FAILED);
    pages[ppn] = page;
  }
  
public:
  SimpleMemoryModel(const std::string &n)
    : InnerCohPortUncached(nullptr), id(UniqueID::new_id(n)), name(n)
  {
    InnerCohPortBase::policy = new MIPolicy<MetadataMI,false,false>();
    CacheMonitorSupport::monitors = new CacheMonitorImp<DLY, EnMon>(id);
  }

  virtual ~SimpleMemoryModel() {
    delete CacheMonitorSupport::monitors;
    delete InnerCohPortBase::policy;
  }

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) {
    if constexpr (!C_VOID(DT)) {
      auto ppn = addr >> 12;
      auto offset = addr & 0x0fffull;
      if(!pages.count(ppn)) allocate(ppn);
      uint64_t *mem_addr = reinterpret_cast<uint64_t *>(pages[ppn] + offset);
      data_inner->write(mem_addr);
      if(meta_inner) meta_inner->to_modified(-1);
    }
    hook_read(addr, 0, 0, 0, true, meta_inner, data_inner, delay);
  }

  virtual void writeback_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) {
    if constexpr (!C_VOID(DT)) {
      auto ppn = addr >> 12;
      auto offset = addr & 0x0fffull;
      assert(pages.count(ppn));
      uint64_t *mem_addr = reinterpret_cast<uint64_t *>(pages[ppn] + offset);
      for(int i=0; i<8; i++) mem_addr[i] = data_inner->read(i);
    }
    hook_write(addr, 0, 0, 0, true, true, meta_inner, data_inner, delay);
  }

  // monitor related
  void attach_monitor(MonitorBase *m) { CacheMonitorSupport::monitors->attach_monitor(m); }
  // support run-time assign/reassign mointors
  void detach_monitor() { CacheMonitorSupport::monitors->detach_monitor(); }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay) {
    if constexpr (EnMon || !C_VOID(DLY)) CacheMonitorSupport::monitors->hook_read(addr, -1, -1, -1, hit, meta, data, delay);
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool is_release, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay) {
    if constexpr (EnMon || !C_VOID(DLY)) CacheMonitorSupport::monitors->hook_write(addr, -1, -1, -1, hit, meta, data, delay);
  }

private:
  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay) {}
  virtual void query_loc_resp(uint64_t addr, std::list<LocInfo> *locs) {}
  virtual std::pair<bool,bool> probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd, uint64_t *delay) {
    return std::make_pair(false,false);
  } // hidden
};

#endif
