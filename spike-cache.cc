#include "cache/memory.hpp"
#include "util/cache_type.hpp"
#include "cache/slicehash.hpp"
#include "util/query.hpp"
#include "rsa/monitor.hpp"
#include "rsa/cache.hpp"
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
#define L3IW (11+3)
#define L3WN 16
#define L3VW 8
#define L3P 2
#define L3EW 8
#define L3RN 3

// multithread support
#define ENABLE_FLEXICAS_THREAD

#define CACHE_OP_READ         0
#define CACHE_OP_WRITE        1
#define CACHE_OP_FLUSH        2
#define CACHE_OP_WRITEBACK    3
#define CACHE_OP_FLUSH_CACHE  4
#define CACHE_OP_PREFETCH_LLC 5

#define XACT_QUEUE_HIGH    320
#define XACT_QUEUE_LOW     240
#define XACT_QUEUE_BURST   32

namespace {
  static std::vector<CoreInterfaceBase *> core_data, core_inst;
  static std::vector<uint64_t> core_cycle; // record the cycle time in each core
  static uint64_t wall_clock;              // a wall clock shared by all cores
  static EvictDistMonitor *tracer;
  static int NC = 0;
  std::condition_variable xact_non_empty_notify, xact_non_full_notify;
  std::mutex xact_queue_op_mutex;
  std::mutex xact_queue_full_mutex;
  std::mutex xact_queue_empty_mutex;
  static bool exit_flag = false;
  static uint64_t pfc_value = 0;
  static std::atomic_bool cache_idle = false;

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

  inline void read_detailed(uint64_t addr, int core, bool ic) {
    if(ic) core_inst[core]->read(addr, nullptr);
    else   core_data[core]->read(addr, nullptr);
  }

  inline void write_detailed(uint64_t addr, int core) {
    core_data[core]->write(addr, nullptr, nullptr);
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
        case CACHE_OP_WRITE:        write_detailed(xact.addr, xact.core); break;         // write
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

  // get our own statically randomized LLC
  using policy_l3 = MESIPolicy<false, true, policy_memory>;
  using policy_l2 = ExclusiveMSIPolicy<false, false, policy_l3, false>;
  using policy_l1d = MSIPolicy<true, false, policy_l2>;
  using policy_l1i = MSIPolicy<true, true, policy_l2>;

  inline auto llc_gen_static_random(int size, const std::string& name_prefix) {
    static const bool EnMon = true;
    typedef void DT;
    typedef void DLY;
    typedef IndexRandom<L3IW,6> index_type;
    typedef ReplaceLRU<L3IW,L3WN,true,true,false> replace_type;
    typedef InnerCohPort<policy_l3, false> input_type;
    typedef OuterCohPortUncached<policy_l3, false> output_type;
    typedef MetadataMESIDirectory<48, 0, 6> metadata_type;
    typedef CacheNorm<L3IW, L3WN, metadata_type, DT, index_type, replace_type, DLY, EnMon, false> cache_base_type;
    typedef CoherentCacheNorm<cache_base_type, output_type, input_type> cache_type;
    return cache_generator<cache_type>(size, name_prefix);
  }

  inline auto llc_gen_skew(int size, const std::string& name_prefix) {
    static const bool EnMon = true;
    typedef void DT;
    typedef void DLY;
    typedef IndexSkewed<L3IW, 6, L3P> index_type;
    typedef ReplaceLRU<L3IW,L3WN / L3P,true,true,false> replace_type;
    typedef InnerCohPort<policy_l3, false> input_type;
    typedef OuterCohPortUncached<policy_l3, false> output_type;
    typedef MetadataMESIDirectory<48, 0, 6> metadata_type;
    typedef CacheSkewed<L3IW, L3WN / L3P, L3P, metadata_type, DT, index_type, replace_type, DLY, EnMon, false, 4> cache_base_type;
    typedef CoherentCacheNorm<cache_base_type, output_type, input_type> cache_type;
    return cache_generator<cache_type>(size, name_prefix);
  }

  // generate a sequental associative cache
  inline auto llc_gen_seqass(int size, const std::string& name_prefix) {
    static const bool EnMon = true;
    typedef void DT;
    typedef void DLY;
    typedef IndexRandom<L3IW,6> index_type;
    typedef SeqAssReplaceLRU<L3IW,L3WN,true,true,false> replace_type;
    typedef SeqAssMetadata<MetadataMESIDirectory<48, 0, 6> > metadata_type;
    typedef SeqAssCache<L3IW, L3WN, metadata_type, DT, index_type, replace_type, DLY, EnMon, (L3WN * (1 << L3IW))> cache_base_type;
    typedef InnerCohPortT<SeqAssInnerCohPortUncached, policy_l3, false, metadata_type, cache_base_type> input_type;
    typedef OuterCohPortUncached<policy_l3, false> output_type;
    typedef CoherentCacheNorm<cache_base_type, output_type, input_type> cache_type;
    return cache_generator<cache_type>(size, name_prefix);
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
    wall_clock = 0;
    auto l1d = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1d, false, void, true>(NC, "l1d");
    core_data = get_l1_core_interface(l1d);
    auto l1i = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1i, true, void, true>(NC, "l1i");
    core_inst = get_l1_core_interface(l1i);
    auto l2 = cache_gen_exc<L2IW, L2WN, void, MetadataBroadcastBase, ReplaceSRRIP, ExclusiveMSIPolicy, policy_l2, false, void, true>(NC, "l2");

    auto mem = new SimpleMemoryModel<void,void,true>("mem");
    auto mem_pfc = new SimpleAccMonitor;

    auto l3 = cache_gen_inc<L3IW, L3WN, void, MetadataDirectoryBase, ReplaceLRU, MESIPolicy, policy_l3, true, void, true>(1, "l3");
    tracer = new EvictDistMonitor(1, L3IW, L3WN*64, (1<<L3IW)*L3WN, &wall_clock, &core_cycle, mem_pfc); // period is set to ACC1

    //using remap_gen = ct::remap::types<L3IW, L3WN, 1, void, ReplaceLRU, policy_memory, void, true>;  //sp2021
    //auto l3 = remap_gen::cache_gen_remap(1, "l3");  // sp2021
    //tracer = new EvictDistMonitor(1, L3IW, L3WN*64, (1<<L3IW)*L3WN, &wall_clock, &core_cycle, mem_pfc); // period is set to ACC1

    //using remap_gen = ct::remap::types<L3IW, L3WN / L3P, L3P, void, ReplaceSRRIP, policy_memory, void, true>;  //ceaser-s
    //auto l3 = remap_gen::cache_gen_remap(1, "l3");  // ceaser-s and zsev
    //tracer = new EvictDistMonitor(2, L3IW, L3WN*64, (1<<L3IW)*L3WN, &wall_clock, &core_cycle, mem_pfc); // period is set to ACC1

    //using chameleon_gen = ct::chameleon::types<L3IW, L3WN / L3P, L3VW, L3P, void, ReplaceLRU, policy_memory, void, true>;  // chameleon cache
    //auto l3 = chameleon_gen::cache_gen_chameleon(1, "l3");  // chameleon cache
    //tracer = new EvictDistMonitor(2, L3IW, L3WN*64, (1<<L3IW)*L3WN, &wall_clock, &core_cycle, mem_pfc); // period is set to ACC1

    //using mirage_gen = ct::mirage::types<L3IW, L3WN / L3P, L3WN / 2 / L3P, L3P, 3, void, true, ReplaceRandom, ReplaceRandom, policy_memory, void, true, true>;  // mirage-50
    //auto l3 = mirage_gen::cache_gen_mirage(1, "l3");  // mirage
    //tracer = new EvictDistMonitor(2, L3IW, L3WN*64, (1<<L3IW)*L3WN, &wall_clock, &core_cycle, mem_pfc); // period is set to ACC1

    //using mirage_gen = ct::mirage::types<L3IW, L3WN / L3P, L3WN * 3 / 4 / L3P, L3P, 0, void, true, ReplaceRandom, ReplaceRandom, policy_memory, void, true, false>;  // mirage-75
    //auto l3 = mirage_gen::cache_gen_mirage(1, "l3");  // mirage
    //tracer = new EvictDistMonitor(2, L3IW, L3WN*64, (1<<L3IW)*L3WN, &wall_clock, &core_cycle, mem_pfc); // period is set to ACC1

    //auto l3 = llc_gen_seqass(1, "l3"); // sequential associative
    //tracer = new EvictDistMonitor(1, L3IW, L3WN*64, (1<<L3IW)*L3WN, &wall_clock, &core_cycle, mem_pfc); // period is set to ACC1

    if(prefix) tracer->set_prefix(std::string(prefix));

    for(int i=0; i<NC; i++) {
      l1i[i]->outer->connect(l2[i]->inner);
      l1d[i]->outer->connect(l2[i]->inner);
      l2[i]->outer->connect(l3[0]->inner);
      //l1i[i]->attach_monitor(tracer);
      //l1d[i]->attach_monitor(tracer);
      //l2[i]->attach_monitor(tracer);
    }
    l3[0]->outer->connect(mem);
    l3[0]->attach_monitor(tracer);
    mem->attach_monitor(mem_pfc);

    //auto remapper = new ZSEVRemapper<L3IW>(1.0 / 32, (1ul << L3IW) * 4, (1ul << L3IW) * L3WN * 10, 10, true);  // zsev
    //auto remapper = new ACCRemapper((1<<L3IW)*L3WN*100);  // ceaser-s
    //auto remapper = new SeqAssRemapMonitor((1<<L3IW)/4, 32);
    //l3[0]->attach_monitor(remapper); remapper->start();


#ifdef ENABLE_FLEXICAS_THREAD
    // set up the cache server
    std::thread t(cache_server);
    t.detach();
#endif
  }

  void exit() {
    exit_flag = true;
  }

  void read(uint64_t addr, int core, bool ic) {
    assert(core < NC);
#ifdef ENABLE_FLEXICAS_THREAD
    xact_queue_add({CACHE_OP_READ, ic, core, addr});
#else
    read_detailed(addr, core, ic);
#endif
  }

  void write(uint64_t addr, int core) {
    assert(core < NC);
#ifdef ENABLE_FLEXICAS_THREAD
    xact_queue_add({CACHE_OP_WRITE, false, core, addr});
#else
    write_detailed(addr, core);
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
}
