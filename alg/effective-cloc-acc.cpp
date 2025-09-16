#include "alg/common.hpp"
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
    ("test-warm", po::value<bool>(&test_warm), "whether to warmup between tests")
    ("test-warm-prefetch", po::value<bool>(&test_warm_prefetch), "whether to use prefetch for test warmup")
    ("same-target", po::value<bool>(&same_target), "whether to attack the same target address")
    ("prefetch-target", po::value<bool>(&prefetch_target), "whether to prefetch the target address")
    ("use-cloc-addr", po::value<bool>(&use_cloc_addr), "whether to prime using congruent addresses")
    ("use-rand-addr", po::value<bool>(&use_rand_addr), "whether to prime using random addresses")
    ("prefetch-cloc-addr", po::value<bool>(&prefetch_cloc_addr), "whether to prefetch congruent addresses")
    ("prefetch-rand-addr", po::value<bool>(&prefetch_rand_addr), "whether to prefetch random addresses")
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
    std::cout << "use-cloc-addr = " << use_cloc_addr << std::endl;
    std::cout << "prefetch-cloc-addr = " << prefetch_cloc_addr << std::endl;
    std::cout << "use-rand-addr = " << use_rand_addr << std::endl;
    std::cout << "prefetch-rand-addr = " << prefetch_rand_addr << std::endl;
    std::cout << "trace-target-addr = " << trace_target_addr << std::endl;
  }
  
  auto l3 = cache_hier_gen(); // sequential associative

  AddrTracer addr_tracer;
  if(trace_target_addr) { l3[0]->attach_monitor(&addr_tracer); addr_tracer.start(); }

  if(init_warm) warm_up(init_warm_prefetch);
  std::cout << "warm up finished" << std::endl;

  int evset_size = 1;

  while(evset_size <= 84140) {
    int succ_vbuf = 0;
    int succ = 0;
    uint64_t target = cm_get_random_uint64() & addr_mask;
    if(trace_target_addr) addr_tracer.set_target(target);
    auto llc = get_llc(target);
    for(int testN=0; testN<samples; testN++) {
      if(test_warm) warm_up(test_warm_prefetch);
      if(!same_target) {
        target = cm_get_random_uint64() & addr_mask;
        if(trace_target_addr) addr_tracer.set_target(target);
        llc = get_llc(target);
      } else if(!test_warm)
        light_warm(1ull << L3IW);
      if(prefetch_target)  mem_prefetch(target);
      else                 mem_access(target);
      if(use_cloc_addr) {
        std::set<uint64_t> evset;
        evset_gen(target, evset, evset_size);
        for(auto a:evset) {
          if(prefetch_cloc_addr)  mem_prefetch(a);
          else                    mem_access(a);
        }
      }
      if(use_rand_addr) {
        for(int i=0; i<evset_size; i++) {
          uint64_t a = cm_get_random_uint64() & addr_mask;
          if(prefetch_rand_addr)  mem_prefetch(a);
          else                    mem_access(a);
        }
      }
      if(!llc->hit(target)) succ++;
      if(!llc->hit(target, &dai, &ds, &dw, 1, false)) succ_vbuf++;
      if(prefetch_target)  mem_prefetch(target);
      else                 mem_access(target);
    }
    std::cout << "evset size = " << evset_size << ": " << succ << "/"
	    //<< succ_vbuf << "/"
	    << samples << std::endl;
    if(evset_size < 3) evset_size++;
    else evset_size *= 1.41422;
  }

  return 0;
}
