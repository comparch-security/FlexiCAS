#ifndef CM_CACHE_MEMORY_HPP
#define CM_CACHE_MEMORY_HPP

#include "cache/coherence.hpp"
#include <sys/mman.h>
#include <unordered_map>
#include <type_traits>

template<typename DT, typename DLY,
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type, // DT <- CMDataBase or void
         typename = typename std::enable_if<std::is_base_of<DelayBase, DLY>::value || std::is_void<DLY>::value>::type>  // DLY <- DelayBase or void
class SimpleMemoryModel : public CohMasterBase
{
protected:
  std::string name;
  std::unordered_map<uint64_t, char *> pages;
  DLY *timer;      // delay estimator

  void allocate(uint64_t ppn) {
    char *page = static_cast<char *>(mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, 0, 0));
    assert(page != MAP_FAILED);
    pages[ppn] = page;
  }
  
public:
  SimpleMemoryModel(const std::string &n) : name(n) {
    if constexpr (!std::is_void<DLY>::value) timer = new DLY();
  }
  virtual ~SimpleMemoryModel() {
    if constexpr (!std::is_void<DLY>::value) delete timer;
  }

  virtual void acquire_resp(uint64_t addr, CMDataBase *data, uint32_t cmd, uint64_t *delay) {
    if constexpr (!std::is_void<DT>::value) {
      auto ppn = addr >> 12;
      auto offset = addr & 0x0fffull;
      if(!pages.count(ppn)) allocate(ppn);
      uint64_t *mem_addr = reinterpret_cast<uint64_t *>(pages[ppn] + offset);
      data->write(mem_addr);
    }
    if constexpr (!std::is_void<DLY>::value) timer->read(addr, 0, 0, 0, 0, delay);
  }

  virtual void writeback_resp(uint64_t addr, CMDataBase *data, uint32_t cmd, uint64_t *delay) {
    if constexpr (!std::is_void<DT>::value) {
      auto ppn = addr >> 12;
      auto offset = addr & 0x0fffull;
      assert(pages.count(ppn));
      uint64_t *mem_addr = reinterpret_cast<uint64_t *>(pages[ppn] + offset);
      for(int i=0; i<8; i++) mem_addr[i] = data->read(i);
    }
    if constexpr (!std::is_void<DLY>::value) timer->write(addr, 0, 0, 0, 0, delay);
  }

private:
  virtual void probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd, uint64_t *delay) {} // hidden
};

#endif
