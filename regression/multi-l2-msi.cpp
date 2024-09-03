#include "cache/metadata.hpp"
#include "cache/msi.hpp"
#include "cache/memory.hpp"
#include "util/cache_type.hpp"
#include "util/parallel_regression.hpp"

#define L1IW 4
#define L1WN 4

#define L2IW 5
#define L2WN 8

#define PAddrN 1024
#define SAddrN 256
#define NCore 4
#define TestN ((PAddrN + SAddrN) * NCore * 2)

//#define NCore 4
//#define PAddrN 128
//#define SAddrN 64
//#define TestN 512

PrintPool *globalPrinter;
#ifdef CHECK_MULTI
  LockCheck * global_lock_checker = new LockCheck;
#endif

int main(){
  using policy_l2 = MSIPolicy<false, true, policy_memory>;
  using policy_l1d = MSIPolicy<true, false, policy_l2>;
  using policy_l1i = MSIPolicy<true, true, policy_l2>;
  auto l1d = cache_gen_l1<L1IW, L1WN, Data64B, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1d, false, void, true, true>(NCore, "l1d");
  auto core_data = get_l1_core_interface(l1d);
  auto l1i = cache_gen_l1<L1IW, L1WN, Data64B, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1i, true, void, true, true>(NCore, "l1i");
  auto core_inst = get_l1_core_interface(l1i);
  auto l2 = cache_gen_inc<L2IW, L2WN, Data64B, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l2, true, void, true, true>(1, "l2")[0];
  auto mem = new SimpleMemoryModel<Data64B, void, true, true>("mem");
  globalPrinter = new PrintPool(256);
  SimpleTracerMT tracer(true);

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

  ParallelRegressionGen<NCore, true, false, PAddrN, SAddrN, Data64B> tgen;

  tgen.run(TestN, &core_inst, &core_data);

  tracer.stop();
  delete_caches(l1d);
  delete_caches(l1i);
  delete l2;
  delete mem;
  delete globalPrinter;

#ifdef CHECK_MULTI
  delete global_lock_checker;
#endif

  return 0;
}
