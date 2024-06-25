#include "cache/metadata.hpp"
#include "cache/policy_multi.hpp"
#include "cache/msi.hpp"
#include "cache/memory.hpp"
#include "cache/cache_multi.hpp"
#include "cache/coherence_multi.hpp"
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

int main(){
  auto l1d = cache_gen_multi_thread_l1<L1IW, L1WN, Data64B, MetadataBroadcastBase, ReplaceLRU_MT, MSIMultiThreadPolicy, false, false, void, true>(NCore, "l1d");
  auto core_data = get_l1_core_interface(l1d);
  auto l1i = cache_gen_multi_thread_l1<L1IW, L1WN, Data64B, MetadataBroadcastBase, ReplaceLRU_MT, MSIMultiThreadPolicy, false, true, void, true>(NCore, "l1i");
  auto core_inst = get_l1_core_interface(l1i);

  auto l2 = cache_gen_multi_thread_l2<L2IW, L2WN, Data64B, MetadataBroadcastBase, ReplaceLRU_MT, MSIMultiThreadPolicy, true, void, true>(1, "l2")[0];
  auto mem = new SimpleMemoryModel<Data64B, void, true, true>("mem");
  SimpleTracerMT tracer(true);

  for(int i=0; i<NCore; i++) {
    l1i[i]->outer->connect(l2->inner, l2->inner->connect(l1i[i]->outer, true));
    l1d[i]->outer->connect(l2->inner, l2->inner->connect(l1d[i]->outer));
    l1i[i]->attach_monitor(&tracer);
    l1d[i]->attach_monitor(&tracer);
  }
  l2->outer->connect(mem, mem->connect(l2->outer));

  l2->attach_monitor(&tracer);
  mem->attach_monitor(&tracer);

  ParallelRegressionGen<NCore, true, false, PAddrN, SAddrN, Data64B> tgen;

  tgen.run(TestN, &core_inst, &core_data);

  tracer.stop();
  delete_caches(l1d);
  delete_caches(l1i);
  delete l2;
  delete mem;

}
