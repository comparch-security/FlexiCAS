#include "cache/memory.hpp"
#include "cache/metadata.hpp"
#include "util/cache_type.hpp"
#include "util/common.hpp"
#include "util/log.hpp"
#include "util/parallel_regression.hpp"
#include "util/random.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <queue>
#include <condition_variable>
#include <thread>

#define NCore 4
#define AddrN 128000

#define NCoreM 4

#define L1IW 4
#define L1WN 4

#define L2IW 5
#define L2WN 8

#define XACT_QUEUE_HIGH    100
#define XACT_QUEUE_LOW     10

#define CACHE_OP_READ        0
#define CACHE_OP_WRITE       1
#define CACHE_OP_FLUSH       2

typedef Data64B data_type;

FILE *lock_log_fp;

std::vector<std::unique_ptr<std::mutex>> xact_queue_op_mutex_array, xact_queue_full_mutex_array, xact_queue_empty_mutex_array;
std::vector<std::unique_ptr<std::condition_variable>> xact_non_empty_notify_array, xact_non_full_notify_array;
std::vector<std::queue<cache_xact<data_type>>> xact_queue(NCore);
std::vector<uint64_t>    addr_pool; 
std::unordered_map<uint64_t, int> addr_map;
std::vector<DTContainer<NCore,data_type>* >  data_pool;   
std::vector<bool>        iflag;       // belong to instruction
int64_t gi;
CMHasher hasher(1203);
std::vector<CoreInterface *> core_data, core_inst;

void init(bool EnIC){
  gi = 71;
  xact_queue_empty_mutex_array.resize(NCore);
  xact_queue_full_mutex_array.resize(NCore);
  xact_queue_op_mutex_array.resize(NCore);
  xact_non_full_notify_array.resize(NCore);
  xact_non_empty_notify_array.resize(NCore);
  for(int i = 0; i < NCore; i++){
    xact_queue_empty_mutex_array[i] = std::make_unique<std::mutex>();
    xact_queue_full_mutex_array[i] = std::make_unique<std::mutex>();
    xact_queue_op_mutex_array[i] = std::make_unique<std::mutex>();
    xact_non_full_notify_array[i] = std::make_unique<std::condition_variable>();
    xact_non_empty_notify_array[i] = std::make_unique<std::condition_variable>();
  }
  addr_pool.resize(AddrN);
  iflag.resize(AddrN);
  data_pool.resize(AddrN);
  for(int i=0; i<AddrN; i++){
    auto addr = hasher(gi++) & addr_mask;
    while(addr_map.count(addr)) addr = hasher(gi++) & addr_mask;
    addr_pool[i] = addr;
    addr_map[addr] = i;
    data_pool[i] = new DTContainer<NCore, data_type>();
    data_pool[i]->init(addr);
    if(EnIC)
      iflag[i] = (0 == hasher(gi++) & 0x111); // 12.5% is instruction
    else
      iflag[i] = false;
  }
}
// inline cache_xact<Data64B> gen_xact(int core){
//   cache_xact<Data64B> act;
//   act.core = core;
//   unsigned int index = hasher(tgi[core]++) % AddrN;
//   uint64_t addr = addr_pool[index];
//   auto d = data_pool[index];
//   Data64B dt;
//   int ran_num = hasher(tgi[core]++);
//   bool rw = (0 == (ran_num & 0x11)); // 25% write
//   bool flush = (0 == (ran_num & 0x17)) ? 1 : 0; // 25% of write is flush
//   bool is_inst = iflag[index];
//   bool ic;

//   if(is_inst && rw){
//     ic = false;
//     // flush = 1;
//   } else{
//     ic = is_inst ;
//     if(is_inst) flush = 0; // read instruction
//     if(flush)   rw = 0;
//   }
//   if(rw){
//     dt.write(0, hasher(tgi[core]++), 0xffffffffffffffffull);
//     d->write(&dt);
//     act.data.copy(&dt);
//   }
//   act.addr = addr;
//   act.ic = ic;
//   act.op_t = flush ? CACHE_OP_FLUSH : (rw ? CACHE_OP_WRITE : CACHE_OP_READ);
//   return act;
// }

void xact_queue_add(int core, std::atomic<int>& counter) {
  // std::unique_lock queue_full_lock(*(xact_queue_full_mutex_array[core]), std::defer_lock);
  int num = 0;
  cache_xact<data_type> act;
  act.core = core;
  CMHasher thasher(999+core);
  uint32_t tgi = cm_get_random_uint32(); 
  while(num < (AddrN/NCore)){
    // xact_queue_op_mutex_array[core]->lock();
    // if(xact_queue[core].size() >= XACT_QUEUE_HIGH){
    //   xact_queue_op_mutex_array[core]->unlock();
    //   xact_non_full_notify_array[core]->wait(queue_full_lock);
    // }else{
    //   xact_queue[core].push_back(xact);
    //   // xact_non_empty_notify_array[core]->notify_all();
    //   xact_queue_op_mutex_array[core]->unlock();
    //   num++;
    // }
    // xact_queue[core].push_back(xact);
    // xact_non_empty_notify_array[core]->notify_all();
    // xact_queue_op_mutex_array[core]->unlock();
    unsigned int index = thasher(tgi++) % AddrN;
    uint64_t addr = addr_pool[index];
    auto d = data_pool[index];
    data_type dt;
    int ran_num = thasher(tgi++);
    bool rw = (0 == (ran_num & 0x11)); // 25% write
    bool flush = (0 == (ran_num & 0x17)) ? 1 : 0; // 25% of write is flush
    bool is_inst = iflag[index];
    bool ic;

    if(is_inst && rw){
      ic = false;
      // flush = 1;
    } else{
      ic = is_inst ;
      if(is_inst) flush = 0; // read instruction
      if(flush)   rw = 0; 
    }
    if (!C_VOID(data_type) && rw){
      dt.write(0, num, 0xffffffffffffffffull);
      d->write(&dt);
      act.data.copy(&dt);
    }
    act.addr = addr;
    act.ic = ic;
    act.op_t = flush ? CACHE_OP_FLUSH : (rw ? CACHE_OP_WRITE : CACHE_OP_READ);
    xact_queue[core].push(act);
    num++;
  }
  counter.fetch_add(1, std::memory_order_relaxed);
}

void cache_server(int core, std::atomic<int>& counter){
  // std::unique_lock queue_empty_lock(*(xact_queue_empty_mutex_array[core]), std::defer_lock);
  database.insert_id(get_thread_id);
  int num = 0;
  cache_xact<data_type> xact;
  while(num < (AddrN/NCore)){
    // xact_queue_op_mutex_array[core]->lock();
    bool busy = !(xact_queue[core].empty());
    if(busy){
      xact = xact_queue[core].front();
      xact_queue[core].pop();
      num++;
      // if(xact_queue.size() <= XACT_QUEUE_LOW)
      //   xact_non_full_notify_array[core]->notify_all();
    }
    // xact_queue_op_mutex_array[core]->unlock(); 
    if(busy){
      switch (xact.op_t) {
      case CACHE_OP_READ:
        if(xact.ic)
          core_inst[core]->read(xact.addr, nullptr);
        else
          core_data[core]->read(xact.addr, nullptr);
        break;
      case CACHE_OP_WRITE:
        core_data[core]->write(xact.addr, &(xact.data), nullptr);
        break;
      case CACHE_OP_FLUSH:
        core_data[core]->flush(xact.addr, nullptr);
        break;
      default:
        assert(0 == "unknown op type");
      }
    }
  }
  counter.fetch_add(1, std::memory_order_relaxed);
}


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
  auto mem = new SimpleMemoryModel<data_type,void,true>("mem");
  for(int i=0; i<NCore; i++){
    l1i[i]->outer->connect(l2->inner, l2->inner->connect(l1i[i]->outer, true));
    l1d[i]->outer->connect(l2->inner, l2->inner->connect(l1d[i]->outer));
  } 
  l2->outer->connect(mem, mem->connect(l2->outer));
  lock_log_fp = fopen("dtrace", "w");
  close_log();
  init(true);

  std::atomic<int> counter(0);

  std::vector<std::thread> server_thread;
  std::vector<std::thread> add_thread;
  auto startTime = std::chrono::steady_clock::now();
  for(int i = 0; i < NCore; i++){
    add_thread.emplace_back(xact_queue_add, i, std::ref(counter));
  }

  while(counter.load(std::memory_order_relaxed) < 1) {}

  auto endTime = std::chrono::steady_clock::now();
  auto durationA = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
  std::cout << "thread num  : " << NCore << std::endl;
  std::cout << "add addr : " << AddrN << std::endl;
  std::cout << "cost time   : " << durationA.count() << "ms " << std::endl;
  std::cout << std::endl;
  for(auto & t : add_thread){
    t.join();
  }
  std::atomic<int> server_counter(0);
  
  auto start = std::chrono::high_resolution_clock::now();
  for(int i = 0; i < NCore; i++){
    server_thread.emplace_back(cache_server, i, std::ref(server_counter));
  }
  while(server_counter.load(std::memory_order_relaxed) < 1) {}

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> durationB = end - start;

  std::cout << "thread num  : " << NCore << std::endl;
  std::cout << "handle addr : " << AddrN << std::endl;
  std::cout << "cost time   : " << durationB.count() << "s" << std::endl;

  for(auto &s : server_thread){
    s.join();
  }

  del();

  return 0;
}