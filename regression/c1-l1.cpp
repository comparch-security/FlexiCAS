#include "cache/memory.hpp"
#include "util/cache_type.hpp"
#include "util/regression.hpp"

#define AddrN 128
#define TestN 512

int main() {
  auto cache = cache_gen_l1<4, 4, Data64B, MetadataBroadcastBase, ReplaceFIFO, MSIPolicy, true, true, void, true>(1, "l1d");
  auto l1d = cache[0];
  auto core = get_l1_core_interface(cache)[0];
  auto mem = new SimpleMemoryModel<Data64B,void,true>("mem");
  l1d->outer->connect(mem, mem->connect(l1d->outer));

  SimpleTracer tracer(true);
  l1d->attach_monitor(&tracer);
  mem->attach_monitor(&tracer);

  RegressionGen<1, false, AddrN, 0, Data64B> tgen;

  for(int i=0; i<TestN; i++) {
    auto [addr, wdata, rw, nc, ic, flush] = tgen.gen();
    if(flush) {
      core->flush(addr, nullptr);
      core->write(addr, wdata, nullptr);
    } else if(rw) {
      core->write(addr, wdata, nullptr);
    } else {
      if(!tgen.check(addr, core->read(addr, nullptr))) return 1; // test failed!
    }
  }

  return 0;
}
