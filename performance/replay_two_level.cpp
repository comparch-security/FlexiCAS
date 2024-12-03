#include "replayer/st_parser.hpp"
#include "replayer/synchro_trace.hpp"
#include "util/cache_type.hpp"
#include "util/delay.hpp"
#include "cache/memory.hpp"
#include "cache/metadata.hpp"
#include "cache/msi.hpp"

#define L1IW 6
#define L1WN 8

#define L2IW 10
#define L2WN 16

int main(int argc, char* argv[]) {

  if (argc != 2) {
    std::cerr << "Usage replay_two_level <trace_dir>" << std::endl;
    return 0;
  }

  char* dir = argv[1];

  using policy_l2 = MSIPolicy<false, true, policy_memory>;
  using policy_l1d = MSIPolicy<true, false, policy_l2>;
  using policy_l1i = MSIPolicy<true, true, policy_l2>;
  using TimePoint = std::chrono::steady_clock::time_point;

  auto l1d = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1d, false, DelayL1<1, 1, 1>, true, true>(NCore, "l1d");
  auto core_data = get_l1_core_interface(l1d);
  auto l1i = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1i, true, DelayL1<1, 1, 1>, true, true>(NCore, "l1i");
  auto core_inst = get_l1_core_interface(l1i);

  auto l2 = cache_gen_inc<L2IW, L2WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l2, true, DelayCoherentCache<2, 3, 4>, true, true>(1, "l2")[0];
  auto mem = new SimpleMemoryModel<void, DelayMemory<10>, true, true>("mem");
  
  for(int i=0; i<NCore; i++) {
    l1i[i]->outer->connect(l2->inner);
    l1d[i]->outer->connect(l2->inner);
  }
  l2->outer->connect(mem);

  SynchroTraceReplayer<NThread, NCore> replayer(dir, 1.0, 2.0, 1, 1, 300, core_data);
  replayer.init();
  replayer.start();

  auto waketime = replayer.get_wakeuptime();

  std::vector<TimePoint> filtered_time_points;
  for (const auto& pair : waketime) {
    if (pair.first) {
      filtered_time_points.push_back(pair.second);
    }
  }
 
  auto max_it = std::max_element(filtered_time_points.begin(), filtered_time_points.end());
  TimePoint max_value = *max_it;
  filtered_time_points.erase(max_it);

  auto se_max_it = std::max_element(filtered_time_points.begin(), filtered_time_points.end());
  TimePoint se_max_value = *se_max_it;

  auto min_it = std::min_element(filtered_time_points.begin(), filtered_time_points.end());
  TimePoint min_value = *min_it;

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(max_value-se_max_value);

  auto all_duration = std::chrono::duration_cast<std::chrono::milliseconds>(max_value - min_value);

  std::cout << "replay_two_level: " << NThread << " " << NCore << std::endl;
  std::cout << "directory: " << dir << std::endl;
  std::cout << "from all thread starts to end time cost: " << duration.count() << " ms" << std::endl;
  std::cout << "all thread time cost: " << all_duration.count() << " ms" << std::endl;
}