#ifndef CM_CACHE_MEMORY_HPP
#define CM_CACHE_MEMORY_HPP

#include "cache/coherence.hpp"
#include <sys/mman.h>
#include <unordered_map>
#include <type_traits>

template<typename DT,
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
class SimpleMemoryModel : public CohMasterBase
{
  std::string name;
  std::unordered_map<uint64_t, char *> pages;

  void allocate(uint64_t ppn) {
    char *page = static_cast<char *>(mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, 0, 0));
    assert(page != MAP_FAILED);
    pages[ppn] = page;
  }
  
public:
  SimpleMemoryModel(const std::string &n) : name(n) {}
  virtual ~SimpleMemoryModel() {}

  virtual void acquire_resp(uint64_t addr, CMDataBase *data, uint32_t cmd) {
    if(!std::is_void<DT>::value) {
      auto ppn = addr >> 12;
      auto offset = addr & 0x0fffull;
      if(!pages.count(ppn)) allocate(ppn);
      uint64_t *mem_addr = reinterpret_cast<uint64_t *>(pages[ppn] + offset);
      data->write(mem_addr);
    }
  }

  virtual void writeback_resp(uint64_t addr, CMDataBase *data, uint32_t cmd) {
    if(!std::is_void<DT>::value) {
      auto ppn = addr >> 12;
      auto offset = addr & 0x0fffull;
      assert(pages.count(ppn));
      uint64_t *mem_addr = reinterpret_cast<uint64_t *>(pages[ppn] + offset);
      for(int i=0; i<8; i++) mem_addr[i] = data->read(i);
    }
  }

private:
  virtual void probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd) {} // hidden
  virtual void writeback_invalidate_resp() {}
};

#endif
