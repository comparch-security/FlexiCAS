#include "cache/cache.hpp"
#include "define.hpp"
#include "util/log.hpp"
#include "util/common.hpp"
#include "util/random.hpp"
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <set>
#include <map>
#include <ctime>
#include <chrono>
#include <cstdio>
#include <thread>

int threads_num = 1;

uint64_t da[8] = {0, 1, 2, 3, 4, 5, 6, 7};
uint64_t db[8] = {1, 1, 1, 1, 1, 1, 1, 1};

std::vector<data_type *> data(4);
std::vector<l1_policy *> l1_p(16);
std::vector<l1_cache_type *> l1(16);
std::vector<l2_policy *> l2_p(2);
std::vector<l2_cache_type *> l2(2);
#ifdef THREE_LEVEL_CACHE
  std::vector<llc_policy *> llc_p(2);
  std::vector<llc_cache_type *> llc(2);
  std::vector<llc_dispatcher_type *> llc_dispatcher(1);
#endif
std::vector<memory_type *> mem(1);
std::vector<l1_inner_type *> core(16);
std::vector<PFCMonitor *> pfc(1);

FILE *lock_log_fp;

std::mutex  mtx;
std::vector<uint64_t> addr_buffer(threads_num);
std::condition_variable cv_addr;
uint64_t buffer_record = 0; 
uint64_t addr_num = 128000;


void init(){
  for(int i=0; i<2; i++) data[i] = new data_type();
  data[0]->write(da); data[1]->write(db);
  // initialize entities
  for(int i=0; i<16; i++) l1_p[i] = new l1_policy();
  for(int i=0; i<16; i++) l1[i] = new l1_cache_type(l1_p[i], std::string("l1") + "_" + std::to_string(i));
  for(int i=0; i<2; i++) l2_p[i] = new l2_policy();
  for(int i=0; i<2; i++) l2[i] = new l2_cache_type(l2_p[i], std::string("l2") + "_" + std::to_string(i));
  #ifdef THREE_LEVEL_CACHE
    for(int i=0; i<2; i++) llc_p[i] = new llc_policy();
    for(int i=0; i<2; i++) llc[i] = new llc_cache_type(llc_p[i], std::string("llc") + "_" + std::to_string(i));
    for(int i=0; i<1; i++) llc_dispatcher[i] = new llc_dispatcher_type(std::string("llc_dispatcher") + "_" + std::to_string(i));
  #endif
  for(int i=0; i<1; i++) mem[i] = new memory_type(std::string("mem") + "_" + std::to_string(i));

  for(int i=0; i<16; i++) core[i] = static_cast<l1_inner_type*>(l1[i]->inner);

  for(int i=0; i<1; i++) pfc[i] = new PFCMonitor(); 

  #ifdef LEVEL_1
    l1[0]->outer->connect(l2[0]->inner, l2[0]->inner->connect(l1[0]->outer));
    l1[1]->outer->connect(l2[0]->inner, l2[0]->inner->connect(l1[1]->outer));
    l1[2]->outer->connect(l2[0]->inner, l2[0]->inner->connect(l1[2]->outer));
    l1[3]->outer->connect(l2[0]->inner, l2[0]->inner->connect(l1[3]->outer));
    l1[4]->outer->connect(l2[0]->inner, l2[0]->inner->connect(l1[4]->outer));
    l1[5]->outer->connect(l2[0]->inner, l2[0]->inner->connect(l1[5]->outer));
    l1[6]->outer->connect(l2[0]->inner, l2[0]->inner->connect(l1[6]->outer));
    l1[7]->outer->connect(l2[0]->inner, l2[0]->inner->connect(l1[7]->outer));
    // l1[8]->outer->connect(l2[0]->inner, l2[0]->inner->connect(l1[8]->outer));
    // l1[9]->outer->connect(l2[0]->inner, l2[0]->inner->connect(l1[9]->outer));
    // l1[10]->outer->connect(l2[0]->inner, l2[0]->inner->connect(l1[10]->outer));
    // l1[11]->outer->connect(l2[0]->inner, l2[0]->inner->connect(l1[11]->outer));
    // l1[12]->outer->connect(l2[0]->inner, l2[0]->inner->connect(l1[12]->outer));
    // l1[13]->outer->connect(l2[0]->inner, l2[0]->inner->connect(l1[13]->outer));
    // l1[14]->outer->connect(l2[0]->inner, l2[0]->inner->connect(l1[14]->outer));
    // l1[15]->outer->connect(l2[0]->inner, l2[0]->inner->connect(l1[15]->outer));
    l2[0]->outer->connect(mem[0], mem[0]->connect(l2[0]->outer));
  #endif
}

void del(){
  for(int i=0; i<2; i++) delete data[i];
  for(int i=0; i<16; i++) delete l1[i];
  for(int i=0; i<2; i++) delete l2[i];
  #ifdef THREE_LEVEL_CACHE
    for(int i=0; i<2; i++) delete llc[i];
    for(int i=0; i<1; i++) delete llc_dispatcher[i];
  #endif
  for(int i=0; i<1; i++) delete mem[i];
  for(int i=0; i<1; i++) delete pfc[i];
}

void threadRW(int operation){
  database.insert_id(get_thread_id);
  uint64_t delay = 0;
  // uint64_t src1 = get_thread_id & 0xFFFFC0;
  uint64_t src1 = 0;
  uint64_t src2 = cm_get_random_uint64() & 0xFFFFC0;
  uint64_t addr = src1 + src2;
  data_type thread_data;
  uint64_t dd[8];
  database.insert_addr(addr, dd, database.get_sync());
  thread_data.write(dd);
  core[database.get_id(get_thread_id)]->write(addr, &thread_data, &delay);
}

void threadRW_same_addr(uint64_t addr){
  database.insert_id(get_thread_id);
  uint64_t delay = 0;
  data_type thread_data;
  uint64_t dd[8];
  database.insert_addr(addr, dd, database.get_sync());
  thread_data.write(dd);
  core[database.get_id(get_thread_id)]->write(addr, &thread_data, &delay);
}
 
void producer_thread(){
  std::unique_lock lk(mtx, std::defer_lock);

  for(int i = 0; i < addr_num/threads_num; i++){
    lk.lock();
    cv_addr.wait(lk, [] { return buffer_record == 0;});

    for(int i = 0; i < threads_num; i++){
      addr_buffer[i] = cm_get_random_uint64() & 0xFFFFC0;
      buffer_record |= (1 << i);
    }
    database.add_sync();

    lk.unlock();
    cv_addr.notify_all();
    // lock_log_write("sync %d\n", i);
  }

  lk.lock();
  cv_addr.wait(lk, [] {return buffer_record == 0;});
  for(int i = 0; i <= threads_num ; i++){
    buffer_record |= (1<<i);
  }
  lk.unlock();
  cv_addr.notify_all();

}

void worker_thread(int id){
  std::unique_lock lk(mtx, std::defer_lock);
  database.insert_id(get_thread_id);
  while(true){
    lk.lock();
    cv_addr.wait(lk, [id]{ return (buffer_record & (1 << id)) == (1 << id);});
    lk.unlock();

    if((buffer_record & (1 << threads_num)) == (1 << threads_num)){
      break;
    }

    uint64_t delay = 0;
    uint64_t addr = addr_buffer[id];
    data_type thread_data;
    uint64_t dd[8];
    database.insert_addr(addr, dd, database.get_sync());
    thread_data.write(dd);
    core[id]->write(addr_buffer[id], &thread_data, &delay);

    lk.lock();
    buffer_record &= ~(1 << id);
    lk.unlock();

    cv_addr.notify_all();
  }
}


int main(){
  init();
  #ifdef LEVEL_1
    lock_log_fp = fopen("dtrace", "w");

    auto start = std::chrono::high_resolution_clock::now();
    std::thread producer(producer_thread);
    std::vector<std::thread> thread_array(threads_num);
    for(int i = 0; i < threads_num; i++){
      thread_array[i] = std::thread(worker_thread, i);
    }

    producer.join();
    for(int i = 0; i < threads_num; i++){
      thread_array[i].join();
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;

    std::cout << "thread num  : " << threads_num << std::endl;
    std::cout << "access addr : " << addr_num << std::endl;
    std::cout << "cost time   : " << duration.count() << " s " << std::endl;

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
    
    del();
  #endif
}