#ifndef CM_CACHE_MEMORY_MULTI_HPP
#define CM_CACHE_MEMORY_MULTI_HPP

#include "cache/memory.hpp"

template<typename DT, typename DLY, bool EnMon = false>
  requires C_DERIVE_OR_VOID<DT, CMDataBase> && C_DERIVE_OR_VOID<DLY, DelayBase>

class SimpleMultiThreadMemoryModel : public SimpleMemoryModel<DT, DLY, EnMon>, public InnerCohPortMultiThreadSupport
{
  typedef SimpleMemoryModel<DT, DLY, EnMon> SmT;
protected:
  using SmT::allocate;
  using SmT::pages;
  using SmT::timer;
  using SmT::hook_read;
  using SmT::hook_write;
  std::mutex mtx;

  char* get_base_address(uint64_t ppn, bool assert){
    /**
     * Due to unordered_map not being able to read and write from multiple threads 
     * simultaneously, mutex must be used to ensure that only one thread is using the page map 
     */
    char* base;
    std::unique_lock lk(mtx);
    if(assert) assert(pages.count(ppn));
    if(!pages.count(ppn)) allocate(ppn);
    base = pages[ppn];
    return base;
  }
public:
  SimpleMultiThreadMemoryModel(const std::string &n): SmT(n) { }

  virtual ~SimpleMultiThreadMemoryModel() {}

  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay){
    if constexpr (!C_VOID<DT>) {
      auto ppn = addr >> 12;
      auto offset = addr & 0x0fffull;
      auto base = get_base_address(ppn, false);
      uint64_t *mem_addr = reinterpret_cast<uint64_t *>(base + offset);
      data_inner->write(mem_addr);
    }
    if(meta_inner) meta_inner->to_modified(-1);
    hook_read(addr, 0, 0, 0, true, meta_inner, data_inner, delay);
  }

  virtual void writeback_resp(uint64_t addr, CMDataBase *data_inner, CMMetadataBase *meta_inner, coh_cmd_t cmd, uint64_t *delay) {
    if constexpr (!C_VOID<DT>) {
      auto ppn = addr >> 12;
      auto offset = addr & 0x0fffull;
      auto base = get_base_address(ppn, true);
      uint64_t *mem_addr = reinterpret_cast<uint64_t *>(base + offset);
      for(int i=0; i<8; i++) mem_addr[i] = data_inner->read(i);
    }
    hook_write(addr, 0, 0, 0, true, true, meta_inner, data_inner, delay);
  }

private:
  virtual void acquire_ack_resp(uint64_t addr, coh_cmd_t cmd, uint64_t *delay) {}
  virtual std::tuple<CMMetadataBase *, CMDataBase *, uint32_t, uint32_t, uint32_t, std::mutex*, 
          std::condition_variable*, std::vector<uint32_t>*, bool, std::mutex*>
          access_line_multithread(uint64_t addr, coh_cmd_t cmd, uint64_t *delay)
  {
    return std::make_tuple(nullptr, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, nullptr);
  }

};

#endif
