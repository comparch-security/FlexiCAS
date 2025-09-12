#include "alg/common.hpp"
#include "util/statistics.hpp"

uint32_t dai, ds, dw;
int evset_size = 1;

int main(int argc, char **argv) {
  check_vbuf_hit = false;

  namespace po = boost::program_options;
  po::options_description desc("options");
  desc.add_options()
    ("help", "describe arguments")
    ("print", "print arguments")
    ("samples", po::value<int>(&samples), "number of sample for each test")
    ("init-warm", po::value<bool>(&init_warm), "whether to warmup initially")
    ("evset-size", po::value<int>(&evset_size), "evset size")
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
    std::cout << "evset-size = " << evset_size << std::endl;
    std::cout << "check-vbuf-hit = " << check_vbuf_hit << std::endl;
    std::cout << "trace-target-addr = " << trace_target_addr << std::endl;
  }

  auto l3 = cache_hier_gen(); // sequential associative

  SeqAssAddrTracer addr_tracer;
  if(trace_target_addr) { l3[0]->attach_monitor(&addr_tracer); addr_tracer.start(); }

  if(init_warm) warm_up(init_warm_prefetch);
  std::cout << "warm up finished" << std::endl;

  int random_size = 0;

  while(random_size < (1 << L3IW)) {
    int succ_vbuf_pos = 0;
    int succ_pos = 0;
    int succ_vbuf_neg = 0;
    int succ_neg = 0;
    int pos_num = 0;
    int neg_num = 0;
    int premuture_num = 0;

    uint64_t target = cm_get_random_uint64() & addr_mask;
    if(trace_target_addr) addr_tracer.set_target(target);
    auto llc = get_llc(target);

    // positive test
    for(int testN=0; testN<samples; testN++) {
      std::set<uint64_t> evset;
      evset_gen(target, evset, evset_size);
      for(auto a:evset) mem_prefetch(a);
      for(auto a:evset) mem_prefetch(a);
      for(auto a:evset) mem_prefetch(a);
      for(auto a:evset) mem_prefetch(a);
      for(auto a:evset) mem_prefetch(a);

      for(int i=0; i<random_size; i++)
        mem_access(cm_get_random_uint64() & addr_mask);

      // check whether the evset intact
      bool premuture = false;
      for(auto a:evset) {
        if(!llc->hit(a)) { premuture = true; break; }
      }

      if(premuture) {
        premuture_num++;
        continue;
      }

      pos_num++;
      mem_access(target);

      bool pos = false, vbuf_pos = false;
      for(auto a:evset) {
        if(!llc->hit(a)) pos = true;
        if(!llc->hit(a, &dai, &ds, &dw, 1, false)) vbuf_pos = true;
        mem_prefetch(a);
      }

      if(pos) succ_pos++;
      if(vbuf_pos) succ_vbuf_pos++;
    }
    std::cout << "random size = " << random_size << ": " << succ_pos << "," << premuture_num << std::endl;
    if(random_size < 6) random_size++;
    else if(random_size < 16) random_size += 2;
    else random_size *= 1.1892;
  }

  return 0;
}
