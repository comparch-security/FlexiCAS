#ifndef CM_UTIL_PRINT_HPP_
#define CM_UTIL_PRINT_HPP_

#include <string>
#include <vector>
#include <utility>
#include <iostream>
#include "util/multithread.hpp"

// can be implemented using std::osyncstream after C++20
class PrintPool final {
  const int pool_size;
  AtomicVar<int> pw, pr;
  std::vector<std::string>      pool;
  std::vector<AtomicVar<bool> > valid;
  AtomicVar<bool> finish;
public:
  PrintPool(int pool_size) : pool_size(pool_size), pw(0), pr(0), pool(pool_size), valid(pool_size, false), finish(false) {}

  std::pair<std::string *, AtomicVar<bool> *> allocate() {
    while(true) {
      auto index = pw.read();
      if(valid[index].read()) pr.wait(); // full, wait
      if(pw.swap(index, (index + 1)%pool_size)) return std::pair(&(pool[index]), &(valid[index]));
    }
  }

  // a safer but might slower way of print a message
  void add(std::string& msg) {
    auto [buf, flag] = allocate();
    *buf = msg;
    flag->write(true, true);
  }

  void stop() { finish.write(true); }

  void sync() { // wait until all existing messages are printed
  auto index = (pw.read() + pool_size - 1) % pool_size;
  while(index != pr.read()) pr.wait();
  while(valid[index].read() != false) valid[index].wait();
  }

  void print() { // start print the pool
    auto index = pr.read();
    while(!finish.read()) {
      if(!valid[index].read()) { valid[index].wait(); continue; }
      std::cout << pool[index] << std::endl;
      valid[index].write(false, true);
      index = (index + 1) % pool_size;
      pr.write(index, true);
    }
  }
};

extern PrintPool *globalPrinter;

inline void global_print(std::string msg) {
  if(globalPrinter) globalPrinter->add(msg);
  else std::cout << msg << std::endl;
}

#endif
