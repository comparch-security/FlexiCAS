#include "cache/memory.hpp"
#include "util/cache_type.hpp"
#include "util/common.hpp"
#include "util/log.hpp"
#include <cstdio>
#include <iostream>

#define NCore 2
#define addr_num 1280000

std::mutex mtx;
std::vector<uint64_t> addr_buffer(NCore);
std::condition_variable cv_addr;
uint64_t buffer_record = 0; 
FILE *lock_log_fp;
typedef Data64B data_type;

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

void threadfixAddr(int operation, uint64_t addr){
  database.insert_id(get_thread_id);
  uint64_t delay = 0;
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

void producer_thread(){
  std::unique_lock lk(mtx, std::defer_lock);

  for(int i = 0; i < addr_num/NCore; i++){
    lk.lock();
    // printf("sync %d\n", i);
    // lock_log_write("sync %d\n", i);
    cv_addr.wait(lk, [] { return buffer_record == 0;});

    for(int i = 0; i < NCore; i++){
      addr_buffer[i] = cm_get_random_uint64() & 0xFFFFC0;
      buffer_record |= (1 << i);
    }
    database.add_sync();

    lk.unlock();
    cv_addr.notify_all();
  }

  lk.lock();
  cv_addr.wait(lk, [] {return buffer_record == 0;});
  for(int i = 0; i <= NCore; i++){
    buffer_record |= (1<<i);
  }
  lk.unlock();
  cv_addr.notify_all();

}

void worker_thread(int id){
  std::unique_lock lk(mtx, std::defer_lock);
  database.insert_id(get_thread_id);
  int operation;
  while(true){
    lk.lock();
    cv_addr.wait(lk, [id]{ return (buffer_record & (1 << id)) == (1 << id);});
    lk.unlock();

    if((buffer_record & (1 << NCore)) == (1 << NCore)){
      break;
    }

    uint64_t delay = 0;
    uint64_t addr = addr_buffer[id];
    data_type thread_data;
    uint64_t dd[8];
    operation = cm_get_random_uint32()%3;
    switch(operation){
    case 0:          // read
      core[id]->read(addr, &delay);
      break;
    case 1:         // write
      database.insert_addr(addr, dd, database.get_sync());
      thread_data.write(dd);
      core[id]->write(addr, &thread_data, &delay);
      break;
    case 2:         // flush
      core[id]->flush(addr, &delay);
      break;
    }

    lk.lock();
    buffer_record &= ~(1 << id);
    lk.unlock();

    cv_addr.notify_all();
  }
}

int main() {

  // isLLC, uncache, EnMon
  auto l1d = cache_gen_l1<6, 8, Data64B, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, false, false, void, false>(NCore, "l1d");
  core = get_l1_core_interface(l1d);
  auto l2 = cache_gen_l2_inc<10, 16, Data64B, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, true, void, false>(1, "l2")[0];
  // auto llc = cache_gen_llc_inc<4, 4, Data64B, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, void, false>(1, "llc")[0];
  auto mem = new SimpleMemoryModel<Data64B,void,true>("mem");

  SimpleTracer tracer(true);
  for(int i=0; i<NCore; i++){
    l1d[i]->outer->connect(l2->inner, l2->inner->connect(l1d[i]->outer));
    // l1d[i]->attach_monitor(&tracer);
    // l2[i]->outer->connect(llc->inner, llc->inner->connect(l2[i]->outer));
  }
  l2->outer->connect(mem, mem->connect(l2->outer));
  llc->outer->connect(mem, mem->connect(llc->outer));
  // mem->attach_monitor(&tracer);

  lock_log_fp = fopen("dtrace", "w");
  close_log();

  // // for time test 
  // auto start = std::chrono::high_resolution_clock::now();
  // std::thread producer(producer_thread);
  // std::vector<std::thread> thread_array(NCore);
  // for(int i = 0; i < NCore; i++){
  //   thread_array[i] = std::thread(worker_thread, i);
  // }

  // producer.join();
  // for(int i = 0; i < NCore; i++){
  //   thread_array[i].join();
  // }
  // auto end = std::chrono::high_resolution_clock::now();
  // std::chrono::duration<double> duration = end - start;

  // std::cout << "thread num  : " << NCore << std::endl;
  // std::cout << "access addr : " << addr_num << std::endl;
  // std::cout << "cost time   : " << duration.count() << " s " << std::endl;

  // for(int i=0; i < addr_num/NCore; i++){
  //   uint64_t addr1 = cm_get_random_uint64() & 0xFFFFC0;

  //   std::thread threadA(threadfixAddr, cm_get_random_uint32()%3, addr1);
  //   std::thread threadB(threadfixAddr, cm_get_random_uint32()%3, addr1);
  //   std::thread threadC(threadRW, cm_get_random_uint32()%3);
  //   std::thread threadD(threadRW, cm_get_random_uint32()%3);

  //   threadA.join();
  //   threadB.join();
  //   threadC.join();
  //   threadD.join();

  //   database.clear();
  //   database.add_sync();

  //   printf("sync %d\n", i);
  //   lock_log_write("sync %d\n", i);
  // }

  // close_log();
  // uint64_t delay;
  // for(auto addr : database.addr_set){
  //   uint64_t res = 0;
  //   auto data = core[0]->read(addr, &delay);
  //   for(int i = 0; i < 8; i++) {
  //     res += data->read(i);
  //   }
  //   bool flag = false;
  //   for(auto r : database.addr_map[addr].l){
  //     if(res == r){
  //       // printf("addr 0x%lx pass\n", addr);
  //       flag = true;
  //       break;
  //     }
  //   }
  //   if(!flag){
  //     printf("addr is 0x%lx, read res is 0x%lx\n", addr, res);
  //     assert(0);
  //   }
  // }
  // printf("check succ\n");

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
