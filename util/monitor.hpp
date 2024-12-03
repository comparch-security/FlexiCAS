#ifndef CM_UTIL_PFC_HPP
#define CM_UTIL_PFC_HPP

#include <cstdint>
#include <set>
#include <string>
#include <thread>
#include <boost/format.hpp>
#include "util/delay.hpp"
#include "util/concept_macro.hpp"
#include "util/print.hpp"
#include "cache/metadata.hpp"
#include "util/random.hpp"

class CMDataBase;
class CMMetadataBase;

// monitor base class
class MonitorBase
{
protected:
  std::string prefix; // in case a log file is generate, specify the prefix of the log file
public:
  virtual ~MonitorBase() = default;

  // standard functions to supprt a type of monitoring
  virtual bool attach(uint64_t cache_id) = 0; // decide whether to attach the mointor to this cache
  virtual void read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data) = 0;
  virtual void write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data) = 0;
  virtual void invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, const CMMetadataBase *meta, const CMDataBase *data) = 0;
  virtual bool magic_func(uint64_t cache_id, uint64_t addr, uint64_t magic_id, void *magic_data) { return false; } // a special function to log non-standard information to a special monitor

  // control
  virtual void start() = 0;    // start the monitor, assuming the monitor is just initialized
  virtual void stop() = 0;     // stop the monitor, assuming it will soon be destroyed
  virtual void pause() = 0;    // pause the monitor, assming it will resume later
  virtual void resume() = 0;   // resume the monitor, assuming it has been paused
  virtual void reset() = 0;    // reset all internal statistics, assuming to be later started as new

  __always_inline void set_prefix(const std::string& s) { prefix = s; }
};

// mointor container used in cache
class MonitorContainerBase
{
protected:
  const uint32_t id;                    // a unique id to identify the attached cache
  std::set<MonitorBase *> monitors;     // performance moitors

public:
  MonitorContainerBase(uint32_t id) : id(id) {}
  virtual ~MonitorContainerBase() = default;

  virtual void attach_monitor(MonitorBase *m) = 0;

  // support run-time assign/reassign mointors
  void detach_monitor() { monitors.clear(); }

  virtual void hook_read(uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay, unsigned int genre = 0) = 0;
  virtual void hook_write(uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay, unsigned int genre = 0) = 0;
  virtual void hook_manage(uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, bool evict, bool writeback, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay, unsigned int genre = 0) = 0;
  virtual void magic_func(uint64_t addr, uint64_t magic_id, void *magic_data) = 0; // an interface for special communication with a specific monitor if attached
  virtual void pause() = 0;
  virtual void resume() = 0;
};

// class monitor helper
class CacheMonitorSupport
{
public:
  MonitorContainerBase *monitors; // monitor container

  // hook interface for replacer state update, Monitor and delay estimation
  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay) = 0;
  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay) = 0;
  // probe, invalidate and writeback
  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint32_t evict, bool writeback, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay) = 0;
  // an interface for special communication with a specific monitor if attached
  __always_inline void monitor_magic_func(uint64_t addr, uint64_t magic_id, void *magic_data) {
    monitors->magic_func(addr, magic_id, magic_data);
  }
};

// Cache monitor and delay support
template<typename DLY, bool EnMon>
class CacheMonitorImp : public MonitorContainerBase
{
protected:
  DLY *timer;                           // delay estimator

public:
  CacheMonitorImp(uint32_t id) : MonitorContainerBase(id) {
    if constexpr (!C_VOID<DLY>) timer = new DLY();
  }

  virtual ~CacheMonitorImp() override {
    if constexpr (!C_VOID<DLY>) delete timer;
  }

  virtual void attach_monitor(MonitorBase *m) override {
    if constexpr (EnMon) {
      if(m->attach(id)) monitors.insert(m);
    }
  }

  virtual void hook_read(uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay, unsigned int genre = 0) override {
    if constexpr (EnMon) for(auto m:monitors) m->read(id, addr, ai, s, w, ev_rank, hit, meta, data);
    if constexpr (!C_VOID<DLY>) timer->read(addr, ai, s, w, hit, delay);
  }

  virtual void hook_write(uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay, unsigned int genre = 0) override {
    if constexpr (EnMon) for(auto m:monitors) m->write(id, addr, ai, s, w, ev_rank, hit, meta, data);
    if constexpr (!C_VOID<DLY>) timer->write(addr, ai, s, w, hit, delay);
  }

  virtual void hook_manage(uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, bool evict, bool writeback, const CMMetadataBase *meta, const CMDataBase *data, uint64_t *delay, unsigned int genre = 0) override {
    if(hit && evict) {
      if constexpr (EnMon) for(auto m:monitors) m->invalid(id, addr, ai, s, w, ev_rank, meta, data);
    }
    if constexpr (!C_VOID<DLY>) timer->manage(addr, ai, s, w, hit, evict, writeback, delay);
  }

  virtual void magic_func(uint64_t addr, uint64_t magic_id, void *magic_data) {
    if constexpr (EnMon) {
      for(auto m:monitors)
        if(m->magic_func(id, addr, magic_id, magic_data))
          return;
    }
  }

  virtual void pause() override {
    if constexpr (EnMon) {
      for(auto monitor : monitors) monitor->pause();
    }
  }

  virtual void resume() override {
    if constexpr (EnMon) {
      for(auto monitor : monitors) monitor->resume();
    }
  }
};

// Simple Access Monitor
class SimpleAccMonitor : public MonitorBase
{
protected:
  uint64_t cnt_access = 0, cnt_miss = 0, cnt_write = 0, cnt_write_miss = 0, cnt_invalid = 0;
  bool active;

public:
  SimpleAccMonitor(bool active = false) : active(active) {}

  virtual bool attach(uint64_t cache_id) override { return true; }

  virtual void read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data)  override {
    if(!active) return;
    cnt_access++;
    if(!hit) cnt_miss++;
  }

  virtual void write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data) override {
    if(!active) return;
    cnt_access++;
    cnt_write++;
    if(!hit) {
      cnt_miss++;
      cnt_write_miss++;
    }
  }

  virtual void invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, const CMMetadataBase *meta, const CMDataBase *data) override {
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

// a tracer
class SimpleTracer : public MonitorBase
{
  bool active;
  bool compact_data;

  virtual void print(std::string& msg) { std::cout << msg << std::endl; }

public:
  SimpleTracer(bool cd = false): active(false), compact_data(cd) {}

  virtual bool attach(uint64_t cache_id) { return true; }

  virtual void read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data) override {
    if(!active) return;
    std::string msg;  msg.reserve(100);
    msg += (boost::format("%-10s read  %016x %02d %04d %02d %02d %1x") % UniqueID::name(cache_id) % addr % ai % s % w % ev_rank % hit).str();

    if(meta)
      msg.append(" [").append(meta->to_string()).append("]");
    else if(data)
      msg.append("      ");

    if(data)
      msg.append(" ").append(compact_data ? (boost::format("%016x") % (data->read(0))).str() : data->to_string());

    print(msg);
  }
  virtual void write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data) override {
    if(!active) return;
    std::string msg;  msg.reserve(100);
    msg += (boost::format("%-10s write %016x %02d %04d %02d %02d %1x") % UniqueID::name(cache_id) % addr % ai % s % w % ev_rank % hit).str();

    if(meta)
      msg.append(" [").append(meta->to_string()).append("]");
    else if(data)
      msg.append("      ");

    if(data)
      msg.append(" ").append(compact_data ? (boost::format("%016x") % (data->read(0))).str() : data->to_string());

    print(msg);
  }
  virtual void invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, const CMMetadataBase *meta, const CMDataBase *data) override {
    if(!active) return;
    std::string msg;  msg.reserve(100);
    msg += (boost::format("%-10s evict %016x %02d %04d %02d %02d  ") % UniqueID::name(cache_id) % addr % ai % s % w % ev_rank).str() ;

    if(meta)
      msg.append(" [").append(meta->to_string()).append("]");
    else if(data)
      msg.append("      ");

    if(data)
      msg.append(" ").append(compact_data ? (boost::format("%016x") % (data->read(0))).str() : data->to_string());

    print(msg);
  }

  virtual void start() override { active = true;  }
  virtual void stop()  override { active = false; }
  virtual void pause() override { active = false; }
  virtual void resume() override { active = true; }
  virtual void reset() override { active = false; }
};

// multithread version of simple tracer
class SimpleTracerMT : public SimpleTracer
{
  std::thread print_thread;
  std::hash<std::thread::id> hasher;
  virtual void print(std::string& msg) override {
    uint16_t id = hasher(std::this_thread::get_id());
    std::string msg_ext = (boost::format("thread %04x: %s") % id % msg).str();
    globalPrinter->add(msg_ext);
  }

public:
  SimpleTracerMT(bool cd = false): SimpleTracer(cd) {
    print_thread = std::thread(&PrintPool::print, globalPrinter);
  }

  virtual void stop() { globalPrinter->stop(); print_thread.join(); }
};

// Simple Access Monitor
class AddrTracer : public MonitorBase
{
protected:
  uint64_t target;
  bool active;
  bool compact_data = true;

  __always_inline void print(std::string& msg) { std::cout << msg << std::endl; }

public:
  AddrTracer(bool active = false) : active(active) {}

  virtual bool attach(uint64_t cache_id) override { return true; }

  virtual void read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data)  override {
    if(!active || addr != target) return;
    std::string msg;  msg.reserve(100);
    msg += (boost::format("%-10s read  %016x %02d %04d %02d %02d %1x") % UniqueID::name(cache_id) % addr % ai % s % w % ev_rank % hit).str();

    if(meta)
      msg.append(" [").append(meta->to_string()).append("]");
    else if(data)
      msg.append("      ");

    if(data)
      msg.append(" ").append(compact_data ? (boost::format("%016x") % (data->read(0))).str() : data->to_string());

    print(msg);
  }

  virtual void write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data) override {
    if(!active || target != addr) return;
    std::string msg;  msg.reserve(100);
    msg += (boost::format("%-10s write %016x %02d %04d %02d %02d %1x") % UniqueID::name(cache_id) % addr % ai % s % w % ev_rank % hit).str();

    if(meta)
      msg.append(" [").append(meta->to_string()).append("]");
    else if(data)
      msg.append("      ");

    if(data)
      msg.append(" ").append(compact_data ? (boost::format("%016x") % (data->read(0))).str() : data->to_string());

    print(msg);
  }

  virtual void invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, const CMMetadataBase *meta, const CMDataBase *data) override {
    if(!active || target != addr) return;
    std::string msg;  msg.reserve(100);
    msg += (boost::format("%-10s evict %016x %02d %04d %02d %02d  ") % UniqueID::name(cache_id) % addr % ai % s % w % ev_rank).str() ;

    if(meta)
      msg.append(" [").append(meta->to_string()).append("]");
    else if(data)
      msg.append("      ");

    if(data)
      msg.append(" ").append(compact_data ? (boost::format("%016x") % (data->read(0))).str() : data->to_string());

    print(msg);
  }

  virtual void start() { active = true;  }
  virtual void stop()  { active = false; }
  virtual void pause() { active = false; }
  virtual void resume() { active = true; }
  virtual void reset() {  active = false; }

  // special function supported by PFC only
  void set_target(uint64_t addr) { target = addr; }
};

#endif
