#include "cache/memory.hpp"
#include "util/cache_type.hpp"
#include "util/regression.hpp"

#define PAddrN 256
#define SAddrN 128
#define NCore 2
#define TestN ((PAddrN + SAddrN) * NCore * 2)

#define L1IW 4
#define L1WN 4

#define L2IW 5
#define L2WN 4
#define L2EW 1
#define L2P  2
#define L2RN 2

int main() {
  using mirage_gen = ct::mirage::types<L2IW, L2WN, L2EW, L2P, L2RN, Data64B, ReplaceSRRIP, ReplaceRandom, policy_memory, void, true, true>;
  using policy_l2 = mirage_gen::policy_type;
  using policy_l1d = MSIPolicy<true, false, policy_l2>;
  using policy_l1i = MSIPolicy<true, true, policy_l2>;
  auto l1d = cache_gen_l1<L1IW, L1WN, Data64B, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1d, false, void, true>(NCore, "l1d");
  auto core_data = get_l1_core_interface(l1d);
  auto l1i = cache_gen_l1<L1IW, L1WN, Data64B, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1i, true, void, true>(NCore, "l1i");
  auto core_inst = get_l1_core_interface(l1i);
  auto l2 = mirage_gen::cache_gen_mirage(1, "l2")[0];
  auto mem = new SimpleMemoryModel<Data64B,void,true>("mem");
  SimpleTracer tracer(true);

  for(int i=0; i<NCore; i++) {
    l1i[i]->outer->connect(l2->inner);
    l1d[i]->outer->connect(l2->inner);
    l1i[i]->attach_monitor(&tracer);
    l1d[i]->attach_monitor(&tracer);
  }
  l2->outer->connect(mem);

  l2->attach_monitor(&tracer);
  mem->attach_monitor(&tracer);
  tracer.start();

  RegressionGen<NCore, true, false, PAddrN, SAddrN, Data64B> tgen;
  auto rv = tgen.run(TestN, core_inst, core_data);

  tracer.stop();
  delete_caches(l1d);
  delete_caches(l1i);
  delete l2;
  delete mem;
  return rv;
}
