#include "rsa/monitor.hpp"
#include "util/statistics.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace {
  void calculate_quantile(uint32_t nset,
                          std::deque<EVRecord>& dist_db, std::vector<uint64_t>& result,
                          double& s_ev_avg,  double& s_ev_var, double& divergance)
  {
    static std::vector<uint64_t> s_ev_data;
    static std::vector<uint64_t> s_dist;
    s_dist.resize(nset, 0);
    auto ssize = dist_db.size();
    auto s_ev_handle  = init_mean_stat();

    for(unsigned int i=0; i<ssize; i++) {
      auto &record = dist_db.front();
      s_ev_data.push_back(record.s_ev);   record_mean_stat(s_ev_handle,  (double)(record.s_ev));
      s_dist[record.si]++;
      dist_db.pop_front();
    }

    s_ev_avg  = get_mean_mean(s_ev_handle);  s_ev_var  = std::sqrt(get_mean_variance(s_ev_handle));

    close_mean_stat(s_ev_handle);

    std::vector<double> points = {0.00, 0.01, 0.05, 0.10, 0.25, 0.50, 0.75, 1.00};
    shape_distribution(points, result, s_ev_data);
    s_ev_data.clear();

    divergance = kl_divergence_with_uniform(s_dist);
    s_dist.clear();
  }
}

bool EvictDistMonitor::attach(uint64_t cache_id) {
  assert(!attached || 0 == "Only one cache can be attached to each EvictDistMonitor!");
  attached = true;
  return true;
}

void EvictDistMonitor::read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data) {
  if(!active) return;
  int index = ai*npar + s;
  acc_cnt++; acc_db[index]++;
  auto [use_hit, use_record] = insert_db[index]->count(addr);
  if(use_hit) {
    *use_record = {acc_cnt, ev_cnt, acc_db[index], ev_db[index]};
  } else {
    insert_db[index]->insert(addr) = {acc_cnt, ev_cnt, acc_db[index], ev_db[index]};
  }
  if(!hit) {
    auto [reuse_hit, reuse_record] = evict_db[index]->count(addr);
    if(reuse_hit)
      reuse_dist_db.emplace_back(acc_cnt       - reuse_record->c_acc, ev_cnt       - reuse_record->c_ev,
                                 acc_db[index] - reuse_record->s_acc, ev_db[index] - reuse_record->s_ev,
                                 index);
    if(global_reuse_db->count(addr).first) reuse_cnt++;
    miss_cnt++;
  }
  housekeep();
}

void EvictDistMonitor::write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, bool hit, const CMMetadataBase *meta, const CMDataBase *data) {
  int index = ai*npar + s;
  auto [use_hit, use_record] = insert_db[index]->count(addr);
  if(use_hit) {
    *use_record = {acc_cnt, ev_cnt, acc_db[index], ev_db[index]};
  } else {
    insert_db[index]->insert(addr) = {acc_cnt, ev_cnt, acc_db[index], ev_db[index]};
  }
}

void EvictDistMonitor::invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, int32_t ev_rank, const CMMetadataBase *meta, const CMDataBase *data) {
  if(!active) return;
  int index = ai*npar + s;
  ev_cnt++; ev_db[index]++;
  evict_db[index]->insert(addr) = {acc_cnt, ev_cnt, acc_db[index], ev_db[index]};
  global_reuse_db->insert(addr);
  // auto [use_hit, use_record] = insert_db[index]->count(addr);
  // if(use_hit)
  //   use_dist_db.emplace_back(acc_cnt       - use_record->c_acc, ev_cnt       - use_record->c_ev,
  //                            acc_db[index] - use_record->s_acc, ev_db[index] - use_record->s_ev,
  //                            index);
}

bool EvictDistMonitor::magic_func(uint64_t cache_id, uint64_t addr, uint64_t magic_id, void *magic_data) {
  if(!active) return false;
  if(magic_id == EvictDistRelocEvent)    {
    reloc_cnt++;
    // record use distance for relocation
    uint32_t s = *(static_cast<uint32_t *>(magic_data));
    auto [use_hit, use_record] = insert_db[s]->count(addr);
    if(use_hit) {
      *use_record = {acc_cnt, ev_cnt, acc_db[s], ev_db[s]};
    } else {
      insert_db[s]->insert(addr) = {acc_cnt, ev_cnt, acc_db[s], ev_db[s]};
    }
    return true; }
  if(magic_id == EvictDistAttackEvent)   { attack_cnt++; return false; }
  if(magic_id == EvictDistConflictEvent) { conflict_cnt++; conflict_sets = *(static_cast<uint32_t *>(magic_data)); return true; }
  if(magic_id == EvictDistPredHitEvent)  { pred_hit_cnt++; return true; }
  if(magic_id == EvictDistEvictRank)     {
    auto [s, w, rank] = *(static_cast<std::tuple<uint32_t, uint32_t, uint32_t> *>(magic_data));
    auto [use_hit, use_record] = insert_db[s]->count(addr);
    assert(rank < 16);
    if(use_hit)
      use_dist_db.emplace_back(acc_cnt   - use_record->c_acc, ev_cnt - use_record->c_ev,
                               acc_db[s] - use_record->s_acc, rank,
                               s);
    return true;
  }
  if(magic_id == MAGIC_ID_REMAP_END) { remap_cnt++; return false; }
  return false;
}

void EvictDistMonitor::housekeep() {
  if((acc_cnt - prev_acc) < period) return;

  std::vector<uint64_t> percentile(8, 0);
  double s_ev_avg, s_ev_var, divergance;

  auto miss_num = miss_cnt - prev_miss;
  auto ev_num = ev_cnt - prev_ev;
  auto reuse_num = reuse_cnt;
  auto reloc_num = reloc_cnt - prev_reloc;
  auto remap_num = remap_cnt - prev_remap;
  double miss_rate = (double)(miss_num)/period;
  auto attack_num = attack_cnt - prev_attack;
  //auto conflict_num = conflict_cnt - prev_conflict;
  prev_acc = acc_cnt; prev_miss = miss_cnt; prev_ev = ev_cnt; reuse_cnt = 0; prev_reloc = reloc_cnt; prev_remap = remap_cnt;
  prev_attack = attack_cnt; prev_conflict = conflict_cnt;
  auto clock_now = *clock - clock_start;

  std::ofstream reuse_dist_ev_file((prefix.empty() ? std::string() : prefix+"-")+reuse_ev_dist_log, std::ios_base::app);
  reuse_dist_ev_file << (boost::format("%12d: ") % clock_now).str();
  reuse_dist_ev_file << (boost::format("%9.4f, %9.4f, %10d, %10d, %10d, %10d, %10d, %10d, ") % miss_rate % ((double)(conflict_sets)/(npar*nset)) % miss_num % ev_num % reuse_num % reloc_num % remap_num % attack_num).str();
  if(!reuse_dist_db.empty()) {
    auto sample_size = reuse_dist_db.size();
    calculate_quantile(npar*nset, reuse_dist_db, percentile, s_ev_avg, s_ev_var, divergance);
    reuse_dist_ev_file << (boost::format("%10d, %10.2f, %10.2f, %10.4f, ") % sample_size % s_ev_avg % s_ev_var % divergance).str();
    reuse_dist_ev_file << (boost::format("%6d, %6d, %6d, %6d, %6d, %6d, %6d, %6d")
                           % percentile[0] % percentile[1] % percentile[2] % percentile[3] % percentile[4] % percentile[5] % percentile[6] % percentile[7]).str();
  }
  reuse_dist_ev_file << std::endl;
  reuse_dist_ev_file.close();

  std::ofstream use_dist_ev_file((prefix.empty() ? std::string() : prefix+"-")+use_ev_dist_log, std::ios_base::app);
  use_dist_ev_file << (boost::format("%12d: ") % clock_now).str();
  use_dist_ev_file << (boost::format("%9.4f, %9.4f, %10d, %10d, %10d, %10d, %10d, %10d, ") % miss_rate % ((double)(conflict_sets)/(npar*nset)) % miss_num % ev_num % reuse_num % reloc_num % remap_num % attack_num).str();
  if(!use_dist_db.empty()) {
    auto sample_size = use_dist_db.size();
    calculate_quantile(npar*nset, use_dist_db, percentile, s_ev_avg, s_ev_var, divergance);
    use_dist_ev_file << (boost::format("%10d, %10.2f, %10.2f, %10.4f, ") % sample_size % s_ev_avg % s_ev_var % divergance).str();
    use_dist_ev_file << (boost::format("%6d, %6d, %6d, %6d, %6d, %6d, %6d, %6d")
                         % percentile[0] % percentile[1] % percentile[2] % percentile[3] % percentile[4] % percentile[5] % percentile[6] % percentile[7]).str();
  }
  use_dist_ev_file << std::endl;
  use_dist_ev_file.close();
}

void EvictDistMonitor::log_start() {
  std::string header("#       time, miss-rate, high-rate,       miss,   eviction,      reuse,      reloc,      remap,     attack, s-dist-num, s-dist-avg, s-dist-var, divergance,     0%,     1%,     5%,    10%,    25%,    50%,    75%,   100%");
  std::ofstream reuse_ev_dist_file((prefix.empty() ? std::string() : prefix+"-")+reuse_ev_dist_log);
  reuse_ev_dist_file << header << std::endl;
  reuse_ev_dist_file.close();
  std::ofstream use_ev_dist_file((prefix.empty() ? std::string() : prefix+"-")+use_ev_dist_log);
  use_ev_dist_file << header << std::endl;
  use_ev_dist_file.close();

  for(auto db: insert_db) db->clear();
  for(auto db: evict_db)  db->clear();
  global_reuse_db->clear();
  use_dist_db.clear();
  reuse_dist_db.clear();
  acc_db.clear(); acc_db.resize(npar*nset, 0);
  ev_db.clear(); ev_db.resize(npar*nset, 0);
  acc_cnt = 0; prev_acc = 0;
  miss_cnt = 0; prev_miss = 0;
  ev_cnt  = 0; prev_ev  = 0;
  reuse_cnt = 0;
  reloc_cnt = 0; prev_reloc = 0;
  remap_cnt = 0; prev_remap = 0;
  conflict_cnt = 0; prev_conflict = 0;
  conflict_sets = 0;
  attack_cnt = 0; prev_attack = 0;
  pred_hit_cnt = 0;
  clock_start = *clock;
  core_cycle_start = *core_cycle;
  time_start = std::time(nullptr);
  if(mem_pfc) { mem_pfc->reset(); mem_pfc->start(); }
}

void EvictDistMonitor::print_summary() {
  // avoid multiple process crash on the same file
  auto lock_file_path = std::filesystem::path("summary.lck");
  while(std::filesystem::exists(lock_file_path)) sleep(1);
  std::ofstream lock_file("summary.lck");
  lock_file << " ";
  lock_file.close();

  std::ofstream summary_file(summary_log, std::ios_base::app);
  if(!prefix.empty()) summary_file << (boost::format("%-18s, ") % prefix).str();
  summary_file << (boost::format("%12d") % (std::time(nullptr) - time_start)).str();
  summary_file << (boost::format(", %12d") % (*clock - clock_start)).str();
  for(unsigned int i=0; i<core_cycle_start.size(); i++) summary_file << (boost::format(", %12d") % ((*core_cycle)[i] - core_cycle_start[i])).str();
  summary_file << (boost::format(", %12d") % acc_cnt).str();
  summary_file << (boost::format(", %12d") % miss_cnt).str();
  summary_file << (boost::format(", %12d") % ev_cnt).str();
  summary_file << (boost::format(", %12d") % reloc_cnt).str();
  summary_file << (boost::format(", %12d") % remap_cnt).str();
  summary_file << (boost::format(", %12d") % pred_hit_cnt).str();
  if(mem_pfc) {
    summary_file << (boost::format(", %12d") % mem_pfc->get_access()).str();
    summary_file << (boost::format(", %12d") % mem_pfc->get_access_read()).str();
    summary_file << (boost::format(", %12d") % mem_pfc->get_access_write()).str();
  }
  summary_file << std::endl;
  summary_file.close();

  std::filesystem::remove(lock_file_path);
}

uint64_t EvictDistMonitor::get_reloc() {
  return reloc_cnt;
}

uint64_t EvictDistMonitor::get_acc() {
  return acc_cnt;
}
