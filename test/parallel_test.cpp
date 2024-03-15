#include "test/config.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <deque>
#include <condition_variable>
#include <thread>

typedef Data64B data_type;

FILE *lock_log_fp;

std::vector<std::unique_ptr<std::mutex>> xact_queue_op_mutex_array, xact_queue_full_mutex_array, xact_queue_empty_mutex_array;
std::vector<std::unique_ptr<std::condition_variable>> xact_non_empty_notify_array, xact_non_full_notify_array;
std::vector<std::deque<cache_xact<data_type>>> xact_queue(NCore);
std::vector<uint64_t>    addr_pool; 
std::unordered_map<uint64_t, int> addr_map;
std::vector<DTContainer<NCore,data_type>* >  data_pool;   
std::vector<bool>        iflag;       // belong to instruction
int64_t gi;
CMHasher hasher(1203);
std::vector<CoreInterface *> core_data, core_inst;
SimpleMemoryModel<data_type,void,true>* mem;

extern void PlanA(bool flush_cache, bool remap);
extern void PlanB(bool flush_cache, bool remap);
extern void PlanC(bool flush_cache, bool remap);


void del(){
  for(auto a : data_pool) delete a;
}

int main() {
  // isLLC, uncache, EnMon
  auto l1d = cache_gen_l1<L1IW, L1WN, data_type, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, false, void, true>(NCoreM, "l1d");
  core_data = get_l1_core_interface(l1d);
  auto l1i = cache_gen_l1<L1IW, L1WN, data_type, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, true, void, true>(NCoreM, "l1i");
  core_inst = get_l1_core_interface(l1i);
  auto l2 = cache_gen_l2_inc<L2IW, L2WN, data_type, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, true, void, true>(1, "l2")[0];
  mem = new SimpleMemoryModel<data_type,void,true>("mem");
  for(int i=0; i<NCore; i++){
    l1i[i]->outer->connect(l2->inner, l2->inner->connect(l1i[i]->outer, true));
    l1d[i]->outer->connect(l2->inner, l2->inner->connect(l1d[i]->outer));
  } 
  l2->outer->connect(mem, mem->connect(l2->outer));
  lock_log_fp = fopen("dtrace", "w");
  close_log();

  PlanC(false, false);

  del();
  for(auto l : l1d){
    delete l;
  }
  for(auto l : l1i){
    delete l;
  }
  delete l2;

  return 0;
}