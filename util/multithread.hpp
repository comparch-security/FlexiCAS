#ifndef CM_UTIL_MULTITHREAD_HPP
#define CM_UTIL_MULTITHREAD_HPP

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>

template<typename T>
class AtomicVar {
  std::unique_ptr<std::atomic<T> > var;
  std::mutex mtx;
  std::condition_variable cv;

public:
  AtomicVar() : var(new std::atomic<T>()) {}
  AtomicVar(const T& v) : var(new std::atomic<T>(v)) {}

  __always_inline T read() const {
    return var->load();
  }

  __always_inline void write(T v) {
    var->store(v);
  }

  __always_inline bool swap(T& expect, T v, bool notify = false) {
    bool rv = var->compare_exchange_strong(expect, v);
    if(rv && notify) {
      std::unique_lock lk(mtx);
      cv.notify_all();
    }
    return rv;
  }

  __always_inline void wait() {
    std::unique_lock lk(mtx);
    cv.wait(lk);
  }
};

#endif
