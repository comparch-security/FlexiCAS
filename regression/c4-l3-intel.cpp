#include "cache/memory.hpp"
#include "util/cache_type.hpp"
#include "util/regression.hpp"
#include "cache/slicehash.hpp"

#define PAddrN 1024
#define SAddrN 256
#define NCore 4
#define TestN ((PAddrN + SAddrN) * NCore * 2)

#define L1IW 4
#define L1WN 4

#define L2IW 5
#define L2WN 8

#define L3IW 5
#define L3WN 16

int main() {
  auto l1d = cache_gen_l1<L1IW, L1WN, Data64B, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, false, void, true>(NCore, "l1d");
  auto core_data = get_l1_core_interface(l1d);
  auto l1i = cache_gen_l1<L1IW, L1WN, Data64B, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, true, void, true>(NCore, "l1i");
  auto core_inst = get_l1_core_interface(l1i);
  auto l2 = cache_gen_l2_exc<L2IW, L2WN, Data64B, MetadataBroadcastBase, ReplaceSRRIP, MSIPolicy, false, void, true>(NCore, "l2");
  auto l3 = cache_gen_llc_inc<L3IW, L3WN, Data64B, MetadataDirectoryBase, ReplaceSRRIP, MESIPolicy, void, true>(NCore, "l3");
  auto dispatcher = new SliceDispatcher<SliceHashIntelCAS>("disp", NCore);
  auto mem = new SimpleMemoryModel<Data64B,void,true>("mem");
  SimpleTracer tracer(true);

  for(int i=0; i<NCore; i++) {
    l1i[i]->outer->connect(l2[i]->inner, l2[i]->inner->connect(l1i[i]->outer, true));
    l1d[i]->outer->connect(l2[i]->inner, l2[i]->inner->connect(l1d[i]->outer));
    dispatcher->connect(l3[i]->inner);
    l2[i]->outer->connect(dispatcher, l3[0]->inner->connect(l2[i]->outer));
    if(i>0) for(int j=0; j<NCore; j++) l3[i]->inner->connect(l2[j]->outer);
    l3[i]->outer->connect(mem, mem->connect(l3[i]->outer));
    l1i[i]->attach_monitor(&tracer);
    l1d[i]->attach_monitor(&tracer);
    l2[i]->attach_monitor(&tracer);
    l3[i]->attach_monitor(&tracer);
  }
  mem->attach_monitor(&tracer);

  RegressionGen<NCore, true, true, PAddrN, SAddrN, Data64B> tgen;
  return tgen.run(TestN, core_inst, core_data);
}
