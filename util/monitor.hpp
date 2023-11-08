#ifndef CM_UTIL_PFC_HPP
#define CM_UTIL_PFC_HPP

#include <cstdint>
#include <set>
#include "util/delay.hpp"
#include "util/concept_macro.hpp"

// monitor base class
class MonitorBase
{
public:
  MonitorBase() {}
  virtual ~MonitorBase() {}

  // standard functions to supprt a type of monitoring
  virtual bool attach(uint64_t cache_id) = 0; // decide whether to attach the mointor to this cache
  virtual void read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit) = 0;
  virtual void write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit) = 0;
  virtual void invalid(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w) = 0;

  // control
  virtual void start() = 0;    // start the monitor, assuming the monitor is just initialized
  virtual void stop() = 0;     // stop the monitor, assuming it will soon be destroyed
  virtual void pause() = 0;    // pause the monitor, assming it will resume later
  virtual void resume() = 0;   // resume the monitor, assuming it has been paused
  virtual void reset() = 0;    // reset all internal statistics, assuming to be later started as new
};

// mointor container used in cache
class MonitorContainerBase
{
protected:
  const uint32_t id;                    // a unique id to identify the attached cache
  std::set<MonitorBase *> monitors;     // performance moitors

public:

  MonitorContainerBase(uint32_t id) : id(id) {}

  virtual ~MonitorContainerBase() {}

  virtual void attach_monitor(MonitorBase *m) = 0;

  // support run-time assign/reassign mointors
  void detach_monitor() { monitors.clear(); }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay, unsigned int genre = 0) = 0;
  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay, unsigned int genre = 0) = 0;
  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, uint64_t *delay, unsigned int genre = 0) = 0;
};

// Cache monitor and delay support
template<typename DLY, bool EnMon>
class CacheMonitorSupport : public MonitorContainerBase
{
protected:
  DLY *timer;                           // delay estimator

public:
  CacheMonitorSupport(uint32_t id) : MonitorContainerBase(id) {
    if constexpr (!C_VOID(DLY)) timer = new DLY();
  }

  virtual ~CacheMonitorSupport() {
    if constexpr (!C_VOID(DLY)) delete timer;
  }

  virtual void attach_monitor(MonitorBase *m) {
    if constexpr (EnMon) {
      if(m->attach(id)) monitors.insert(m);
    }
  }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay, unsigned int genre = 0) {
    if constexpr (EnMon) for(auto m:monitors) m->read(addr, ai, s, w, hit);
    if constexpr (!C_VOID(DLY)) timer->read(addr, ai, s, w, hit, delay);
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay, unsigned int genre = 0) {
    if constexpr (EnMon) for(auto m:monitors) m->write(addr, ai, s, w, hit);
    if constexpr (!C_VOID(DLY)) timer->write(addr, ai, s, w, hit, delay);
  }

  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, uint64_t *delay, unsigned int genre = 0) {
    if(hit && evict) {
      if constexpr (EnMon) for(auto m:monitors) m->invalid(addr, ai, s, w);
    }
    if constexpr (!C_VOID(DLY)) timer->manage(addr, ai, s, w, hit, evict, writeback, delay);
  }

};

// performance counter
class PFCMonitor : public MonitorBase
{
protected:
  uint64_t cnt_access, cnt_miss, cnt_write, cnt_write_miss, cnt_invalid;
  bool active;

public:
  PFCMonitor() : cnt_access(0), cnt_miss(0), cnt_write(0), cnt_write_miss(0), cnt_invalid(0), active(false) {}
  virtual ~PFCMonitor() {}

  virtual bool attach(uint64_t cache_id) { return true; }

  virtual void read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit)  {
    if(!active) return;
    cnt_access++;
    if(!hit) cnt_miss++;
  }

  virtual void write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit) {
    if(!active) return;
    cnt_access++;
    cnt_write++;
    if(!hit) {
      cnt_miss++;
      cnt_write_miss++;
    }
  }

  virtual void invalid(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w) {
    if(!active) return;
    cnt_invalid++;
  }

  virtual void start() { active = true;  }
  virtual void stop()  { active = false; }
  virtual void pause() { active = false; }
  virtual void resume() { active = true; }
  virtual void reset() {
    cnt_access = 0;
    cnt_miss = 0;
    cnt_write = 0;
    cnt_write_miss = 0;
    cnt_invalid = 0;
    active = false;
  }

  // special function supported by PFC only
  uint64_t get_access() { return cnt_access; }
  uint64_t get_access_read() { return cnt_access - cnt_write; }
  uint64_t get_access_write() {return cnt_write; }
  uint64_t get_miss() {return cnt_miss; }
  uint64_t get_miss_read() { return cnt_miss - cnt_write_miss; }
  uint64_t get_miss_write() { return cnt_write_miss; }
  uint64_t get_invalid() { return cnt_invalid; }
};

#endif
