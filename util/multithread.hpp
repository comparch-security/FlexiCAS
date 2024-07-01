#ifndef CM_UTIL_MULTITHREAD_HPP
#define CM_UTIL_MULTITHREAD_HPP

#include <unordered_map>
#include <cstdint>
#include <cassert>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <chrono>
#include <iostream>

template<typename T>
class AtomicVar {
  std::unique_ptr<std::atomic<T> > var;
  std::mutex mtx;
  std::condition_variable cv;

public:
  AtomicVar() : var(new std::atomic<T>()) {}
  AtomicVar(const T& v) : var(new std::atomic<T>(v)) {}

  // to put AtomicVar into a runtime resizable vector, a copy constructor must be provided
  AtomicVar(const AtomicVar<T>& v) : var(new std::atomic<T>(v.read())) {}

  __always_inline T read() const {
    return var->load();
  }

  __always_inline void write(T v, bool notify = false) {
    var->store(v);
    if(notify) cv.notify_all();
  }

  __always_inline bool swap(T& expect, T v, bool notify = false) {
    bool rv = var->compare_exchange_strong(expect, v);
    if(rv && notify) {
      std::unique_lock lk(mtx);
      cv.notify_all();
    }
    return rv;
  }

  __always_inline void wait(bool report = false) {
    using namespace std::chrono_literals;
    std::unique_lock lk(mtx);
    auto result = cv.wait_for(lk, 100us);
    if(report && std::cv_status::timeout == result)
      std::cerr << "cv [" << std::hex << this << "] waits timeout once ..." << std::endl;
  }
};

// a database for recoridng the pending transactions
template<bool EnMT>
class PendingXact {
  std::unordered_map<uint64_t, uint64_t> db;
  std::mutex mtx;
public:
  PendingXact() {}
  virtual ~PendingXact() {}

  void insert(uint64_t addr, uint32_t id) {
    std::lock_guard lk(mtx);
    if(!db.count(addr)) db[addr] = 0;
    assert(0ull == (db[addr] & (1ull << id)) || 0 ==
           "The to be inserted <addr, id> pair has already been inserted in the database!");
    db[addr] |= (1ull << id);
  }

  void remove(uint64_t addr, uint32_t id) {
    std::lock_guard lk(mtx);
    if(db.count(addr)) {
      db[addr] &= ~(1ull << id);
      if(db[addr] == 0) db.erase(addr);
    }
  }

  bool count(uint64_t addr, uint32_t id) {
    std::lock_guard lk(mtx);
    return db.count(addr) && (db[addr] & (1ull << id));
  }
};

// specialization for non-multithread env
template<>
class PendingXact<false> {

  uint64_t addr;
  int32_t  id;

public:
  PendingXact(): addr(0), id(0) {}
  virtual ~PendingXact() {}

  void insert(uint64_t addr, uint32_t id) {
    this->addr = addr;
    this->id = id;
  }

  void remove(uint64_t addr, uint32_t id) {
    if(count(addr, id)) this->addr = 0;
  }

  bool count(uint64_t addr, uint32_t id) {
    return this->addr == addr && this->id == id;
  }
};


#endif
