#ifndef CM_UTIL_UTIL_HPP
#define CM_UTIL_UTIL_HPP

#include <cassert>
#define get_thread_id std::hash<std::thread::id>{}(std::this_thread::get_id())

#include <condition_variable>
#include <vector>
#include <mutex>
#include <unordered_map>

typedef struct{
  uint32_t ai    ;
  uint32_t s     ;
  uint32_t w     ;
  std::mutex* mtx;
  std::mutex* cmtx; // cacheline mutex
  std::condition_variable* cv;
  std::vector<uint32_t>* status;
}addr_info;

typedef struct{
  bool valid;
  addr_info loc;
}info;

/////////////////////////////////
// Record inner probing address
class InnerAcquireRecord
{
protected:
  std::vector<std::mutex *>  mtx;           // mutex for protecting record
  std::vector<std::unordered_map<uint64_t, addr_info> > map;
public:
  void add(int id, uint64_t addr, addr_info loc){
    std::unique_lock lk(*mtx[id], std::defer_lock);
    lk.lock();
    map[id][addr] = loc;
    lk.unlock();
  }
  void erase(int id, uint64_t addr){
    std::unique_lock lk(*mtx[id], std::defer_lock);
    lk.lock();
    map[id].erase(addr);
    lk.unlock();
  }
  std::pair<bool, addr_info> query(int id, uint64_t addr){
    addr_info info;
    bool count;
    std::unique_lock lk(*mtx[id], std::defer_lock);
    lk.lock();
    if(map[id].count(addr)){
      count = true;
      info = map[id][addr];
    }else{
      count = false;
    }
    lk.unlock();
    return std::make_pair(count, info);
  }
  void add_size(){
    map.resize(map.size()+1);
    std::mutex* m = new std::mutex();
    mtx.push_back(m);
  }

  ~InnerAcquireRecord(){
    for(auto m : mtx) delete m;
  }
// protected:
//   std::mutex mtx;  
//   std::vector<info> array;
// public:
//   void add(int id, uint64_t addr, addr_info loc){
//     array[id] = info{true, loc};
//   }
//   void erase(int id, uint64_t addr){
//     array[id].valid = false;
//   }
//   std::pair<bool, addr_info> query(int id, uint64_t addr){
//     return std::make_pair(array[id].valid, array[id].loc);
//   }
//   void add_size(){
//     array.resize(array.size()+1);
//   }

};

#endif