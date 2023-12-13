#include "cache/memory.hpp"
#include "util/cache_type.hpp"

#include <iostream>

// intel coffe lake 9Gen: https://en.wikichip.org/wiki/intel/microarchitectures/coffee_lake

#define NC 2

// 32K 8W, both I and D
#define L1IW 6
#define L1WN 8

// 256K, 4W, exclusive 
#define L2IW 10
#define L2WN 4

// 2M per core
#define L3IW (11+1)
#define L3WN 16

static std::vector<CoreInterface *> core_data, core_inst;

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

  void init() {
    auto l1d = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, false, void, true>(NC, "l1d");
    core_data = get_l1_core_interface(l1d);
    auto l1i = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, true, void, true>(NC, "l1i");
    core_inst = get_l1_core_interface(l1i);
    auto l2 = cache_gen_l2_exc<L2IW, L2WN, void, MetadataBroadcastBase, ReplaceSRRIP, MSIPolicy, false, void, true>(NC, "l2");
    auto l3 = cache_gen_llc_inc<L3IW, L3WN, void, MetadataDirectoryBase, ReplaceSRRIP, MESIPolicy, void, true>(1, "l3")[0];
    auto mem = new SimpleMemoryModel<void,void,true>("mem");
    SimpleTracer tracer(true);

    for(int i=0; i<NC; i++) {
      l1i[i]->outer->connect(l2[i]->inner, l2[i]->inner->connect(l1i[i]->outer, true));
      l1d[i]->outer->connect(l2[i]->inner, l2[i]->inner->connect(l1d[i]->outer));
      l1i[i]->attach_monitor(&tracer);
      l1d[i]->attach_monitor(&tracer);
      l2[i]->outer->connect(l3->inner, l3->inner->connect(l2[i]->outer));
      l2[i]->attach_monitor(&tracer);
    }

    l3->outer->connect(mem, mem->connect(l3->outer));
    l3->attach_monitor(&tracer);
    mem->attach_monitor(&tracer);

    std::cout << "cache model initialized!" << std::endl;
  }

  void read(uint64_t addr, int core, bool ic) {
    if(ic)
      core_inst[core]->read(addr, nullptr);
    else
      core_data[core]->read(addr, nullptr);
  }

  void write(uint64_t addr, int core) {
    core_data[core]->write(addr, nullptr, nullptr);
  }

  void flush(uint64_t addr, int core) {
    core_data[core]->flush(addr, nullptr);
  }

  void writeback(uint64_t addr, int core) {
    core_data[core]->writeback(addr, nullptr);
  }
}
