#include "cache/memory.hpp"
#include "util/cache_type.hpp"
#include "cache/slicehash.hpp"

#include<list>
#include<mutex>
#include<thread>
#include<condition_variable>
//#include<iostream>

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

//#define EnTrace


#define CACHE_OP_READ      1
#define CACHE_OP_WRITE     2
#define CACHE_OP_FLUSH     3
#define CACHE_OP_WRITEBACK 4

#define XACT_QUEUE_HIGH    100
#define XACT_QUEUE_LOW     10

namespace {
  static std::vector<CoreInterface *> core_data, core_inst;
  static SimpleTracer tracer(true);
  static int NC = 0;
  std::condition_variable xact_non_empty_notify, xact_non_full_notify;
  std::mutex xact_queue_op_mutex;
  std::mutex xact_queue_full_mutex;
  std::mutex xact_queue_empty_mutex;

  struct cache_xact {
    char op_t;
    bool ic;
    int core;
    uint64_t addr;
  };

  std::list<cache_xact> xact_queue;

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
    while(true) {
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
        case 0: // read
          if(xact.ic)
            core_inst[xact.core]->read(xact.addr, nullptr);
          else
            core_data[xact.core]->read(xact.addr, nullptr);
          break;
        case 1: //write
          core_data[xact.core]->write(xact.addr, nullptr, nullptr);
          break;
        case 2: // flush
          core_data[xact.core]->flush(xact.addr, nullptr);
          break;
        case 3: // writeback
          core_data[xact.core]->writeback(xact.addr, nullptr);
          break;
        default:
          assert(0 == "unknown op type!");
        }
      } else {
        xact_non_empty_notify.wait(queue_empty_lock);
      }
    }
  }
}

namespace flexicas {

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

  void init(int ncore) {
    NC = ncore;
    auto l1d = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, false, void, true>(NC, "l1d");
    core_data = get_l1_core_interface(l1d);
    auto l1i = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, true, void, true>(NC, "l1i");
    core_inst = get_l1_core_interface(l1i);
    auto l2 = cache_gen_l2_exc<L2IW, L2WN, void, MetadataBroadcastBase, ReplaceSRRIP, MSIPolicy, false, void, true>(NC, "l2");
    auto l3 = cache_gen_llc_inc<L3IW, L3WN, void, MetadataDirectoryBase, ReplaceSRRIP, MESIPolicy, void, true>(NC, "l3");
    auto dispatcher = new SliceDispatcher<SliceHashNorm<> >("disp", NC);
    auto mem = new SimpleMemoryModel<void,void,true>("mem");

    for(int i=0; i<NC; i++) {
      l1i[i]->outer->connect(l2[i]->inner, l2[i]->inner->connect(l1i[i]->outer, true));
      l1d[i]->outer->connect(l2[i]->inner, l2[i]->inner->connect(l1d[i]->outer));
      dispatcher->connect(l3[i]->inner);
      l2[i]->outer->connect(dispatcher, l3[0]->inner->connect(l2[i]->outer));
      if(i>0) for(int j=0; j<NC; j++) l3[i]->inner->connect(l2[j]->outer);
      l3[i]->outer->connect(mem, mem->connect(l3[i]->outer));
#ifdef EnTrace
      l1i[i]->attach_monitor(&tracer);
      l1d[i]->attach_monitor(&tracer);
      l2[i]->attach_monitor(&tracer);
      l3[i]->attach_monitor(&tracer);
#endif
    }

#ifdef EnTrace
    mem->attach_monitor(&tracer);
#endif

    // set up the cache server
    std::thread t(cache_server);
    t.detach();
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

  void writeback(uint64_t addr, int core) {
    assert(core < NC);
    xact_queue_add({CACHE_OP_WRITEBACK, false, core, addr});
  }
}
