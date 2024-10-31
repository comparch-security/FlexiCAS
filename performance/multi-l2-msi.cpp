#include "cache/metadata.hpp"
#include "cache/msi.hpp"
#include "cache/memory.hpp"
#include "util/cache_type.hpp"
#include "util/parallel_regression.hpp"
#include <chrono>
#include <iostream>
#include <cstdlib>  
#define L1IW 4
#define L1WN 4

#define L2IW 10
#define L2WN 16

#define PAddrN 1024
#define SAddrN 256
#define MCore 4
#define TestN ((PAddrN + SAddrN) * 200 * 2)

#define Repe 10

typedef void data_type;

#ifdef CHECK_MULTI
  LockCheck * global_lock_checker = new LockCheck;
#endif

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <NCore>" << std::endl;
    return 1;
  }

  int NCore = std::atoi(argv[1]);

  assert(NCore <= MCore);

  using policy_l2 = MSIPolicy<false, true, policy_memory>;
  using policy_l1d = MSIPolicy<true, false, policy_l2>;
  using policy_l1i = MSIPolicy<true, true, policy_l2>;
  
  auto l1d = cache_gen_l1<L1IW, L1WN, data_type, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1d, false, void, false, true>(MCore, "l1d");
  auto core_data = get_l1_core_interface(l1d);
  auto l1i = cache_gen_l1<L1IW, L1WN, data_type, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1i, true, void, false, true>(MCore, "l1i");
  auto core_inst = get_l1_core_interface(l1i);
  auto l2 = cache_gen_inc<L2IW, L2WN, data_type, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l2, true, void, false, true>(1, "l2")[0];
  auto mem = new SimpleMemoryModel<data_type, void, false, true>("mem");

  for(int i = 0; i < MCore; i++) {
    l1i[i]->outer->connect(l2->inner);
    l1d[i]->outer->connect(l2->inner);
  }
  l2->outer->connect(mem);

  ParallelRegressionGen<true, true, PAddrN, SAddrN, data_type> tgen(NCore);

  int plan = 0;
  std::chrono::duration<double> dura(0);

  for(int i = 0; i < Repe; i++) {
    tgen.init(plan, TestN);
    auto start = std::chrono::steady_clock::now();
    tgen.run(plan, TestN, &core_inst, &core_data);
    auto end = std::chrono::steady_clock::now();
    dura += end - start;
  }

  std::cout << "Cache Level: " << 2 << std::endl;
  std::cout << "MCore: " << MCore << std::endl;
  std::cout << "NCore: " << NCore << std::endl;
  std::cout << "Repe: " << Repe << std::endl;
  std::cout << "TestN: " << TestN << std::endl;
  std::cout << "Average Time: " << dura.count()/Repe << "s" << std::endl;

  delete_caches(l1d);
  delete_caches(l1i);
  delete l2;
  delete mem;

#ifdef CHECK_MULTI
  delete global_lock_checker;
#endif

  return 0;
}
