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
    ("prefetch-cloc-addr", po::value<bool>(&prefetch_cloc_addr), "whether to prefetch cloc addresses")
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
    std::cout << "test-warm = " << test_warm << std::endl;
    std::cout << "test-warm-prefetch = " << test_warm_prefetch << std::endl;
    std::cout << "same-target = " << same_target << std::endl;
    std::cout << "prefetch-target = " << prefetch_target << std::endl;
    std::cout << "prefetch-cloc-addr = " << prefetch_cloc_addr << std::endl;
    std::cout << "check-vbuf-hit = " << check_vbuf_hit << std::endl;
    std::cout << "trace-target-addr = " << trace_target_addr << std::endl;
  }

  auto l3 = cache_hier_gen(); // sequential associative

  SeqAssAddrTracer addr_tracer;
  if(trace_target_addr) { l3[0]->attach_monitor(&addr_tracer); addr_tracer.start(); }

  if(init_warm) warm_up(init_warm_prefetch);
  std::cout << "warm up finished" << std::endl;

  int evset_size = 1;

  while(evset_size <= (prefetch_cloc_addr ? 128 : 512)) {
    int succ_vbuf_pos = 0;
    int succ_pos = 0;
    int succ_vbuf_neg = 0;
    int succ_neg = 0;
    int pos_num = 0;
    int neg_num = 0;

    uint64_t target = cm_get_random_uint64() & addr_mask;
    if(trace_target_addr) addr_tracer.set_target(target);
    auto llc = get_llc(target);

    // positive test
    for(int testN=0; testN<samples; testN++) {
      if(test_warm) warm_up(test_warm_prefetch);
      if(!same_target) {
        target = cm_get_random_uint64() & addr_mask;
        if(trace_target_addr) addr_tracer.set_target(target);
        llc = get_llc(target);
        light_warm(1ull << L3IW);
      } else if(!test_warm) {
        while(llc->hit(target)) light_warm(1ull << L3IW);
      }

      std::set<uint64_t> evset;
      evset_gen(target, evset, evset_size);
      for(auto a:evset) {
        if(prefetch_cloc_addr)  mem_prefetch(a);
        else                    mem_access(a);
      }
      for(auto a:evset) {
        if(prefetch_cloc_addr)  mem_prefetch(a);
        else                    mem_access(a);
      }
      for(auto a:evset) {
        if(prefetch_cloc_addr)  mem_prefetch(a);
        else                    mem_access(a);
      }
      for(auto a:evset) {
        if(prefetch_cloc_addr)  mem_prefetch(a);
        else                    mem_access(a);
      }
      for(auto a:evset) {
        if(prefetch_cloc_addr)  mem_prefetch(a);
        else                    mem_access(a);
      }

      if(0 == (testN % 2)) { // pos test
        pos_num++;
        if(prefetch_target)  mem_prefetch(target);
        else                 mem_access(target);

        bool pos = false, vbuf_pos = false;
        for(auto a:evset) {
          if(!llc->hit(a)) pos = true;
          if(!llc->hit(a, &dai, &ds, &dw, 1, false)) vbuf_pos = true;
          if(prefetch_cloc_addr)  mem_prefetch(a);
          else                    mem_access(a);
        }

        if(pos) succ_pos++;
        if(vbuf_pos) succ_vbuf_pos++;
      } else { // neg test
        neg_num++;

        auto fake_target = cm_get_random_uint64() & addr_mask;
        if(prefetch_target)  mem_prefetch(fake_target);
        else                 mem_access(fake_target);        

        bool pos = false, vbuf_pos = false;
        for(auto a:evset) {
          if(!llc->hit(a)) pos = true;
          if(!llc->hit(a, &dai, &ds, &dw, 1, false)) vbuf_pos = true;
          if(prefetch_cloc_addr)  mem_prefetch(a);
          else                    mem_access(a);
        }

        if(!pos) succ_neg++;
        if(!vbuf_pos) succ_vbuf_neg++;
      }
    }
    auto TP = succ_pos; auto FP = neg_num - succ_neg; auto TN = succ_neg; auto FN = pos_num - succ_pos;
    auto TPv = succ_vbuf_pos; auto FPv = neg_num - succ_vbuf_neg; auto TNv = succ_vbuf_neg; auto FNv = pos_num - succ_vbuf_pos;
    std::cout << "evset size = " << evset_size << ": "
              << "[" << TP << "," << FN << "," << TN << "," << FP << "] F1=" << (2.0 * TP) / (2.0 * TP + FP + FN) << "; "
              //<< "[" << TPv << "," << FNv << "," << TNv << "," << FPv << "] F1=" << (2.0 * TPv) / (2.0 * TPv + FPv + FNv) << "; "
              << std::endl;
    if(evset_size < 6) evset_size++;
    else if(evset_size < 16) evset_size += 2;
    else evset_size *= 1.1892;
  }

  return 0;
}
