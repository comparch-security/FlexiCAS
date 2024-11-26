#include "replayer/st_parser.hpp"
#include "replayer/synchro_trace.hpp"
#include "util/cache_type.hpp"
#include "util/delay.hpp"
#include "cache/memory.hpp"
#include "cache/metadata.hpp"
#include "cache/msi.hpp"

#define L1IW 4
#define L1WN 4

#define L2IW 5
#define L2WN 8

#define NThread 4
#define NCore 4

int main(){

  using policy_l2 = MSIPolicy<false, true, policy_memory>;
  using policy_l1d = MSIPolicy<true, false, policy_l2>;
  using policy_l1i = MSIPolicy<true, true, policy_l2>;

  auto l1d = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1d, false, DelayL1<1, 1, 1>, true, true>(NCore, "l1d");
  auto core_data = get_l1_core_interface(l1d);
  auto l1i = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1i, true, DelayL1<1, 1, 1>, true, true>(NCore, "l1i");
  auto core_inst = get_l1_core_interface(l1i);

  auto l2 = cache_gen_inc<L2IW, L2WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l2, true, DelayCoherentCache<2, 3, 4>, true, true>(1, "l2")[0];
  auto mem = new SimpleMemoryModel<void, DelayMemory<10>, true, true>("mem");
  
  for(int i=0; i<NCore; i++) {
    l1i[i]->outer->connect(l2->inner);
    l1d[i]->outer->connect(l2->inner);
  }
  l2->outer->connect(mem);

  SynchroTraceReplayer<NThread, NCore> replayer("./performance", 1.0, 2.0, 1, 1, 300, core_data);
  replayer.init();
  replayer.start();
}