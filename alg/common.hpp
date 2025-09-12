#include "cache/memory.hpp"
#include "util/cache_type.hpp"
#include "cache/slicehash.hpp"
#include "util/query.hpp"
#include "rsa/monitor.hpp"
#include "rsa/cache.hpp"
#include "util/random.hpp"

#define NC 2

// 32K 8W, both I and D
#define L1IW 6
#define L1WN 8

// 256K, 4W, exclusive
#define L2IW 10
#define L2WN 4

// 2M per core
#define L3IW (11+3)
#define L3WN 16
#define L3VW 8
#define L3P 2
#define L3EW 8
#define L3RN 3

#define addr_mask 0x0000ffffffffffc0ull

static std::vector<CoreInterfaceBase *> core_data, core_inst;
static uint64_t wall_clock;              // a wall clock shared by all cores
static std::vector<uint64_t> core_cycle;
//static MonitorBase *remapper;
static SimpleAccMonitor *simple_pfc;
static EvictDistMonitor *tracer;

// get our own statically randomized LLC
using policy_l3 = MESIPolicy<false, true, policy_memory>;
using policy_l2 = ExclusiveMSIPolicy<false, false, policy_l3, false>;
using policy_l1d = MSIPolicy<true, false, policy_l2>;
using policy_l1i = MSIPolicy<true, true, policy_l2>;

void mem_access(uint64_t addr) {
  core_data[0]->read(addr, nullptr);
  wall_clock++;
}

void mem_prefetch(uint64_t addr) {
  core_data[0]->prefetch(addr, nullptr);
  wall_clock++;
}

void flush(uint64_t addr) {
  core_data[0]->flush(addr, nullptr);
  wall_clock++;
}

auto get_llc(uint64_t addr) {
  std::list<LocInfo> query_locs;
  core_data[0]->query_loc(addr, &query_locs);
  return query_locs.back().cache;
}

inline auto llc_gen_static_random(int size, const std::string& name_prefix) {
  static const bool EnMon = true;
  typedef void DT;
  typedef void DLY;
  typedef IndexRandom<L3IW,6> index_type;
  typedef ReplaceLRU<L3IW,L3WN,true,true,false> replace_type;
  typedef InnerCohPort<policy_l3, false> input_type;
  typedef OuterCohPortUncached<policy_l3, false> output_type;
  typedef MetadataMESIDirectory<48, 0, 6> metadata_type;
  typedef CacheNorm<L3IW, L3WN, metadata_type, DT, index_type, replace_type, DLY, EnMon, false> cache_base_type;
  typedef CoherentCacheNorm<cache_base_type, output_type, input_type> cache_type;
  return cache_generator<cache_type>(size, name_prefix);
}

inline auto llc_gen_skew(int size, const std::string& name_prefix) {
  static const bool EnMon = true;
  typedef void DT;
  typedef void DLY;
  typedef IndexSkewed<L3IW, 6, L3P> index_type;
  typedef ReplaceLRU<L3IW,L3WN / L3P,true,true,false> replace_type;
  typedef InnerCohPort<policy_l3, false> input_type;
  typedef OuterCohPortUncached<policy_l3, false> output_type;
  typedef MetadataMESIDirectory<48, 0, 6> metadata_type;
  typedef CacheSkewed<L3IW, L3WN / L3P, L3P, metadata_type, DT, index_type, replace_type, DLY, EnMon, false, 4> cache_base_type;
  typedef CoherentCacheNorm<cache_base_type, output_type, input_type> cache_type;
  return cache_generator<cache_type>(size, name_prefix);
}

// generate a sequental associative cache
inline auto llc_gen_seqass(int size, const std::string& name_prefix) {
  static const bool EnMon = true;
  typedef void DT;
  typedef void DLY;
  typedef IndexRandom<L3IW,6> index_type;
  typedef SeqAssReplaceLRU<L3IW,L3WN,true,true,false> replace_type;
  typedef SeqAssMetadata<MetadataMESIDirectory<48, 0, 6> > metadata_type;
  typedef SeqAssCache<L3IW, L3WN, metadata_type, DT, index_type, replace_type, DLY, EnMon, (L3WN * (1 << L3IW))> cache_base_type;
  typedef InnerCohPortT<SeqAssInnerCohPortUncached, policy_l3, false, metadata_type, cache_base_type> input_type;
  typedef OuterCohPortUncached<policy_l3, false> output_type;
  typedef CoherentCacheNorm<cache_base_type, output_type, input_type> cache_type;
  return cache_generator<cache_type>(size, name_prefix);
}

inline auto cache_hier_gen() {
  auto mem_pfc = new SimpleAccMonitor;

  auto l1d = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1d, false, void, true>(NC, "l1d");
  core_data = get_l1_core_interface(l1d);
  auto l1i = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1i, true, void, true>(NC, "l1i");
  core_inst = get_l1_core_interface(l1i);
  auto l2 = cache_gen_exc<L2IW, L2WN, void, MetadataBroadcastBase, ReplaceSRRIP, ExclusiveMSIPolicy, policy_l2, false, void, true>(NC, "l2");

  auto l3 = cache_gen_inc<L3IW, L3WN, void, MetadataDirectoryBase, ReplaceLRU, MESIPolicy, policy_l3, true, void, true>(1, "l3");
  tracer = new EvictDistMonitor(1, L3IW, L3WN*64, (1<<L3IW)*L3WN, &wall_clock, &core_cycle, mem_pfc); // period is set to ACC1

  //using remap_gen = ct::remap::types<L3IW, L3WN, 1, void, ReplaceLRU, policy_memory, void, true>;  //sp2021
  //auto l3 = remap_gen::cache_gen_remap(1, "l3");  // sp2021
  //tracer = new EvictDistMonitor(1, L3IW, L3WN*64, (1<<L3IW)*L3WN, &wall_clock, &core_cycle, mem_pfc); // period is set to ACC1

  //using remap_gen = ct::remap::types<L3IW, L3WN / L3P, L3P, void, ReplaceSRRIP, policy_memory, void, true>;  //ceaser-s
  //auto l3 = remap_gen::cache_gen_remap(1, "l3");  // ceaser-s and zsev
  //tracer = new EvictDistMonitor(2, L3IW, L3WN*64, (1<<L3IW)*L3WN, &wall_clock, &core_cycle, mem_pfc); // period is set to ACC1

  //using chameleon_gen = ct::chameleon::types<L3IW, L3WN / L3P, L3VW, L3P, void, ReplaceLRU, policy_memory, void, true>;  // chameleon cache
  //auto l3 = chameleon_gen::cache_gen_chameleon(1, "l3");  // chameleon cache
  //tracer = new EvictDistMonitor(2, L3IW, L3WN*64, (1<<L3IW)*L3WN, &wall_clock, &core_cycle, mem_pfc); // period is set to ACC1

  //using mirage_gen = ct::mirage::types<L3IW, L3WN / L3P, L3WN / 2 / L3P, L3P, 3, void, true, ReplaceRandom, ReplaceRandom, policy_memory, void, true, true>;  // mirage-50
  //auto l3 = mirage_gen::cache_gen_mirage(1, "l3");  // mirage
  //tracer = new EvictDistMonitor(2, L3IW, L3WN*64, (1<<L3IW)*L3WN, &wall_clock, &core_cycle, mem_pfc); // period is set to ACC1

  //using mirage_gen = ct::mirage::types<L3IW, L3WN / L3P, L3WN * 3 / 4 / L3P, L3P, 0, void, true, ReplaceRandom, ReplaceRandom, policy_memory, void, true, false>;  // mirage-75
  //auto l3 = mirage_gen::cache_gen_mirage(1, "l3");  // mirage
  //tracer = new EvictDistMonitor(2, L3IW, L3WN*64, (1<<L3IW)*L3WN, &wall_clock, &core_cycle, mem_pfc); // period is set to ACC1

  //auto l3 = llc_gen_seqass(1, "l3"); // sequential associative
  //tracer = new EvictDistMonitor(1, L3IW, L3WN*64, (1<<L3IW)*L3WN, &wall_clock, &core_cycle, mem_pfc); // period is set to ACC1

  auto mem = new SimpleMemoryModel<void,void,true>("mem");
  for(int i=0; i<NC; i++) {
    l1i[i]->outer->connect(l2[i]->inner);
    l1d[i]->outer->connect(l2[i]->inner);
    l2[i]->outer->connect(l3[0]->inner);
  }
  l3[0]->outer->connect(mem);

  //auto remapper = new ZSEVRemapper<L3IW>(1.0 / 32, (1ul << L3IW) * 4, (1ul << L3IW) * L3WN * 10, 10, true);  // zsev
  //auto remapper = new ACCRemapper((1<<L3IW)*L3WN*100);  // ceaser-s
  //auto remapper = new SeqAssRemapMonitor((1<<L3IW)/4, 32);
  //l3[0]->attach_monitor(remapper); remapper->start();

  mem->attach_monitor(mem_pfc);
  l3[0]->attach_monitor(tracer); tracer->start();

  simple_pfc = new SimpleAccMonitor;
  l3[0]->attach_monitor(simple_pfc); simple_pfc->start();

  return l3;
}

inline void evset_gen(uint64_t target, std::set<uint64_t> &evset, uint32_t size) {
  auto llc = get_llc(target);
  while(size) {
    uint64_t addr = cm_get_random_uint64() & addr_mask;
    if(llc->query_coloc(target, addr)) {
      evset.insert(addr);
      size--;
    }
  }
}

inline void warm_up(bool prefetch = false) {
  for(uint64_t i=0; i<2 * L3WN * (1ull << L3IW); i++) {
    uint64_t addr = cm_get_random_uint64() & addr_mask;
    if(prefetch) mem_prefetch(addr);
    else         mem_access(addr);
  }
}

inline void light_warm(uint64_t size) {
  for(uint64_t i=0; i<size; i++) {
    uint64_t addr = cm_get_random_uint64() & addr_mask;
    mem_access(addr);
  }
}

// for argument parsing
#include<boost/program_options.hpp>

int samples             = 200;
bool init_warm          = true;
bool init_warm_prefetch = false;
bool test_warm          = false;
bool test_warm_prefetch = false;
bool same_target        = false;
bool prefetch_target    = false;
bool use_cloc_addr      = true;
bool prefetch_cloc_addr = false;
bool use_rand_addr      = false;
bool prefetch_rand_addr = false;
bool check_vbuf_hit     = true;
bool trace_target_addr  = false;
uint32_t prime_round    = 1;
float candidate_ratio   = 1.05;
