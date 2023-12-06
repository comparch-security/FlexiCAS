#include "cache/memory.hpp"
#include "util/cache_type.hpp"
#include "util/regression.hpp"

#define PAddrN 128
#define SAddrN 64
#define NCore 2
#define TestN ((PAddrN + SAddrN) * NCore * 2)

#define L1IW 4
#define L1WN 4

#define L2IW 5
#define L2WN 8
#define L2Toff (L2IW + 6)

int main() {
  auto l1d = cache_gen_l1<L1IW, L1WN, Data64B, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, false, void, true>(NCore, "l1d");
  auto core_data = get_l1_core_interface(l1d);
  auto l1i = cache_gen_l1<L1IW, L1WN, Data64B, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, true, void, true>(NCore, "l1i");
  auto core_inst = get_l1_core_interface(l1i);
  auto l2 = cache_gen_l2_inc<L2IW, L2WN, Data64B, MetadataBroadcastBase, ReplaceSRRIP, MSIPolicy, true, void, true>(1, "l2")[0];
  auto mem = new SimpleMemoryModel<Data64B,void,true>("mem");
  SimpleTracer tracer(true);

  for(int i=0; i<NCore; i++) {
    l1i[i]->outer->connect(l2->inner, l2->inner->connect(l1i[i]->outer, true));
    l1d[i]->outer->connect(l2->inner, l2->inner->connect(l1d[i]->outer));
    l1i[i]->attach_monitor(&tracer);
    l1d[i]->attach_monitor(&tracer);
  }
  l2->outer->connect(mem, mem->connect(l2->outer));

  l2->attach_monitor(&tracer);
  mem->attach_monitor(&tracer);

  RegressionGen<NCore, true, PAddrN, SAddrN, Data64B> tgen;

  for(int i=0; i<TestN; i++) {
    auto [addr, wdata, rw, nc, ic, flush] = tgen.gen();
    if(flush) {
      if(flush > 1) for( auto ci:core_inst) ci->flush(addr, nullptr); // shared instruction, flush all cores
      else          core_inst[nc]->flush(addr, nullptr);
      core_data[nc]->write(addr, wdata, nullptr);
    } else if(rw) {
      core_data[nc]->write(addr, wdata, nullptr);
    } else {
      auto rdata = ic ? core_inst[nc]->read(addr, nullptr) : core_data[nc]->read(addr, nullptr);
      if(!tgen.check(addr, rdata)) return 1; // test failed!
    }
  }

  return 0;
}
