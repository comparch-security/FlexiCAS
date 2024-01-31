#include "cache/memory.hpp"
#include "util/cache_type.hpp"
#include "util/common.hpp"
#include "util/log.hpp"
#include <cstdio>
#include <iostream>

#define NCore 4

std::mutex Mlock;
FILE *lock_log_fp;
typedef Data64B data_type;

int threads_num = 4;
std::vector<CoreInterface *>core;

void threadRW(int operation){
  database.insert_id(get_thread_id);
  uint64_t delay = 0;
  uint64_t src1 = get_thread_id & 0xFFFFC0;
  uint64_t src2 = cm_get_random_uint64() & 0xFFFFC0;
  uint64_t addr = src1 + src2;
  data_type thread_data;
  uint64_t dd[8];

  switch (operation) {
    case 0:          // read
      core[database.get_id(get_thread_id)]->read(addr, &delay);
      break;
    case 1:         // write
      database.insert_addr(addr, dd, database.get_sync());
      thread_data.write(dd);
      core[database.get_id(get_thread_id)]->write(addr, &thread_data, &delay);
      break;
    case 2:         // flush
      core[database.get_id(get_thread_id)]->flush(addr, &delay);
      break;
  }
}

int main() {
  // isLLC, uncache, EnMon
  auto l1d = cache_gen_l1<2, 2, Data64B, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, false, void, false>(4, "l1d");
  core = get_l1_core_interface(l1d);
  auto l2 = cache_gen_l2_inc<3, 4, Data64B, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, true, void, false>(1, "l2")[0];
  auto mem = new SimpleMemoryModel<Data64B,void,true>("mem");

  SimpleTracer tracer(true);
  for(int i=0; i<NCore; i++){
    l1d[i]->outer->connect(l2->inner, l2->inner->connect(l1d[i]->outer));
    // l1d[i]->attach_monitor(&tracer);
  }
  l2->outer->connect(mem, mem->connect(l2->outer));
  // mem->attach_monitor(&tracer);

  lock_log_fp = fopen("dtrace", "w");

  for(int i=0; i < 12800/threads_num; i++){

    std::thread threadA(threadRW, 1);
    std::thread threadB(threadRW, 1);
    std::thread threadC(threadRW, 1);
    std::thread threadD(threadRW, 1);

    threadA.join();
    threadB.join();
    threadC.join();
    threadD.join();

    database.clear();
    database.add_sync();

    printf("sync %d\n", i);
    lock_log_write("sync %d\n", i);
  }

  close_log();
  uint64_t delay;
  for(auto addr : database.addr_set){
    uint64_t res = 0;
    auto data = core[0]->read(addr, &delay);
    for(int i = 0; i < 8; i++) {
      res += data->read(i);
    }
    bool flag = false;
    for(auto r : database.addr_map[addr].l){
      if(res == r){
        // printf("addr 0x%lx pass\n", addr);
        flag = true;
        break;
      }
    }
    if(!flag){
      printf("addr is 0x%lx, read res is 0x%lx\n", addr, res);
      assert(0);
    }
  }
  printf("check succ\n");

  // std::vector<data_type *> data(4);
  // for(int i=0; i<2; i++) data[i] = new data_type();
  // uint64_t da[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  // uint64_t db[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  // data[0]->write(da); data[1]->write(db);
  // for(uint64_t i = 1; i <= 10000; i+=1){
  //   uint64_t addr = i*64;
  //   uint64_t delay = 0;
  //   std::cout << i << std::endl;
  //   core[0]->write(addr, data[0], nullptr);
  //   lock_log_write("sync %ld\n", i);
  // }
    
  // for(uint64_t i = 10000; i>=1 ; i-=1){
  //   uint64_t addr = i*64;
  //   const CMDataBase* read_data = core[0]->read(addr, nullptr);
  //   std::cout << i << std::endl;
  //   int result = 0;
  //   for(int j = 0; j < 8; j++){
  //     result += read_data->read(j);
  //   }
  //   assert(result==28);
  // }  

  return 0;
}
