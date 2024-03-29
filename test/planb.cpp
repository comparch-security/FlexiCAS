#include "test/config.hpp"

static void init(bool EnIC){
  xact_queue_empty_mutex_array.resize(NCore);
  xact_queue_full_mutex_array.resize(NCore);
  xact_queue_op_mutex_array.resize(NCore);
  xact_non_full_notify_array.resize(NCore);
  xact_non_empty_notify_array.resize(NCore);
  for(int i = 0; i < NCore; i++){
    xact_queue_empty_mutex_array[i] = std::make_unique<std::mutex>();
    xact_queue_full_mutex_array[i] = std::make_unique<std::mutex>();
    xact_queue_op_mutex_array[i] = std::make_unique<std::mutex>();
    xact_non_full_notify_array[i] = std::make_unique<std::condition_variable>();
    xact_non_empty_notify_array[i] = std::make_unique<std::condition_variable>();
  }
  addr_pool.resize(AddrN);
  iflag.resize(AddrN);
  for(int i=0; i<AddrN; i++){
    auto addr = hasher(gi++) & addr_mask;
    while(addr_map.count(addr)) addr = hasher(gi++) & addr_mask;
    addr_pool[i] = addr;
    addr_map[addr] = i;
    if(EnIC)
      iflag[i] = (0 == (hasher(gi++) & 0x111)); // 12.5% is instruction
    else
      iflag[i] = false;
  }
}

static void remap_pool(bool EnIC){
  addr_map.clear();
  database.clear();
  for(int i = 0; i<AddrN; i++){
    auto addr = hasher(gi++) & addr_mask;
    while(addr_map.count(addr)) addr = hasher(gi++) & addr_mask;
    addr_pool[i] = addr;
    if(EnIC)
      iflag[i] = (0 == (hasher(gi++) & 0x111)); // 12.5% is instruction
    else
      iflag[i] = false;
  }
}

static void xact_queue_add(int core, std::atomic<int>& counter) {
  std::unique_lock queue_full_lock(*(xact_queue_full_mutex_array[core]), std::defer_lock);
  int num = 0;
  cache_xact act;
  act.core = core;
  CMHasher thasher(999+core);
  uint32_t tgi = cm_get_random_uint32(); 
  while(num < (AddrN/NCore)){
    xact_queue_op_mutex_array[core]->lock();
    if(xact_queue[core].size() >= XACT_QUEUE_HIGH){
      xact_queue_op_mutex_array[core]->unlock();
      xact_non_full_notify_array[core]->wait(queue_full_lock);
    }else{
      unsigned int index = thasher(tgi++) % AddrN;
      uint64_t addr = addr_pool[index];
      int ran_num = thasher(tgi++);
      bool rw = (0 == (ran_num & 0x11)); // 25% write
      bool flush = (0 == (ran_num & 0x17)) ? 1 : 0; // 25% of write is flush
      bool is_inst = iflag[index];
      bool ic;
      if(is_inst && rw){
        ic = false;
      } else{
        ic = is_inst ;
        if(flush)   rw = 0; 
      }
#ifdef USE_DATA
    if (rw){
      data_type dt;
      dt.write(0, num, 0xffffffffffffffffull);
      act.data.copy(&dt);
    }
#endif
      act.addr = addr;
      act.ic = ic;
      act.op_t = flush ? CACHE_OP_FLUSH : (rw ? CACHE_OP_WRITE : CACHE_OP_READ);
      xact_queue[core].push_back(act);
      xact_queue_op_mutex_array[core]->unlock();
      num++;
    }
  }
  counter.fetch_add(1, std::memory_order_relaxed);
}

static void cache_server(int core, std::atomic<int>& counter){
  std::unique_lock queue_empty_lock(*(xact_queue_empty_mutex_array[core]), std::defer_lock);
  database.insert_id(get_thread_id);
  int num = 0;
  cache_xact xact;
  while(num < (AddrN/NCore)){
    xact_queue_op_mutex_array[core]->lock();
    bool busy = !(xact_queue[core].empty());
    if(busy){
      xact = xact_queue[core].front();
      xact_queue[core].pop_front();
      num++;
      if(xact_queue.size() <= XACT_QUEUE_LOW)
        xact_non_full_notify_array[core]->notify_all();
    }
    xact_queue_op_mutex_array[core]->unlock(); 
    if(busy){
      switch (xact.op_t) {
      case CACHE_OP_READ:
        if(xact.ic)
          core_inst[core]->read(xact.addr, nullptr);
        else
          core_data[core]->read(xact.addr, nullptr);
        break;
      case CACHE_OP_WRITE:
#ifdef USE_DATA
        core_data[core]->write(xact.addr, &(xact.data), nullptr);
#else
        core_data[core]->write(xact.addr, nullptr, nullptr);
#endif
        break;
      case CACHE_OP_FLUSH:
        if(xact.ic)
          core_inst[core]->flush(xact.addr, nullptr);
        else
          core_data[core]->flush(xact.addr, nullptr);
        break;
      default:
        assert(0 == "unknown op type");
      }
    }
  }
  counter.fetch_add(1, std::memory_order_relaxed);
}


void PlanB(bool flush_cache, bool remap){
  init(true);

  int i = 0;
  double all_time = 0;
  while(i < REPE){
    std::atomic<int> server_counter(0);
    auto start = std::chrono::high_resolution_clock::now();

    // std::atomic<int> counter(0);

    std::vector<std::thread> server_thread;
    std::vector<std::thread> add_thread;

    for(int i = 0; i < NCore; i++){
      add_thread.emplace_back(xact_queue_add, i, std::ref(server_counter));
      server_thread.emplace_back(cache_server, i, std::ref(server_counter));
    }

    while(server_counter.load(std::memory_order_relaxed) < 2*NCore) {}

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> durationB = end - start;

    std::cout << "thread num  : " << NCore << std::endl;
    std::cout << "handle addr : " << AddrN << std::endl;
    std::cout << "cost time   : " << durationB.count() << "s" << std::endl;

    for(auto & t : add_thread){
      t.join();
    }

    for(auto &s : server_thread){
      s.join();
    }
    i++;
    all_time += durationB.count();
    if(flush_cache) {
      for(auto c : core_inst) c->flush_cache(nullptr);
      core_data[0]->flush_cache(nullptr);
    }
    if(remap) remap_pool(true);
  }
  std::cout << "average time : " << (all_time / REPE) << "s" << std::endl;
}