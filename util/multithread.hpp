#ifndef CM_UTIL_MULTITHREAD_HPP
#define CM_UTIL_MULTITHREAD_HPP

#include <unordered_map>
#include <vector>
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
class AtomicVar final {
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
    if(notify) cv.notify_one();
  }

  __always_inline bool swap(T& expect, T v, bool notify = false) {
    bool rv = var->compare_exchange_strong(expect, v);
    if(rv && notify) {
      std::unique_lock lk(mtx);
      cv.notify_one();
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

class CMMetadataBase; // forward declaration

template<bool EnMT, int MSHR = 16>
class PendingXact final {
  std::vector<std::tuple<uint64_t, bool, CMMetadataBase *, uint32_t, uint32_t> > xact;
  std::vector<bool> valid;
  std::mutex mtx;

  __always_inline uint64_t key(uint64_t addr, int32_t id) {
    assert(id < 63 || 0 == "We do not support more than 63 coherent inner cache for any cache level!");
    return addr | (id & 0x3f);
  }

  __always_inline int find(uint64_t addr, int32_t id) {
    auto k = key(addr, id);
    for(int i=0; i<MSHR; i++) if(valid[i] && k == std::get<0>(xact[i])) return i;
    return -1;
  }

public:
  PendingXact(): xact(MSHR), valid(MSHR, false) {}

  void insert(uint64_t addr, int32_t id, bool forward, CMMetadataBase *meta, uint32_t ai, uint32_t s) {
    std::lock_guard lk(mtx);
    int index = 0;
#ifdef CHECK_MULTI
    assert(-1 == find(addr, id) || "The transaction has already been inserted!");
#endif
    // get an empty place
    for(int i=0; i<MSHR; i++) if(!valid[i]) { index = i; break; }
    assert(index < MSHR || 0 == "Pending transaction queue for finish message overflow!");
    valid[index] = true;
    xact[index] = std::make_tuple(key(addr, id), forward, meta, ai, s);
  }

  void remove(uint64_t addr, int32_t id) {
    std::lock_guard lk(mtx);
    int index = find(addr, id);
    if(index >= 0) valid[index] = false;
  }

  std::tuple<bool, bool, CMMetadataBase *, uint32_t, uint32_t>
  read(uint64_t addr, int32_t id) {
    std::lock_guard lk(mtx);
    auto index = find(addr, id);
    if(index >= 0) {
      auto [key, forward, meta, ai, s] = xact[index];
      return std::make_tuple(true, forward, meta, ai, s);
    } else
      return std::make_tuple(false, false, nullptr, 0, 0);
  }
};

// specialization for non-multithread env
template<>
class PendingXact<false> final {

  uint64_t addr;
  int32_t  id;
  bool     forward;

public:
  PendingXact(): addr(0), id(0) {}

  void insert(uint64_t addr, int32_t id, bool forward, CMMetadataBase *, uint32_t, uint32_t) {
    this->addr = addr;
    this->id = id;
    this->forward = forward;
  }

  void remove(uint64_t addr, int32_t id) {
    if(this->addr == addr && this->id == id) this->addr = 0;
  }

  std::tuple<bool, bool, CMMetadataBase *, uint32_t, uint32_t>
  read(uint64_t addr, int32_t id) {
    if(this->addr == addr && this->id == id) return std::make_tuple(true,  forward, nullptr, 0, 0);
    else                                     return std::make_tuple(false, false,   nullptr, 0, 0);
  }
};

class LockCheck final {
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
    auto id = thread_id();
    std::lock_guard lock(lock_map_mtx);
#ifdef BOOST_STACKTRACE_LINK
      lock_map[id].push(std::make_pair(p, boost::stacktrace::to_string(boost::stacktrace::stacktrace())));
#else
      lock_map[id].push(p);
#endif
  }

  void pop(void *p) {
    auto id = thread_id();
    std::lock_guard lock(lock_map_mtx);
    assert(lock_map.count(id));
    #ifdef BOOST_STACKTRACE_LINK
      auto [pm, trace] = lock_map[id].top();
      if(p != pm) {
        std::cout << "unlock violate the LIFO order." << std::endl;
        std::cout << "metadata: " << (uint64_t)(pm) << " is at the top" << std::endl;
        std::cout << trace << std::endl;
      }
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
      std::cout << "\n the current trace:" << std::endl;
      std::cout << boost::stacktrace::stacktrace() << std::endl;
    }
    #endif
    assert(!lock_map.count(id) || lock_map[id].size() == 0);
  }
};

#ifdef CHECK_MULTI
extern LockCheck * global_lock_checker;
#endif

#endif
