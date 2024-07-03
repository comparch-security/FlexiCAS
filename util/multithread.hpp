#ifndef CM_UTIL_MULTITHREAD_HPP
#define CM_UTIL_MULTITHREAD_HPP

#include <unordered_map>
#include <stack>
#include <cstdint>
#include <cassert>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <memory>
#include <chrono>
#include <thread>
#include <iostream>

#ifdef BOOST_STACKTRACE_LINK
#include <boost/stacktrace.hpp>
#endif

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

class LockCheck {
  std::hash<std::thread::id> hasher;
  std::mutex hasher_mtx;

  #ifdef BOOST_STACKTRACE_LINK
    std::unordered_map<uint64_t, std::stack<std::pair<void *, std::string> > > lock_map; // lock should always behave like a stack
  #else
    std::unordered_map<uint64_t, std::stack<void *> > lock_map;
  #endif

  std::shared_mutex lock_map_mtx;

public:
  uint64_t thread_id() {
    std::lock_guard lock(hasher_mtx);
    return hasher(std::this_thread::get_id());
  }

  void push(void *p) {
    bool hit;
    auto id = thread_id();
    {
      std::shared_lock lock(lock_map_mtx);
      hit = lock_map.count(id);
      if(hit) {
        #ifdef BOOST_STACKTRACE_LINK
          lock_map[id].push(std::make_pair(p, boost::stacktrace::to_string(boost::stacktrace::stacktrace())));
        #else
          lock_map[id].push(p);
        #endif
      }
    }

    if(!hit) {
      std::lock_guard lock(lock_map_mtx);
      #ifdef BOOST_STACKTRACE_LINK
        lock_map[id].push(std::make_pair(p, boost::stacktrace::to_string(boost::stacktrace::stacktrace())));
      #else
        lock_map[id].push(p);
      #endif
    }
  }

  void pop(void *p) {
    auto id = thread_id();
    std::shared_lock lock(lock_map_mtx);
    assert(lock_map.count(id));
    #ifdef BOOST_STACKTRACE_LINK
      auto [pm, trace] = lock_map[id].top();
      assert(p == pm);
    #else
      assert(p == lock_map[id].top());
    #endif
    lock_map[id].pop();
  }

  void check() {
    auto id = thread_id();
    std::shared_lock lock(lock_map_mtx);
    #ifdef BOOST_STACKTRACE_LINK
    if(lock_map.count(id) && lock_map[id].size()) {
      auto [p, trace] = lock_map[id].top();
      std::cout << "metadata: " << (uint64_t)(p) << " is kept locked by thread " << id << std::endl;
      std::cout << trace << std::endl;
    }
    #endif
    assert(!lock_map.count(id) || lock_map[id].size() == 0);
  }
};

#ifdef CHECK_MULTI
extern LockCheck * global_lock_checker;
#endif

#endif
