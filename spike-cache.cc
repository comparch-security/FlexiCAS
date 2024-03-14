#include "cache/memory.hpp"
#include "util/cache_type.hpp"
#include "cache/slicehash.hpp"
#include "util/query.hpp"
#include "flexicas-pfc.h"

#include <list>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <atomic>
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

#define CACHE_OP_READ        0
#define CACHE_OP_WRITE       1
#define CACHE_OP_FLUSH       2
#define CACHE_OP_WRITEBACK   3
#define CACHE_OP_FLUSH_CACHE 4

#define XACT_QUEUE_HIGH    100
#define XACT_QUEUE_LOW     10

namespace {
  static std::vector<CoreInterface *> core_data, core_inst;
  static std::vector<uint64_t> core_cycle; // record the cycle time in each core
  static MonitorBase *tracer;
  static int NC = 0;
  std::condition_variable xact_non_empty_notify, xact_non_full_notify;
  std::mutex xact_queue_op_mutex;
  std::mutex xact_queue_full_mutex;
  std::mutex xact_queue_empty_mutex;
  static bool exit_flag = false;
  static std::string pfc_log_prefix;
  static uint64_t pfc_value = 0;
  static std::atomic_bool cache_idle = false;

  struct cache_xact {
    char op_t;
    bool ic;
    int core;
    uint64_t addr;
  };

  std::deque<cache_xact> xact_queue;

  void xact_queue_add(cache_xact xact) {
    std::unique_lock queue_full_lock(xact_queue_full_mutex, std::defer_lock);
    while(true) {
      xact_queue_op_mutex.lock();
      if(xact_queue.size() >= XACT_QUEUE_HIGH) {
        //std::cerr << "FlexiCAS lost sync with Spike with a transaction back log of " << xact_queue.size() << " transactions!" << std::endl;
        xact_queue_op_mutex.unlock();
        xact_non_full_notify.wait(queue_full_lock);
      } else {
        xact_queue.push_back(xact);
        xact_non_empty_notify.notify_all();
        xact_queue_op_mutex.unlock();
        break;
      }
    }
  }

  void cache_server() {
    std::unique_lock queue_empty_lock(xact_queue_empty_mutex, std::defer_lock);
    cache_xact xact;
    bool busy;
    while(!exit_flag) {
      // get the next xact
      xact_queue_op_mutex.lock();
      busy = !xact_queue.empty();
      if(busy) {
        xact = xact_queue.front();
        xact_queue.pop_front();
        if(xact_queue.size() <= XACT_QUEUE_LOW)
          xact_non_full_notify.notify_all();
      }
      xact_queue_op_mutex.unlock();

      if(busy) {
        switch(xact.op_t) {
        case CACHE_OP_READ: // read
          if(xact.ic)
            core_inst[xact.core]->read(xact.addr, nullptr);
          else
            core_data[xact.core]->read(xact.addr, nullptr);
          break;
        case CACHE_OP_WRITE: //write
          core_data[xact.core]->write(xact.addr, nullptr, nullptr);
          break;
        case CACHE_OP_FLUSH: // flush
          core_data[xact.core]->flush(xact.addr, nullptr);
          break;
        case CACHE_OP_WRITEBACK: // writeback
          core_data[xact.core]->writeback(xact.addr, nullptr);
          break;
        case CACHE_OP_FLUSH_CACHE: // flush the whole cache
          assert(xact.ic); // only happens to IC
          core_inst[xact.core]->flush_cache(nullptr);
          break;
        default:
          assert(0 == "unknown op type!");
        }
      } else {
        using namespace std::chrono_literals;
        cache_idle = true;
        xact_non_empty_notify.wait_for(queue_empty_lock, 1ms);
        cache_idle = false;
      }
    }
  }

  void cache_sync() {
    using namespace std::chrono_literals;
    while(!cache_idle) std::this_thread::sleep_for(1ms);
  }

  static std::list<LocInfo> query_locs;
  static CacheBase* coloc_query_cache;
  static uint64_t   coloc_query_target;

  CacheBase* query_cache(uint64_t paddr, int core) {
    query_locs.clear();
    core_data[core]->query_loc(paddr, &query_locs);
    return query_locs.back().cache;
  }
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
    NC = ncore;
    core_cycle.resize(NC, 0);
    if(prefix) pfc_log_prefix = std::string(prefix); // not currently used by the simple tracer but other advanced tracer may need print out logs
    auto l1d = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, false, void, true>(NC, "l1d");
    core_data = get_l1_core_interface(l1d);
    auto l1i = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, true, void, true>(NC, "l1i");
    core_inst = get_l1_core_interface(l1i);
    auto l2 = cache_gen_l2_exc<L2IW, L2WN, void, MetadataBroadcastBase, ReplaceSRRIP, MSIPolicy, false, void, true>(NC, "l2");
    auto l3 = cache_gen_llc_inc<L3IW, L3WN, void, MetadataDirectoryBase, ReplaceSRRIP, MESIPolicy, void, true>(NC, "l3");
    auto dispatcher = new SliceDispatcher<SliceHashNorm<> >("disp", NC);
    auto mem = new SimpleMemoryModel<void,void,true>("mem");
    tracer = new SimpleTracer(true);

    for(int i=0; i<NC; i++) {
      l1i[i]->outer->connect(l2[i]->inner, l2[i]->inner->connect(l1i[i]->outer, true));
      l1d[i]->outer->connect(l2[i]->inner, l2[i]->inner->connect(l1d[i]->outer));
      dispatcher->connect(l3[i]->inner);
      l2[i]->outer->connect(dispatcher, l3[0]->inner->connect(l2[i]->outer));
      if(i>0) for(int j=0; j<NC; j++) l3[i]->inner->connect(l2[j]->outer);
      l3[i]->outer->connect(mem, mem->connect(l3[i]->outer));
      l1i[i]->attach_monitor(tracer);
      l1d[i]->attach_monitor(tracer);
      l2[i]->attach_monitor(tracer);
      l3[i]->attach_monitor(tracer);
    }

    mem->attach_monitor(tracer);

    // set up the cache server
    std::thread t(cache_server);
    t.detach();
  }

  void exit() {
    exit_flag = true;
  }

  void read(uint64_t addr, int core, bool ic) {
    assert(core < NC);
    xact_queue_add({CACHE_OP_READ, ic, core, addr});
  }

  void write(uint64_t addr, int core) {
    assert(core < NC);
    xact_queue_add({CACHE_OP_WRITE, false, core, addr});
  }

  void flush(uint64_t addr, int core) {
    assert(core < NC);
    xact_queue_add({CACHE_OP_FLUSH, false, core, addr});
  }

  void flush_icache(int core) {
    xact_queue_add({CACHE_OP_FLUSH_CACHE, true, core, 0});
  }

  void writeback(uint64_t addr, int core) {
    assert(core < NC);
    xact_queue_add({CACHE_OP_WRITEBACK, false, core, addr});
  }

  void csr_write(uint64_t cmd, int core, tlb_translate_func translator) {
    if(cmd == FLEXICAS_PFC_START) {
      tracer->start();
      return;
    }

    if(cmd == FLEXICAS_PFC_STOP) {
      tracer->stop();
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

  }

  uint64_t csr_read(int core) {
    // ToDo: connect this with monitor
    return pfc_value;
  }

  void bump_cycle(int step, int core) {
    core_cycle[core] += step;
  }
}
