#include "replayer/st_parser.hpp"
#include "replayer/synchro_trace.hpp"
#include "util/cache_type.hpp"
#include "cache/memory.hpp"
#include "util/delay.hpp"

#define L1IW 4
#define L1WN 4

#define L2IW 5
#define L2WN 8

#define NCore 3

int main(){

  auto l1d = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, false, DelayL1<1, 1, 1>, true>(NCore, "l1d");
  auto core_data = get_l1_core_interface(l1d);
  auto l1i = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, true, DelayL1<1, 1, 1>, true>(NCore, "l1i");
  auto core_inst = get_l1_core_interface(l1i);
  auto l2 = cache_gen_l2_inc<L2IW, L2WN, void, MetadataDirectoryBase, ReplaceSRRIP, MESIPolicy, true, DelayCoherentCache<2, 3, 4>, true>(1, "l2")[0];
  auto mem = new SimpleMemoryModel<void,DelayMemory<10>,true>("mem");
  // SimpleTracer tracer(true);

  for(int i=0; i<NCore; i++) {
    l1i[i]->outer->connect(l2->inner, l2->inner->connect(l1i[i]->outer, true));
    l1d[i]->outer->connect(l2->inner, l2->inner->connect(l1d[i]->outer));
    // l1i[i]->attach_monitor(&tracer);
    // l1d[i]->attach_monitor(&tracer);
  }
  l2->outer->connect(mem, mem->connect(l2->outer));

  // l2->attach_monitor(&tracer);
  // mem->attach_monitor(&tracer);

  SynchroTraceReplayer<NCore, NCore, 1000> replayer(".", 1.0, 2.0, 1, 1, core_data);
  replayer.init();
  replayer.start();
}