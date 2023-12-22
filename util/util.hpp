#ifndef CM_UTIL_UTIL_HPP
#define CM_UTIL_UTIL_HPP

#define get_thread_id std::hash<std::thread::id>{}(std::this_thread::get_id())

#include <mutex>
#include <unordered_map>

typedef struct{
  uint32_t  ai;
  uint32_t s;
  uint32_t w;
}addr_loc;

/////////////////////////////////
// Record inner probing address
class InnerProbeRecord
{
protected:
  std::mutex  mtx;           // mutex for protecting record
  std::unordered_map<uint64_t, addr_loc> map;
public:
  void add(uint64_t addr, addr_loc loc){
    std::unique_lock lk(mtx, std::defer_lock);
    lk.lock();
    map[addr] = loc;
    lk.unlock();
  }
  void erase(uint64_t addr){
    std::unique_lock lk(mtx, std::defer_lock);
    lk.lock();
    map.erase(addr);
    lk.unlock();
  }
  std::tuple<bool, addr_loc> query(uint64_t addr){
    std::unique_lock lk(mtx, std::defer_lock);
    bool va; 
    addr_loc loc;
    lk.lock();
    if(map.count(addr)){
      va = true;
      loc = map[addr];
    }else{
      va = false;
    }
    lk.unlock();
    return std::make_pair(va, loc);
  }
  bool valid(uint64_t addr){
    std::unique_lock lk(mtx, std::defer_lock);
    bool va;
    lk.lock();
    va = map.count(addr);
    lk.unlock();
    return va;
  }
};

#endif