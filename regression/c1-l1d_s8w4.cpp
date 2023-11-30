#include "cache/cache.hpp"
#include "cache/index.hpp"
#include "cache/replace.hpp"
#include "cache/coherence.hpp"
#include "cache/msi.hpp"
#include "cache/memory.hpp"
#include "util/regression.hpp"

#define AddrN 128
#define TestN 512

#define L1IW 3
#define L1WN 4
#define L1Toff 9

typedef Data64B data_type;
typedef MetadataMSIBroadcast<48, L1IW, L1Toff> l1_metadata_type;
typedef IndexNorm<L1IW,6> l1_indexer_type;
typedef ReplaceFIFO<L1IW,L1WN,1> l1_replacer_type;
typedef void l1_delay_type;
typedef CacheNorm<L1IW,L1WN,l1_metadata_type,data_type,l1_indexer_type,l1_replacer_type,void,true> l1_type;
typedef MSIPolicy<l1_metadata_type,1,1> l1_policy_type;

typedef OuterCohPortUncached memory_port_type;
typedef CoherentL1CacheNorm<l1_type,memory_port_type> l1_cache_type;
typedef SimpleMemoryModel<data_type,void,true> memory_type;

int main() {
  auto l1_policy = new l1_policy_type();
  auto l1d = new l1_cache_type(l1_policy, "l1d");
  auto core = static_cast<CoreInterface *>(l1d->inner);
  auto mem = new memory_type("mem");
  l1d->outer->connect(mem, mem->connect(l1d->outer));

  SimpleTracer tracer(true);
  l1d->attach_monitor(&tracer);
  mem->attach_monitor(&tracer);

  RegressionGen<1, false, AddrN, 0, data_type> tgen;

  for(int i=0; i<TestN; i++) {
    auto [addr, wdata, rw, nc, ic, flush] = tgen.gen();
    if(flush) {
      core->flush(addr, nullptr);
      core->write(addr, wdata, nullptr);
    } else if(rw) {
      core->write(addr, wdata, nullptr);
    } else {
      auto rdata = static_cast<const data_type *>(core->read(addr, nullptr));
      if(!tgen.check(addr, rdata)) return 1; // test failed!
    }
  }

  return 0;
}
