#ifndef CM_CACHE_MEMORY_HPP
#define CM_CACHE_MEMORY_HPP

#include "cache/coherence.hpp"
#include <sys/mman.h>
#include <unordered_map>
#include <type_traits>

template<typename DT, typename DLY>
  requires C_DERIVE_OR_VOID(DT, CMDataBase) && C_DERIVE_OR_VOID(DLY, DelayBase)
class SimpleMemoryModel : public InnerCohPortUncached
{
protected:
  const std::string name;
  std::unordered_map<uint64_t, char *> pages;
  DLY *timer;      // delay estimator

  void allocate(uint64_t ppn) {
    char *page = static_cast<char *>(mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, 0, 0));
    assert(page != MAP_FAILED);
    pages[ppn] = page;
  }
  
public:
  SimpleMemoryModel(const std::string &n) : InnerCohPortUncached(nullptr), name(n) {
    if constexpr (!C_VOID(DLY)) timer = new DLY();
  }
  virtual ~SimpleMemoryModel() {
    if constexpr (!C_VOID(DLY)) delete timer;
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
    if constexpr (!C_VOID(DLY)) timer->read(addr, 0, 0, 0, 0, delay);
  }

  virtual void writeback_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay, bool dirty = true) {
    if constexpr (!C_VOID(DT)) {
      auto ppn = addr >> 12;
      auto offset = addr & 0x0fffull;
      assert(pages.count(ppn));
      uint64_t *mem_addr = reinterpret_cast<uint64_t *>(pages[ppn] + offset);
      for(int i=0; i<8; i++) mem_addr[i] = data_inner->read(i);
    }
    if constexpr (!C_VOID(DLY)) timer->write(addr, 0, 0, 0, 0, delay);
  }

private:
  virtual void query_loc_resp(uint64_t addr, std::list<LocInfo> *locs) {}
  virtual bool probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd, uint64_t *delay) { return false; } // hidden
};

#endif
