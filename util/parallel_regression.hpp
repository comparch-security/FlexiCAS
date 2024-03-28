#ifndef CM_UTIL_PARALLEL_REGRESSION_HPP
#define CM_UTIL_PARALLEL_REGRESSION_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>
#include <queue>


static const uint64_t addr_mask = 0x0ffffffffffc0ull;

class cache_xact 
{
public:
  char op_t;
  bool ic;
  int core;
  uint64_t addr;
  Data64B data;
};

template <int NC, typename DT>
class DTContainer
{
protected:
  uint64_t addr;
  std::queue<DT*> data;
  std::mutex mtx;
public:
  void init(uint64_t waddr) { addr = waddr;}
  void write(DT* wdata){
    if constexpr (!C_VOID(DT)){
      DT* tdata = new DT();
      tdata->copy(wdata);
      mtx.lock();
      data.push(tdata);
      if(data.size() > NC) {
        auto d = data.front();
        delete d;
        data.pop();
      }
      mtx.unlock();
    }
  }
};


#endif