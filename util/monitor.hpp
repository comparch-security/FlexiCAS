#ifndef CM_UTIL_PFC_HPP
#define CM_UTIL_PFC_HPP

#include <cstdint>
#include <set>

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
