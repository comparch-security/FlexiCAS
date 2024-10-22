#include "cache/memory.hpp"
#include "cache/metadata.hpp"
#include "flexicas.h"
#include "util/cache_type.hpp"
#include "cache/slicehash.hpp"
#include "util/query.hpp"
#include "flexicas-pfc.h"

#include <algorithm>
#include <list>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <vector>
#include <unordered_set>

#define insn_length(x) \
  (((x) & 0x03) < 0x03 ? 2 : \
   ((x) & 0x1f) < 0x1f ? 4 : \
   ((x) & 0x3f) < 0x3f ? 6 : \
   8)
//#include <iostream>

// intel coffe lake 9Gen: https://en.wikichip.org/wiki/intel/microarchitectures/coffee_lake

// 32K 8W, both I and D
#define L1IW 6
#define L1WN 8

// 256K, 4W, exclusive 
#define L2IW 10
#define L2WN 4

// 2M per core
#define L3IW (11+1)
#define L3WN 16

// multithread support
// #define ENABLE_FLEXICAS_THREAD

#define CACHE_OP_READ         0
#define CACHE_OP_WRITE        1
#define CACHE_OP_FLUSH        2
#define CACHE_OP_WRITEBACK    3
#define CACHE_OP_FLUSH_CACHE  4
#define CACHE_OP_PREFETCH_LLC 5

#define XACT_QUEUE_HIGH    320
#define XACT_QUEUE_LOW     240
#define XACT_QUEUE_BURST   32

#define TRACE false

namespace {
  static std::vector<CoreInterfaceBase *> core_data, core_inst;
  static std::vector<uint64_t> core_cycle; // record the cycle time in each core
  static uint64_t wall_clock;              // a wall clock shared by all cores
  static MonitorBase *tracer;
  static int NC = 0;
  #define data_type Data64B
  #define USE_DATA
  static SimpleMemoryModel<data_type,void,TRACE>* mem;
  std::condition_variable xact_non_empty_notify, xact_non_full_notify;
  std::mutex xact_queue_op_mutex;
  std::mutex xact_queue_full_mutex;
  std::mutex xact_queue_empty_mutex;
  static bool exit_flag = false;
  static uint64_t pfc_value = 0;
  static std::atomic_bool cache_idle = false;
  static std::unordered_set<uint64_t> uncached_set;
  static bool enable_log = false;

  struct cache_xact {
    char op_t;
    bool ic;
    int core;
    uint64_t addr;
  };

  std::vector<cache_xact> xact_input(XACT_QUEUE_BURST);
  std::deque<cache_xact>  xact_queue;
  std::vector<cache_xact> xact_output(XACT_QUEUE_BURST);
  int input_index = 0, output_index = 0;

  void xact_queue_flush() {
    std::unique_lock queue_full_lock(xact_queue_full_mutex, std::defer_lock);
    while(true) {
      xact_queue_op_mutex.lock();
      if(xact_queue.size() >= XACT_QUEUE_HIGH) {
        //std::cerr << "FlexiCAS lost sync with Spike with a transaction back log of " << xact_queue.size() << " transactions!" << std::endl;
        xact_queue_op_mutex.unlock();
        xact_non_full_notify.wait(queue_full_lock);
      } else {
        for(int i=0; i<XACT_QUEUE_BURST; i++) xact_queue.push_back(xact_input[i]);
        xact_non_empty_notify.notify_all();
        xact_queue_op_mutex.unlock();
        input_index = 0;
        break;
      }
    }
  }

  void xact_queue_add(cache_xact xact) {
    xact_input[input_index++] = xact;
    if(input_index == XACT_QUEUE_BURST) xact_queue_flush();
  }

  inline const CMDataBase* read_detailed(uint64_t addr, int core, bool ic) {
    if(ic) return core_inst[core]->read(addr, nullptr);
    else   return core_data[core]->read(addr, nullptr);
    
  }

  inline void write_detailed(uint64_t addr, int core, CMDataBase* data, std::vector<uint64_t>& wmask) {
    core_data[core]->write(addr, data, wmask, nullptr);
  }

  inline void flush_detailed(uint64_t addr, int core) {
    core_data[core]->flush(addr, nullptr);
  }

  inline void writeback_detailed(uint64_t addr, int core) {
    core_data[core]->writeback(addr, nullptr);
  }

  inline void flush_icache_detailed(int core) {
    core_inst[core]->flush_cache(nullptr);
  }

  inline void prefetch_llc_detailed(uint64_t addr, int core) {
    core_data[core]->prefetch(addr, nullptr);
  }

  void cache_server() {
    std::unique_lock queue_empty_lock(xact_queue_empty_mutex, std::defer_lock);
    int burst_size = 0;
    while(!exit_flag) {
      // get the next xact
      xact_queue_op_mutex.lock();
      burst_size = xact_queue.size() > XACT_QUEUE_BURST ? XACT_QUEUE_BURST : xact_queue.size();
      for(int i=0; i<burst_size; i++) { xact_output[i] = xact_queue.front(); xact_queue.pop_front(); }
      if(xact_queue.size() <= XACT_QUEUE_LOW)
        xact_non_full_notify.notify_all();
      xact_queue_op_mutex.unlock();

      for(int i=0; i<burst_size; i++) {
        cache_xact &xact = xact_output[i];
        switch(xact.op_t) {
        case CACHE_OP_READ:         read_detailed(xact.addr, xact.core, xact.ic); break; // read
        // case CACHE_OP_WRITE:        write_detailed(xact.addr, xact.core); break;         // write
        case CACHE_OP_FLUSH:        flush_detailed(xact.addr, xact.core); break;         // flush
        case CACHE_OP_WRITEBACK:    writeback_detailed(xact.addr, xact.core); break;     // writeback
        case CACHE_OP_FLUSH_CACHE:  flush_icache_detailed(xact.core); break;             // flush the whole cache
        case CACHE_OP_PREFETCH_LLC: prefetch_llc_detailed(xact.addr, xact.core); break;  // prefetch
        default: assert(0 == "unknown op type!");
        }
      }

      if(burst_size == 0) {
        using namespace std::chrono_literals;
        cache_idle = true;
        xact_non_empty_notify.wait_for(queue_empty_lock, 1ms);
        cache_idle = false;
      }
    }
  }

  void cache_sync() {
#ifdef ENABLE_FLEXICAS_THREAD
    using namespace std::chrono_literals;
    xact_queue_flush();
    while(!cache_idle) std::this_thread::sleep_for(1ms);
#endif
  }

  static std::list<LocInfo> query_locs;
  static CacheBase* coloc_query_cache;
  static uint64_t   coloc_query_target;

  CacheBase* query_cache(uint64_t paddr, int core) {
    query_locs.clear();
    core_data[core]->query_loc(paddr, &query_locs);
    return query_locs.back().cache;
  }

  static std::string pfc_str; // a string buffer used by PFC's CSR interface

}

namespace flexicas {
  typedef std::function<uint64_t(uint64_t)> tlb_translate_func;

  int  ncore() { return NC; }

  int  cache_level() {return 3; }

  int  cache_set(int level, bool ic) {
    switch(level) {
    case 1: return 1 << L1IW;
    case 2: return 1 << L2IW;
    case 3: return 1 << L3IW;
    default: return 0;
    }
  }

  int  cache_way(int level, bool ic) {
    switch(level) {
    case 1: return 1 << L1WN;
    case 2: return 1 << L2WN;
    case 3: return 1 << L3WN;
    default: return 0;
    }
  }

  void init(int ncore, const char *prefix) {
    using policy_l2 = MESIPolicy<false, true, policy_memory>;
    using policy_l1d = MSIPolicy<true, false, policy_l2>;
    using policy_l1i = MSIPolicy<true, true, policy_l2>;
    NC = ncore;
    core_cycle.resize(NC, 0);
    wall_clock = 0;
    auto l1d = cache_gen_l1<L1IW, L1WN, data_type, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1d, false, void, TRACE>(NC, "l1d");
    core_data = get_l1_core_interface(l1d);
    auto l1i = cache_gen_l1<L1IW, L1WN, data_type, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1i, true, void, TRACE>(NC, "l1i");
    core_inst = get_l1_core_interface(l1i);
    auto l2 = cache_gen_inc<L2IW, L2WN, data_type, MetadataBroadcastBase, ReplaceSRRIP, MESIPolicy, policy_l2, true, void, TRACE>(NC, "l2")[0];
    mem = new SimpleMemoryModel<data_type,void,TRACE>("mem");
    tracer = new SimpleTracer(true);
    if(prefix) tracer->set_prefix(std::string(prefix));

    for(int i=0; i<NC; i++) {
      l1i[i]->outer->connect(l2->inner);
      l1d[i]->outer->connect(l2->inner);
      // l1i[i]->attach_monitor(tracer);
      // l1d[i]->attach_monitor(tracer);
    }

    // l2->attach_monitor(tracer);
    l2->outer->connect(mem);

    // mem->attach_monitor(tracer);
    // tracer->start();

#ifdef ENABLE_FLEXICAS_THREAD
    // set up the cache server
    std::thread t(cache_server);
    t.detach();
#endif
  }

  void exit() {
    exit_flag = true;
  }

  uint64_t read(uint64_t addr, int core, bool ic, uint64_t len) {
    assert(core < NC);
#ifdef ENABLE_FLEXICAS_THREAD
    xact_queue_add({CACHE_OP_READ, ic, core, addr});
#else
  #ifdef USE_DATA
    auto cache_data = (read_detailed(addr, core, ic))->get_data(); // normally more than one instruction is readed per refill
    uint64_t caddr = addr & ~0x3f;
    size_t offset = addr - caddr;
    uint8_t *byte_ptr = (uint8_t *)cache_data;
    uint64_t data = (uint64_t)(*(uint16_t*)(byte_ptr + offset));
    if (ic) len = insn_length(data);

    if (offset + len > 64){
      auto second_data = (read_detailed(caddr + 64, core, ic))->get_data();
      auto second_data_ptr = (uint8_t*)second_data;
      if (len == 4) {
        data |= (uint64_t)(*(const uint16_t*)(second_data_ptr)) << 16;
      } else if (len == 2) {
        // entire instruction already fetched
      } else if (len == 6) {
        data |= (uint64_t)(*(const uint16_t*)(second_data_ptr)) << 16;
        data |= (uint64_t)(*(const uint16_t*)(second_data_ptr+2)) << 32;
      } else {
        data |= (uint64_t)(*(const uint16_t*)(second_data_ptr)) << 16;
        data |= (uint64_t)(*(const uint16_t*)(second_data_ptr+2)) << 32;
        data |= (uint64_t)(*(const uint16_t*)(second_data_ptr+4)) << 48;
      }
    } else {
      if (len == 4) {
        data |= (uint64_t)(*(const uint16_t*)(byte_ptr+offset+2)) << 16;
      } else if (len == 2) {
        // entire instruction already fetched
      } else if (len == 1){
        data &= 0xFF;
      } else if (len == 6) {
        data |= (uint64_t)(*(const uint16_t*)(byte_ptr+offset+2)) << 16;
        data |= (uint64_t)(*(const uint16_t*)(byte_ptr+offset+4)) << 32;
      } else {
        data |= (uint64_t)(*(const uint16_t*)(byte_ptr+offset+2)) << 16;
        data |= (uint64_t)(*(const uint16_t*)(byte_ptr+offset+4)) << 32;
        data |= (uint64_t)(*(const uint16_t*)(byte_ptr+offset+6)) << 48;
      }
    }
    return data;
  #else
    read_detailed(addr, core, ic);
    return 0;
  #endif
#endif
  }

  void write(uint64_t addr, int core, uint64_t len, uint8_t* data) {
    assert(core < NC);
#ifdef ENABLE_FLEXICAS_THREAD
    xact_queue_add({CACHE_OP_WRITE, false, core, addr});
#else
  #ifdef USE_DATA
    std::vector<uint64_t> wmask(8, 0);
    uint64_t array[8] = {0};
    uint64_t caddr = addr & ~0x3f;
    uint64_t taddr = addr;
    int index = 0;
    for (int i = 0; i < 8 && len > 0; i++){
      uint64_t block_start = caddr + i * 8;
      uint64_t block_end = block_start + 8;
      if (taddr >= block_start && taddr < block_end) {
        auto begin = taddr - block_start;
        auto offset = std::min(len, block_end-taddr);
        uint64_t temp = 0;
        memcpy(&temp, data + index, offset);
        array[i] = temp << (begin * 8);
        for(uint64_t j = begin; j < begin + offset; j++) wmask[i] |= ((0xFFULL) << (j*8)); 
        index += offset;
        taddr += offset;
        len -= offset;
      } else {
        array[i] = 0; wmask[i] = 0;
      }       
    }
    Data64B d(array);
    write_detailed(addr, core, &d, wmask);
    if (taddr >= (caddr + 64) && len > 0){
      auto begin = taddr - caddr - 64;
      auto offset = len;
      uint64_t second_array[8] = {0};
      uint64_t temp = 0;
      memcpy(&temp, data + index, offset);
      second_array[0] = temp << (begin * 8);
      std::vector<uint64_t> second_wmask(8, 0);
      for(uint64_t j = begin; j < begin + offset; j++) second_wmask[0] |= (0xFFULL << (j * 8));
      Data64B sd(second_array);
      write_detailed(caddr + 64, core, &sd, second_wmask);
    }
  #else 
    std::vector<uint64_t> wmask(8, 0);
    write_detailed(addr, core, nullptr, wmask);
  #endif
#endif
  }

  void flush(uint64_t addr, int core) {
    assert(core < NC);
#ifdef ENABLE_FLEXICAS_THREAD
    xact_queue_add({CACHE_OP_FLUSH, false, core, addr});
#else
    flush_detailed(addr, core);
#endif
  }

  void flush_icache(int core) {
#ifdef ENABLE_FLEXICAS_THREAD
    xact_queue_add({CACHE_OP_FLUSH_CACHE, true, core, 0});
#else
    flush_icache_detailed(core);
#endif
  }

  void writeback(uint64_t addr, int core) {
    assert(core < NC);
#ifdef ENABLE_FLEXICAS_THREAD
    xact_queue_add({CACHE_OP_WRITEBACK, false, core, addr});
#else
    writeback_detailed(addr, core);
#endif
  }

  void prefetch_llc(uint64_t addr, int core) {
    assert(core < NC);
#ifdef ENABLE_FLEXICAS_THREAD
    xact_queue_add({CACHE_OP_PREFETCH_LLC, false, core, addr});
#else
    prefetch_llc_detailed(addr, core);
#endif
  }

  void csr_write(uint64_t cmd, int core, tlb_translate_func translator) {
    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_CMD && (cmd & FLEXICAS_PFC_CMD_MASK) == FLEXICAS_PFC_START) {
      enable_log = true;
      tracer->start();
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_CMD && (cmd & FLEXICAS_PFC_CMD_MASK) == FLEXICAS_PFC_STOP) {
      tracer->stop();
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_CMD && (cmd & FLEXICAS_PFC_CMD_MASK) == FLEXICAS_PFC_STR_CHAR) {
      char c = (cmd >> 16) & 0xff;
      pfc_str.push_back(c);
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_CMD && (cmd & FLEXICAS_PFC_CMD_MASK) == FLEXICAS_PFC_STR_CLR) {
      pfc_str.clear();
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_CMD && (cmd & FLEXICAS_PFC_CMD_MASK) == FLEXICAS_PFC_PREFIX) {
      if(tracer) tracer->set_prefix(pfc_str);
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_QUERY) {
      uint64_t addr = FLEXICAS_PFC_EXTRACT_ADDR(cmd);
      uint64_t paddr = translator(addr);
      cache_sync();
      pfc_value = query_cache(paddr, core)->hit(paddr);
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_FLUSH) {
      uint64_t addr = FLEXICAS_PFC_EXTRACT_ADDR(cmd);
      uint64_t paddr = translator(addr);
      flush(paddr, core);
      cache_sync();
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_CONGRU_TARGET) {
      uint64_t addr = FLEXICAS_PFC_EXTRACT_ADDR(cmd);
      uint64_t paddr = translator(addr);
      cache_sync();
      coloc_query_cache = query_cache(paddr, core);
      coloc_query_target = paddr;
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_CONGRU_QUERY) {
      uint64_t addr = FLEXICAS_PFC_EXTRACT_ADDR(cmd);
      uint64_t paddr = translator(addr);
      cache_sync();
      pfc_value = coloc_query_cache->query_coloc(coloc_query_target, paddr);
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_PREFETCH_LLC) {
      uint64_t addr = FLEXICAS_PFC_EXTRACT_ADDR(cmd);
      uint64_t paddr = translator(addr);
      prefetch_llc(paddr, core);
      cache_sync();
      return;
    }

  }

  uint64_t csr_read(int core) {
    // ToDo: connect this with monitor
    return pfc_value;
  }

  void bump_cycle(int step, int core) {
    core_cycle[core] += step;
  }

  void bump_wall_clock(int step) {
    wall_clock += step;
  }

  void init_memory(std::map<uint64_t, char*>& map){
    mem->init_memory(map);
  }

  void write_memory(uint64_t addr, uint64_t data){
    mem->write_uint64(addr, data);
  }

  void add_uncached_addr(uint64_t addr) {
    uncached_set.insert(addr & ~0x3f);
  }

  bool contain_uncached_addr(uint64_t addr) {
    return uncached_set.contains(addr & ~0x3f);
  }

  bool enable_write_log(){
    return enable_log;
  }
}
