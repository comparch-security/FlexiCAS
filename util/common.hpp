#ifndef CM_UTIL_COMMON_HPP
#define CM_UTIL_COMMON_HPP

#include "util/random.hpp"
#include <cassert>
#include <list>
#include <unordered_map>
#include <cstdint>
#include <mutex>
#include <set>


typedef struct{
  uint32_t sync;
  std::list<uint64_t> l;
}addr_p_data;

class ThreadDataBase
{
protected:
  std::unordered_map<uint32_t, int> thread_map;
  uint32_t count = 0;
  std::mutex id_mtx, addr_mtx;
  uint32_t sync = 0;
public:
  std::set<uint64_t> addr_set;
  std::unordered_map<uint64_t, addr_p_data > addr_map;

  void insert_id(uint32_t thread_hash){
    id_mtx.lock();
    thread_map[thread_hash] = count;
    count++;
    id_mtx.unlock();
  }
  int get_id(uint32_t thread_hash){
    return thread_map[thread_hash];
  }
  void clear(){
    thread_map.clear();
    count = 0;
  }
  void insert_addr(uint64_t addr, uint64_t data[], uint32_t sync){
    addr_mtx.lock();
    uint64_t res = 0;
    for(int i = 0; i<8; i++){
      data[i] = cm_get_random_uint32();
      res += data[i];
    }
    if(addr_map.count(addr)){
      if(addr_map[addr].sync != sync){
        addr_map[addr].sync = sync;
        addr_map[addr].l.clear();
      }
      addr_map[addr].l.push_back(res);
    }else{
      addr_map[addr].sync = sync;
      addr_map[addr].l.push_back(res);
    }
    addr_set.insert(addr);
    addr_mtx.unlock();
  }

  void add_sync(){
    sync++;
  }

  uint32_t get_sync(){
    return sync;
  }
};

extern ThreadDataBase database;





#endif