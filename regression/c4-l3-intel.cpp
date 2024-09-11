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
  using policy_l3 = MESIPolicy<false, true, policy_memory>;
  using policy_l2 = ExclusiveMSIPolicy<false, false, policy_l3, false>;
  using policy_l1d = MSIPolicy<true, false, policy_l2>;
  using policy_l1i = MSIPolicy<true, true, policy_l2>;
  auto l1d = cache_gen_l1<L1IW, L1WN, Data64B, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1d, false, void, true>(NCore, "l1d");
  auto core_data = get_l1_core_interface(l1d);
  auto l1i = cache_gen_l1<L1IW, L1WN, Data64B, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1i, true, void, true>(NCore, "l1i");
  auto core_inst = get_l1_core_interface(l1i);
  auto l2 = cache_gen_exc<L2IW, L2WN, Data64B, MetadataBroadcastBase, ReplaceSRRIP, ExclusiveMSIPolicy, policy_l2, false, void, true>(NCore, "l2");
  auto l3 = cache_gen_inc<L3IW, L3WN, Data64B, MetadataDirectoryBase, ReplaceSRRIP, MESIPolicy, policy_l3, true, void, true>(NCore, "l3");
  auto dispatcher = new SliceDispatcher<SliceHashIntelCAS>("disp", NCore);
  auto mem = new SimpleMemoryModel<Data64B,void,true>("mem");
  SimpleTracer tracer(true);

  for(int i=0; i<NCore; i++) {
    l1i[i]->outer->connect(l2[i]->inner);
    l1d[i]->outer->connect(l2[i]->inner);
    dispatcher->connect(l3[i]->inner);
    l2[i]->outer->connect_by_dispatch(dispatcher, l3[0]->inner);
    if constexpr (!policy_l2::is_uncached()) // normally this check is useless as L2 is cached, but provied as an example
      for(int j=1; j<NCore; j++) l3[j]->inner->connect(l2[i]->outer);
    l3[i]->outer->connect(mem);
    l1i[i]->attach_monitor(&tracer);
    l1d[i]->attach_monitor(&tracer);
    l2[i]->attach_monitor(&tracer);
    l3[i]->attach_monitor(&tracer);
  }
  mem->attach_monitor(&tracer);
  tracer.start();

  RegressionGen<NCore, true, true, PAddrN, SAddrN, Data64B> tgen;
  auto rv = tgen.run(TestN, core_inst, core_data);

  tracer.stop();
  delete_caches(l1d);
  delete_caches(l1i);
  delete_caches(l2);
  delete_caches(l3);
  delete mem;
  return rv;
}
