#include "cache/exclusive.hpp"
#include "cache/mesi.hpp"
#include "cache/index.hpp"
#include "cache/replace.hpp"
#include "cache/memory.hpp"
#include "util/random.hpp"
#include "util/regression.hpp"

#define PAddrN 128
#define SAddrN 64
#define NCore 2
#define TestN ((PAddrN + SAddrN) * NCore * 2)

#define L1IW 4
#define L1WN 4
#define L1Toff (L1IW + 6)

#define L2IW 5
#define L2WN 8
#define L2DW 4
#define L2Toff (L2IW + 6)

typedef Data64B data_type;
typedef MetadataMSIBroadcast<48, L1IW, L1Toff> l1_metadata_type;
typedef IndexNorm<L1IW,6> l1_indexer_type;
typedef ReplaceLRU<L1IW,L1WN,true> l1_replacer_type;
typedef CacheNorm<L1IW,L1WN,l1_metadata_type,data_type,l1_indexer_type,l1_replacer_type,void,true> l1_type;
typedef MSIPolicy<l1_metadata_type,true,false> l1_policy_type;
typedef CoherentL1CacheNorm<l1_type, OuterCohPortUncached> l1i_cache_type;
typedef CoherentL1CacheNorm<l1_type> l1d_cache_type;

typedef MetadataMESIDirectory<48, L2IW, L2Toff> l2_metadata_type;
typedef IndexNorm<L2IW,6> l2_indexer_type;
typedef ReplaceSRRIP<L2IW,L2WN,true> l2_replacer_type;
typedef ReplaceLRU<L2IW,L2DW,true> l2_ext_replacer_type;
typedef CacheNormExclusiveDirectory<L2IW,L2WN,L2DW,l2_metadata_type,data_type,l2_indexer_type,l2_replacer_type,l2_ext_replacer_type,void,true> l2_type;
typedef ExclusiveMESIPolicy<l2_metadata_type,true> l2_policy_type;

typedef OuterCohPortUncached memory_port_type;
typedef ExclusiveLLCDirectory<l2_type> l2_cache_type;
typedef SimpleMemoryModel<data_type,void,true> memory_type;

int main() {
  auto l1_policy = new l1_policy_type();
  std::vector<l1i_cache_type *> l1i(NCore);
  std::vector<l1d_cache_type *> l1d(NCore);
  std::vector<CoreInterface *>  core_inst(NCore), core_data(NCore);
  for(int i=0; i<NCore; i++) {
    l1i[i] = new l1i_cache_type(l1_policy, "l1i-" + std::to_string(i));
    l1d[i] = new l1d_cache_type(l1_policy, "l1d-" + std::to_string(i));
    core_inst[i] = static_cast<CoreInterface *>(l1i[i]->inner);
    core_data[i] = static_cast<CoreInterface *>(l1d[i]->inner);
  }
  auto l2_policy = new l2_policy_type();
  auto l2 = new l2_cache_type(l2_policy, "l2");
  auto mem = new memory_type("mem");
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

  RegressionGen<NCore, true, PAddrN, SAddrN, data_type> tgen;

  for(int i=0; i<TestN; i++) {
    auto [addr, wdata, rw, nc, ic, flush] = tgen.gen();
    if(flush) {
      if(flush > 1) for( auto ci:core_inst) ci->flush(addr, nullptr); // shared instruction, flush all cores
      else          core_inst[nc]->flush(addr, nullptr);
      core_data[nc]->write(addr, wdata, nullptr);
    } else if(rw) {
      core_data[nc]->write(addr, wdata, nullptr);
    } else {
      const data_type *rdata;
      if(ic) rdata = static_cast<const data_type *>(core_inst[nc]->read(addr, nullptr));
      else   rdata = static_cast<const data_type *>(core_data[nc]->read(addr, nullptr));
      if(!tgen.check(addr, rdata)) return 1; // test failed!
    }
  }

  return 0;
}
