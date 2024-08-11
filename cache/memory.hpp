#ifndef CM_CACHE_MEMORY_HPP
#define CM_CACHE_MEMORY_HPP

#include "cache/coherence.hpp"
#include "cache/mi.hpp"
#include <sys/mman.h>
#include <unordered_map>
#include <shared_mutex>

template<typename DT, typename DLY, bool EnMon = false, bool EnMT = false>
  requires C_DERIVE_OR_VOID<DT, CMDataBase> && C_DERIVE_OR_VOID<DLY, DelayBase>
  class SimpleMemoryModel : public InnerCohPortUncached<EnMT>, public CacheMonitorSupport
{
  using InnerCohPortBase::policy;

#ifdef CHECK_MULTI
  std::unordered_set<uint64_t> active_addr_set;
  std::mutex                   active_addr_mutex;

  void active_addr_add(uint64_t addr) {
    std::lock_guard lock(active_addr_mutex);
    assert(!active_addr_set.count(addr) || 0 ==
           "Two parallel memory accesses operating on the same memory location!");
    active_addr_set.insert(addr);
  }

  void active_addr_remove(uint64_t addr) {
    std::lock_guard lock(active_addr_mutex);
    assert(active_addr_set.count(addr) || 0 ==
           "memory access corrupted!");
    active_addr_set.erase(addr);
  }
#endif

protected:
  const uint32_t id;                    // a unique id to identify this memory
  const std::string name;
  std::unordered_map<uint64_t, char *> pages;
  DLY *timer;      // delay estimator
  std::shared_mutex         page_mtx;

  __always_inline char * allocate(uint64_t ppn) {
    char *page;
    bool miss = true;

    if constexpr (EnMT) {
      page_mtx.lock();
      miss = !pages.count(ppn);
    }

    if (miss) {
      page = static_cast<char *>(mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, 0, 0));
      assert(page != MAP_FAILED);
      pages[ppn] = page;
    } else
      page = pages[ppn];

    if constexpr (EnMT) page_mtx.unlock();
    return page;
  }
  
  __always_inline bool get_page(uint64_t ppn, char ** page) {
    if constexpr (EnMT) page_mtx.lock_shared();
    bool hit = pages.count(ppn);
    if(hit) *page = pages[ppn];
    if constexpr (EnMT) page_mtx.unlock_shared();
    return hit;
  }

public:
  SimpleMemoryModel(const std::string &n)
    : InnerCohPortUncached<EnMT>(nullptr), id(UniqueID::new_id(n)), name(n)
  {
    policy = policy_ptr(new MIPolicy<MetadataMI,false,false>());
    monitors = new CacheMonitorImp<DLY, EnMon>(id);
  }

  virtual ~SimpleMemoryModel() {
    delete monitors;
  }

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) override {
#ifdef CHECK_MULTI
    if constexpr (EnMT) active_addr_add(addr);
#endif
    if constexpr (!C_VOID<DT>) {
      auto ppn = addr >> 12;
      auto offset = addr & 0x0fffull;
      char * page;
      if(!get_page(ppn, &page)) page = allocate(ppn);
      uint64_t *mem_addr = reinterpret_cast<uint64_t *>(page + offset);
      data_inner->write(mem_addr);
    }
    if(meta_inner) meta_inner->to_modified(-1);
    hook_read(addr, 0, 0, 0, true, meta_inner, data_inner, delay);
#ifdef CHECK_MULTI
    if constexpr (EnMT) active_addr_remove(addr);
#endif
  }

  virtual void writeback_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) override {
#ifdef CHECK_MULTI
    if constexpr (EnMT) active_addr_add(addr);
#endif
    if constexpr (!C_VOID<DT>) {
      auto ppn = addr >> 12;
      auto offset = addr & 0x0fffull;
      char * page;
      bool hit = get_page(ppn, &page); assert(hit);
      uint64_t *mem_addr = reinterpret_cast<uint64_t *>(page + offset);
      for(int i=0; i<8; i++) mem_addr[i] = data_inner->read(i);
    }
    hook_write(addr, 0, 0, 0, true, true, meta_inner, data_inner, delay);
#ifdef CHECK_MULTI
    if constexpr (EnMT) active_addr_remove(addr);
#endif
  }

  // monitor related
  void attach_monitor(MonitorBase *m) { monitors->attach_monitor(m); }
  // support run-time assign/reassign mointors
  void detach_monitor() { monitors->detach_monitor(); }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay) override {
    if constexpr (EnMon || !C_VOID<DLY>) monitors->hook_read(addr, -1, -1, -1, hit, meta, data, delay);
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool demand_acc, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay) override {
    if constexpr (EnMon || !C_VOID<DLY>) monitors->hook_write(addr, -1, -1, -1, hit, meta, data, delay);
  }

private:
  virtual void hook_manage(uint64_t, uint32_t, uint32_t, uint32_t, bool, bool, bool, const CMMetadataBase *, const CMDataBase *, uint64_t *) override {}
  virtual void query_loc_resp(uint64_t, std::list<LocInfo> *) override {}
  virtual std::pair<bool,bool> probe_req(uint64_t, CMMetadataBase *, CMDataBase *, coh_cmd_t, uint64_t *) override { return std::make_pair(false,false); }
};

#endif
