#include "cache/cache.hpp"
#include "cache/index.hpp"
#include "cache/replace.hpp"
#include "cache/coherence.hpp"
#include "cache/msi.hpp"
#include "cache/memory.hpp"
#include "util/random.hpp"
#include "util/regression.hpp"

#define AddrN 160
#define TestN 1024

#define L1IW 3
#define L1WN 4
#define L1Toff 9

#define L2IW 4
#define L2WN 8
#define L2Toff 10

typedef Data64B data_type;
typedef MetadataMSIBroadcast<48, L1IW, L1Toff> l1_metadata_type;
typedef IndexNorm<L1IW,6> l1_indexer_type;
typedef ReplaceLRU<L1IW,L1WN,1> l1_replacer_type;
typedef CacheNorm<L1IW,L1WN,l1_metadata_type,data_type,l1_indexer_type,l1_replacer_type,void,true> l1_type;
typedef MSIPolicy<l1_metadata_type,true,false> l1_policy_type;
typedef CoherentL1CacheNorm<l1_type, OuterCohPortUncached> l1i_cache_type;
typedef CoherentL1CacheNorm<l1_type> l1d_cache_type;

typedef MetadataMSIBroadcast<48, L2IW, L2Toff> l2_metadata_type;
typedef IndexNorm<L2IW,6> l2_indexer_type;
typedef ReplaceSRRIP<L2IW,L2WN,1> l2_replacer_type;
typedef CacheNorm<L2IW,L2WN,l2_metadata_type,data_type,l2_indexer_type,l2_replacer_type,void,true> l2_type;
typedef MSIPolicy<l2_metadata_type,false,true> l2_policy_type;

typedef OuterCohPortUncached memory_port_type;
typedef CoherentCacheNorm<l2_type,memory_port_type> l2_cache_type;
typedef SimpleMemoryModel<data_type,void,true> memory_type;

int main() {
  auto l1_policy = new l1_policy_type();
  auto l1i = new l1i_cache_type(l1_policy, "l1i");
  auto l1d = new l1d_cache_type(l1_policy, "l1d");
  auto core_inst = static_cast<CoreInterface *>(l1i->inner);
  auto core_data = static_cast<CoreInterface *>(l1d->inner);
  auto l2_policy = new l2_policy_type();
  auto l2 = new l2_cache_type(l2_policy, "l2");
  auto mem = new memory_type("mem");

  l1i->outer->connect(l2->inner, l2->inner->connect(l1i->outer, true));
  l1d->outer->connect(l2->inner, l2->inner->connect(l1d->outer));
  l2->outer->connect(mem, mem->connect(l2->outer));

  SimpleTracer tracer(true);
  l1i->attach_monitor(&tracer);
  l1d->attach_monitor(&tracer);
  l2->attach_monitor(&tracer);
  mem->attach_monitor(&tracer);

  RegressionGen<1, true, AddrN, 0, data_type> tgen;

  for(int i=0; i<TestN; i++) {
    auto [addr, wdata, rw, nc, ic, flush] = tgen.gen();
    if(flush) {
      core_inst->flush(addr, nullptr);
      core_data->write(addr, wdata, nullptr);
    } else if(rw) {
      core_data->write(addr, wdata, nullptr);
    } else {
      const data_type *rdata;
      if(ic) rdata = static_cast<const data_type *>(core_inst->read(addr, nullptr));
      else   rdata = static_cast<const data_type *>(core_data->read(addr, nullptr));
      if(!tgen.check(addr, rdata)) return 1; // test failed!
    }
  }

  return 0;
}
