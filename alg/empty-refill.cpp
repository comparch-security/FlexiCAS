#include "alg/common.hpp"
#include "util/statistics.hpp"

uint32_t dai, ds, dw;

int main(int argc, char **argv) {
  namespace po = boost::program_options;
  po::options_description desc("options");
  desc.add_options()
    ("help", "describe arguments")
    ("print", "print arguments")
    ("samples", po::value<int>(&samples), "number of sample for each test")
    ("init-warm", po::value<bool>(&init_warm), "whether to warmup initially")
    ("init-warm-prefetch", po::value<bool>(&init_warm_prefetch), "whether to use prefetch for initial warmup")
    ("same-target", po::value<bool>(&same_target), "whether to attack the same target address")
    ("prefetch-target", po::value<bool>(&prefetch_target), "whether to prefetch the target address")
    ("prefetch-rand-addr", po::value<bool>(&prefetch_rand_addr), "whether to prefetch random addresses")
    ("check-vbuf-hit", po::value<bool>(&check_vbuf_hit), "whether allow hit in victim buffer")
    ("trace-target-addr", po::value<bool>(&trace_target_addr), "whether to trace the target address")
    ;

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  if (vm.count("help")) {
    std::cout << desc << std::endl;
    return 1;
  }

  if (vm.count("print")) {
    std::cout << "samples = " << samples << std::endl;
    std::cout << "init-warm = " << init_warm << std::endl;
    std::cout << "init-warm-prefetch = " << init_warm_prefetch << std::endl;
    std::cout << "same-target = " << same_target << std::endl;
    std::cout << "prefetch-target = " << prefetch_target << std::endl;
    std::cout << "prefetch-rand-addr = " << prefetch_rand_addr << std::endl;
    std::cout << "check-vbuf-hit = " << check_vbuf_hit << std::endl;
    std::cout << "trace-target-addr = " << trace_target_addr << std::endl;
  }

  auto l3 = cache_hier_gen(); // sequential associative

  AddrTracer addr_tracer;
  SimpleAccMonitor pfc;
  l3[0]->attach_monitor(&pfc); pfc.start();
  if(trace_target_addr) { l3[0]->attach_monitor(&addr_tracer); addr_tracer.start(); }

  if(init_warm) warm_up(init_warm_prefetch);
  if(init_warm) warm_up(init_warm_prefetch);
  std::cout << "warm up finished" << std::endl;

  int succ = 0;
  std::set<uint64_t> evset; int evset_size = 0;
  uint64_t target = cm_get_random_uint64() & addr_mask;
  if(trace_target_addr) addr_tracer.set_target(target);
  auto llc = get_llc(target);
  auto stat_acc = init_mean_stat();
  auto stat_llc_acc = init_mean_stat();
  auto stat_llc_evict = init_mean_stat();

  for(int testN=0; testN<samples; testN++) {
    if(test_warm) warm_up(test_warm_prefetch);
    if(!same_target) {
      target = cm_get_random_uint64() & addr_mask;
      if(trace_target_addr) addr_tracer.set_target(target);
      llc = get_llc(target);
    }else if(!test_warm) {
      light_warm(1ull << L3IW);
    }

    uint64_t wall_clock_start = wall_clock;
    uint64_t llc_acc_start = simple_pfc->get_access();
    uint64_t llc_evict_start = simple_pfc->get_invalid();

    if(prefetch_target)  mem_prefetch(target);
    else                 mem_access(target);
    flush(target);

    uint64_t trial = 0;
    uint64_t addr;
    bool found = false;
    do {
      do {
        addr = cm_get_random_uint64() & addr_mask;
      } while (addr == target);
      auto prev_invalidate = pfc.get_invalid();
      //auto prev_reloc = tracer->get_reloc(); // use this will cause deadlock
      if(prefetch_rand_addr)  mem_prefetch(addr);
      else                    mem_access(addr);
      found = (prev_invalidate == pfc.get_invalid());
      trial++;
    } while(!found || trial >= L3WN * (1ull << L3IW));

    if(llc->query_coloc(target, addr)) {
      succ++;
      if(same_target) evset.insert(addr);
    }
    if(same_target) {
      std::set<uint64_t> removal;
      evset_size = 0;
      for(auto a:evset) if(llc->query_coloc(a, target)) evset_size++; else removal.insert(a);
      for(auto a:removal) evset.erase(a);
    }

    auto acc_num = wall_clock - wall_clock_start; record_mean_stat(stat_acc, acc_num);
    auto llc_acc_num = simple_pfc->get_access() - llc_acc_start; record_mean_stat(stat_llc_acc, llc_acc_num);
    auto llc_evict_num = simple_pfc->get_invalid() - llc_evict_start; record_mean_stat(stat_llc_evict, llc_evict_num);
    

    std::cout << "test " << testN << ": " << succ << " use " << trial << " random addresses";
    if(same_target)
      std::cout << ", collected " << evset_size << " cloc addrs.";
    else
      std::cout << ".";
    std::cout << " [" << simple_pfc->get_invalid() << ", " << evset_size
              << ", " << wall_clock - wall_clock_start
              << ", " << simple_pfc->get_access() - llc_acc_start
              << ", " << simple_pfc->get_invalid() - llc_evict_start
              << "]" << std::endl;
  }

  std::cout << "memory accesses: " << get_mean_mean(stat_acc) << ", +/-" << get_mean_error(stat_acc) <<  ", +/-" << get_mean_variance(stat_acc) << std::endl;
  std::cout << "llc accesses: "  << get_mean_mean(stat_llc_acc) << ", +/-" << get_mean_error(stat_llc_acc) <<  ", +/-" << get_mean_variance(stat_llc_acc) << std::endl;
  std::cout << "llc evictions: " << get_mean_mean(stat_llc_evict) << ", +/-" << get_mean_error(stat_llc_evict) <<  ", +/-" << get_mean_variance(stat_llc_evict) << std::endl;

  return 0;
}
