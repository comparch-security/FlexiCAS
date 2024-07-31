#include "cache/memory.hpp"
#include "util/cache_type.hpp"
#include "util/regression.hpp"

#define AddrN 128
#define TestN 512

int main() {
  auto cache = cache_gen_l1<4, 4, Data64B, MetadataBroadcastBase, ReplaceFIFO, MSIPolicy, true, true, void, true>(1, "l1d");
  auto l1d = cache[0];
  auto core = get_l1_core_interface(cache);
  auto mem = new SimpleMemoryModel<Data64B,void,true>("mem");
  l1d->outer->connect(mem, mem->connect(l1d->outer));

  SimpleTracer tracer(true);
  l1d->attach_monitor(&tracer);
  mem->attach_monitor(&tracer);
  tracer.start();

  RegressionGen<1, false, false, AddrN, 0, Data64B> tgen;
  auto rv = tgen.run(TestN, core, core);

  tracer.stop();
  delete_caches(cache);
  delete mem;
  return rv;
}
