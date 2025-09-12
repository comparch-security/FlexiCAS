#ifndef CM_RSA_MONITOR_HPP
#define CM_RSA_MONITOR_HPP

#include "util/monitor.hpp"
#include "rsa/history.hpp"
#include "cache/dynamic_random.hpp"
#include <ctime>

#define EvictDistAttackEvent   2024081400ull
#define EvictDistConflictEvent 2024081401ull
#define EvictDistPredHitEvent  2024092000ull
#define EvictDistEvictRank     2024100800ull

struct EVRecord {
  uint64_t c_acc;
  uint64_t c_ev;
  uint64_t s_acc;
  uint64_t s_ev;
  uint32_t si; // set index
};

class SeqAssAddrTracer : public AddrTracer
{
public:
  virtual bool magic_func(uint64_t cache_id, uint64_t addr, uint64_t magic_id, void *magic_data) override {
    if(!active) return false;
    if(addr == target && magic_id == EvictDistRelocEvent) {
      std::string msg;  msg.reserve(100);
      msg += (boost::format("%-10s relocate  %016x") % UniqueID::name(cache_id) % addr).str();
      print(msg);
    }
    return AddrTracer::magic_func(cache_id, addr, magic_id, magic_data);
  }
};


class SeqAssRemapMonitor : public MonitorBase
{
  bool active = false;
  uint32_t llc_threshold, set_threshold;
  uint32_t attack_cnt = 0;
  std::map<uint32_t, uint32_t> set_attack_cnt;
  bool remap_active = false;
  
public:

  SeqAssRemapMonitor(uint32_t llc_th, uint32_t set_th) : llc_threshold(llc_th), set_threshold(set_th) {}

  virtual bool magic_func(uint64_t cache_id, uint64_t addr, uint64_t magic_id, void *magic_data) {
    if(!active) return false;
    if(magic_id == EvictDistAttackEvent) {
      attack_cnt++;
      auto s = *(static_cast<uint32_t *>(magic_data));
      if(set_attack_cnt.count(s)) set_attack_cnt[s]++;
      else set_attack_cnt[s] = 1;
      if(!remap_active && (attack_cnt >= llc_threshold || set_attack_cnt[s] >= set_threshold)) {
        std::cout << "record an active remap!" << std::endl;
        remap_active = true;
        attack_cnt = 0;
        set_attack_cnt[s] = 0;
      }
      return false;
    }
    if(magic_id == MAGIC_ID_REMAP_ASK) {
      *static_cast<bool*>(magic_data) = remap_active;
      return true;
    }
    if(magic_id == MAGIC_ID_REMAP_END) {
      attack_cnt = 0;
      remap_active = false;
      return false;
    }
    return false;
  }

  virtual bool attach(uint64_t cache_id) {
    return true;
  }

  virtual void read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data) override {}
  virtual void write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data) override {}
  virtual void invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, const CMMetadataBase *meta, const CMDataBase *data) override {}

  virtual void start() override  { active = true;}
  virtual void stop() override   { active = false;}
  virtual void pause() override  { active = false;}
  virtual void resume() override { active = true;}
  virtual void reset() override  { active = true;}
};

class EvictDistMonitor : public MonitorBase
{
  static constexpr char reuse_ev_dist_log[]  = "reuse-ev-dist.log";
  static constexpr char use_ev_dist_log[]    = "use-ev-dist.log";
  static constexpr char summary_log[]        = "summary.csv";
  const uint64_t hist_len, period;
  const int npar, nset;
  uint64_t *clock;
  uint64_t clock_start;
  std::time_t time_start;
  std::vector<uint64_t> *core_cycle;
  std::vector<uint64_t> core_cycle_start;
  std::vector<HistoryRecorder<EVRecord>* > insert_db;
  std::vector<HistoryRecorder<EVRecord>* > evict_db;
  AddrHistoryRecorder                     *global_reuse_db;
  std::deque<EVRecord>                     use_dist_db; // use distance: distance from insert to evict
  std::deque<EVRecord>                     reuse_dist_db; // reuse distance: distance from evict to re-insert
  std::vector<uint64_t>                    acc_db, ev_db;
  uint64_t  acc_cnt = 0,  miss_cnt = 0,  ev_cnt = 0, reuse_cnt = 0, reloc_cnt = 0, remap_cnt = 0;
  uint64_t prev_acc = 0, prev_miss = 0, prev_ev = 0,                prev_reloc = 0, prev_remap = 0;
  uint64_t conflict_cnt = 0, prev_conflict = 0, attack_cnt = 0, prev_attack = 0;
  uint64_t pred_hit_cnt = 0;
  uint32_t conflict_sets = 0;
  bool active = false, attached = false;
  SimpleAccMonitor *mem_pfc;

  void log_start();
  void housekeep();
  void print_summary();

public:
  EvictDistMonitor(int NP, int IW, int HL, uint64_t period, uint64_t *clock, std::vector<uint64_t> *core_cycle, SimpleAccMonitor *mem_pfc = nullptr)
    : MonitorBase(),
      hist_len(HL), period(period), npar(NP), nset(1<<IW), clock(clock), core_cycle(core_cycle),
      insert_db(NP*(1 << IW)), evict_db(NP*(1 << IW)), acc_db(NP*(1 << IW), 0), ev_db(NP*(1 << IW), 0),
      mem_pfc(mem_pfc)
  {
    for(int i=0; i<npar*nset; i++) {
      insert_db[i] = new HistoryRecorder<EVRecord>(HL);
      evict_db[i]  = new HistoryRecorder<EVRecord>(HL);
    }
    global_reuse_db = new AddrHistoryRecorder(HL * npar * nset);
  }

  virtual ~EvictDistMonitor() override {
    for(auto &db : insert_db) delete db;
    for(auto &db : evict_db)  delete db;
    delete global_reuse_db;
  }

  virtual bool attach(uint64_t cache_id) override;
  //void bind(std::function<void()> decay_func) { decay_hook = decay_func; }

  virtual void read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data) override;
  virtual void write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data) override;
  virtual void invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, const CMMetadataBase *meta, const CMDataBase *data) override;
  virtual bool magic_func(uint64_t cache_id, uint64_t addr, uint64_t magic_id, void *magic_data) override;

  virtual void start() override  { active = true; log_start(); }
  virtual void stop() override   { active = false; print_summary();}
  virtual void pause() override  { active = false; }
  virtual void resume() override { active = true; }
  virtual void reset() override  { active = true; log_start(); }
  uint64_t get_reloc();
  uint64_t get_acc();
};


#endif
