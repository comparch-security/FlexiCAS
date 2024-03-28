#include "test/config.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <deque>
#include <condition_variable>
#include <thread>


FILE *lock_log_fp;

std::vector<std::unique_ptr<std::mutex>> xact_queue_op_mutex_array, xact_queue_full_mutex_array, xact_queue_empty_mutex_array;
std::vector<std::unique_ptr<std::condition_variable>> xact_non_empty_notify_array, xact_non_full_notify_array;
std::vector<std::deque<cache_xact>> xact_queue(NCore);
std::vector<uint64_t>    addr_pool; 
std::unordered_map<uint64_t, int> addr_map;
std::vector<DTContainer<NCore,data_type>* >  data_pool;   
std::vector<bool>        iflag;       // belong to instruction
int64_t gi;
CMHasher hasher(1203);
std::vector<CoreInterface *> core_data, core_inst;
SimpleMemoryModel<data_type,void,false>* mem;

extern void PlanA(bool flush_cache, bool remap);
extern void PlanB(bool flush_cache, bool remap);
extern void PlanC(bool flush_cache, bool remap);

// #define THREE_LEVEL_CACHE

void del(){
  for(auto a : data_pool) delete a;
}

int main() {
  // isLLC, uncache, EnMon
#ifdef THREE_LEVEL_CACHE
  auto l1d = cache_gen_l1<L1IW, L1WN, data_type, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, false, void, false>(NCoreM, "l1d");
  core_data = get_l1_core_interface(l1d);
  auto l1i = cache_gen_l1<L1IW, L1WN, data_type, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, true, void, false>(NCoreM, "l1i");
  core_inst = get_l1_core_interface(l1i);
  auto l2 = cache_gen_l2_inc<L2IW, L2WN, data_type, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, void, false>(1, "l2")[0];
  auto llc = cache_gen_llc_inc<L3IW, L3WN, data_type, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, void, false>(1, "llc")[0];
  mem = new SimpleMemoryModel<data_type,void,false>("mem");
  for(int i=0; i<NCore; i++){
    l1i[i]->outer->connect(l2->inner, l2->inner->connect(l1i[i]->outer, true));
    l1d[i]->outer->connect(l2->inner, l2->inner->connect(l1d[i]->outer));
  } 
  l2->outer->connect(llc->inner, llc->inner->connect(l2->outer));
  llc->outer->connect(mem, mem->connect(llc->outer));
#else
  auto l1d = cache_gen_l1<L1IW, L1WN, data_type, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, false, void, false>(NCoreM, "l1d");
  core_data = get_l1_core_interface(l1d);
  auto l1i = cache_gen_l1<L1IW, L1WN, data_type, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, true, void, false>(NCoreM, "l1i");
  core_inst = get_l1_core_interface(l1i);
  auto l2 = cache_gen_l2_inc<L2IW, L2WN, data_type, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, true, void, false>(1, "l2")[0];
  mem = new SimpleMemoryModel<data_type,void,false>("mem");
  for(int i=0; i<NCore; i++){
    l1i[i]->outer->connect(l2->inner, l2->inner->connect(l1i[i]->outer, true));
    l1d[i]->outer->connect(l2->inner, l2->inner->connect(l1d[i]->outer));
  } 
  l2->outer->connect(mem, mem->connect(l2->outer));
#endif

  lock_log_fp = fopen("dtrace", "w");
  close_log();

  PlanB(true, true);

  std::cout << "L1 IW:" << L1IW << " , L1 WN:" << L1WN << std::endl; 
  std::cout << "L2 IW:" << L2IW << " , L2 WN:" << L2WN << std::endl;
#ifdef THREE_LEVEL_CACHE 
  std::cout << "L3 IW:" << L3IW << " , L3 WN:" << L3WN << std::endl;
#endif 

#ifdef USE_DATA
  std::cout << "using data" << std::endl;
#else
  std::cout << "only meta" << std::endl;
#endif 


  del();
  for(auto l : l1d){
    delete l;
  }
  for(auto l : l1i){
    delete l;
  }
  delete l2;
  delete mem;
#ifdef THREE_LEVEL_CACHE 
  delete llc;
#endif 

  return 0;
}