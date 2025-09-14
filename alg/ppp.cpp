#include "alg/common.hpp"
#include "util/statistics.hpp"

uint32_t dai, ds, dw;

int main(int argc, char **argv) {
  check_vbuf_hit = false;

  namespace po = boost::program_options;
  po::options_description desc("options");
  desc.add_options()
    ("help", "describe arguments")
    ("print", "print arguments")
    ("samples", po::value<int>(&samples), "number of sample for each test")
    ("init-warm", po::value<bool>(&init_warm), "whether to warmup initially")
    ("init-warm-prefetch", po::value<bool>(&init_warm_prefetch), "whether to use prefetch for initial warmup")
    ("test-warm", po::value<bool>(&test_warm), "whether to warmup between tests")
    ("test-warm-prefetch", po::value<bool>(&test_warm_prefetch), "whether to use prefetch for test warmup")
    ("same-target", po::value<bool>(&same_target), "whether to attack the same target address")
    ("prefetch-target", po::value<bool>(&prefetch_target), "whether to prefetch the target address")
    ("prefetch-rand-addr", po::value<bool>(&prefetch_rand_addr), "whether to prefetch random addresses")
    ("check-vbuf-hit", po::value<bool>(&check_vbuf_hit), "whether allow hit in victim buffer")
    ("prime-round", po::value<uint32_t>(&prime_round), "number of prime round")
    ("candidate-ratio", po::value<float>(&candidate_ratio), "ratio of the candidate set to LLC size")
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
    std::cout << "test-warm = " << test_warm << std::endl;
    std::cout << "test-warm-prefetch = " << test_warm_prefetch << std::endl;
    std::cout << "same-target = " << same_target << std::endl;
    std::cout << "prefetch-target = " << prefetch_target << std::endl;
    std::cout << "prefetch-rand-addr = " << prefetch_rand_addr << std::endl;
    std::cout << "check-vbuf-hit = " << check_vbuf_hit << std::endl;
    std::cout << "prime-round = " << prime_round << std::endl;
    std::cout << "candidate-ratio = " << candidate_ratio << std::endl;
    std::cout << "trace-target-addr = " << trace_target_addr << std::endl;
  }

  auto l3 = cache_hier_gen(); // sequential associative

  AddrTracer addr_tracer;
  if(trace_target_addr) { l3[0]->attach_monitor(&addr_tracer); addr_tracer.start(); }

  if(init_warm) warm_up(init_warm_prefetch);
  std::cout << "warm up finished" << std::endl;

  int pp_succ = 0;
  int ppp_succ = 0;
  int ppp_with_cloc = 0;
  std::set<uint64_t> final_evset; int final_evset_size = 0;

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
    }
    uint32_t pool_size = (1<<L3IW)*L3WN*candidate_ratio;
    uint64_t wall_clock_start = wall_clock;
    uint64_t llc_acc_start = simple_pfc->get_access();
    uint64_t llc_evict_start = simple_pfc->get_invalid();
    std::set<uint64_t> addr_pool;
    for(unsigned int i=0; i<pool_size; i++)
      addr_pool.insert(cm_get_random_uint64() & addr_mask);

    // prime
    for(uint32_t round = 0; round<prime_round; round++)
      for(auto a : addr_pool) {
        if(prefetch_rand_addr)  mem_prefetch(a);
        else                    mem_access(a);
      }

    // prune
    bool all_cached;
    do {
      std::set<uint64_t> removal;
      all_cached = true;
      for(auto a : addr_pool) {
        if(!(check_vbuf_hit ? llc->hit(a) : llc->hit(a, &dai, &ds, &dw, 1, false))) {
          removal.insert(a);
          all_cached = false;
        }
        if(prefetch_rand_addr)  mem_prefetch(a);
        else                    mem_access(a);
      }
      for(auto a: removal) addr_pool.erase(a);
    } while(!all_cached);

    int pp_colc_addr_num = 0;
    for(auto a : addr_pool) {
      if(llc->query_coloc(target, a))
        pp_colc_addr_num++;
    }
    if(pp_colc_addr_num >= L3WN) pp_succ++;
    auto pruned_pool_size = addr_pool.size();

    // probe
    if(prefetch_target)  mem_prefetch(target);
    else                 mem_access(target);
    std::set<uint64_t> evset;
    for(auto a: addr_pool) {
      if(!(check_vbuf_hit ? llc->hit(a) : llc->hit(a, &dai, &ds, &dw, 1, false))) { evset.insert(a); final_evset.insert(a); }
      if(prefetch_rand_addr)  mem_prefetch(a);
      else                    mem_access(a);
    }

    int ppp_colc_addr_num = 0;
    for(auto a : evset) {
      if(llc->query_coloc(target, a))
        ppp_colc_addr_num++;
    }
    if(ppp_colc_addr_num >= L3WN) ppp_succ++;
    if(ppp_colc_addr_num > 0) ppp_with_cloc++;

    if(same_target) {
      std::set<uint64_t> removal;
      final_evset_size = 0;
      for(auto a:final_evset) if(llc->query_coloc(a, target)) final_evset_size++; else removal.insert(a);
      for(auto a:removal) final_evset.erase(a);
    }

    auto acc_num = wall_clock - wall_clock_start; record_mean_stat(stat_acc, acc_num);
    auto llc_acc_num = simple_pfc->get_access() - llc_acc_start; record_mean_stat(stat_llc_acc, llc_acc_num);
    auto llc_evict_num = simple_pfc->get_invalid() - llc_evict_start; record_mean_stat(stat_llc_evict, llc_evict_num);
    

    std::cout << "test " << testN << ": " << ppp_succ << " use " << pruned_pool_size << " random addresses";
    std::cout << ", collected " << ppp_colc_addr_num << " cloc addr" << std::endl;
    //std::cout << " [" << simple_pfc->get_invalid() << ", " << final_evset_size
    //          << ", " << wall_clock - wall_clock_start
    //          << ", " << simple_pfc->get_access() - llc_acc_start
    //          << ", " << simple_pfc->get_invalid() - llc_evict_start
    //          << "]" << std::endl;
  }

  std::cout << "success rate: " << ppp_succ / (float)(samples) << " using " << get_mean_mean(stat_llc_evict) << " evictions." << std::endl;

  //std::cout << "memory accesses: " << get_mean_mean(stat_acc) << ", +/-" << get_mean_error(stat_acc) <<  ", +/-" << get_mean_variance(stat_acc) << std::endl;
  //std::cout << "llc accesses: "  << get_mean_mean(stat_llc_acc) << ", +/-" << get_mean_error(stat_llc_acc) <<  ", +/-" << get_mean_variance(stat_llc_acc) << std::endl;
  //std::cout << "llc evictions: " << get_mean_mean(stat_llc_evict) << ", +/-" << get_mean_error(stat_llc_evict) <<  ", +/-" << get_mean_variance(stat_llc_evict) << std::endl;

  return 0;
}
