#ifndef CM_UTIL_PARALLEL_REGRESSION_HPP
#define CM_UTIL_PARALLEL_REGRESSION_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>
#include <queue>
#include "util/regression.hpp"


class cache_xact 
{
public:
  char op_t;
  bool ic;
  int core;
  uint64_t addr;
  Data64B data;
};

// template <int NC, typename DT>
// class DTContainer
// {
// protected:
//   uint64_t addr;
//   std::queue<DT> data;
//   std::mutex mtx;
// public:
//   void init(uint64_t waddr) { addr = waddr;}
//   void write(DT* wdata){
//     DT tdata;
//     tdata.copy(wdata);
//     mtx.lock();
//     data.push(tdata);
//     if(data.size() > NC) data.pop();
//     mtx.unlock();
//   }
// };


#endif