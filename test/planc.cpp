#include "test/config.hpp"

static std::vector<cache_xact<data_type>> xact_buffer(NCore);
static std::condition_variable cv;
static uint64_t buffer_record;
static std::mutex mtx;


static void init(bool EnIC){
  addr_pool.resize(AddrN);
  iflag.resize(AddrN);
  data_pool.resize(AddrN);
  for(int i=0; i<AddrN; i++){
    auto addr = hasher(gi++) & addr_mask;
    while(addr_map.count(addr)) addr = hasher(gi++) & addr_mask;
    addr_pool[i] = addr;
    addr_map[addr] = i;
    data_pool[i] = new DTContainer<NCore, data_type>();
    data_pool[i]->init(addr);
    if(EnIC)
      iflag[i] = (0 == hasher(gi++) & 0x111); // 12.5% is instruction
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
    data_pool[i]->init(addr);
    if(EnIC)
      iflag[i] = (0 == hasher(gi++) & 0x111); // 12.5% is instruction
    else
      iflag[i] = false;
  }
}

static void xact_queue_add(std::atomic<int>& counter) {
  cache_xact<data_type> act;
  CMHasher thasher(999);
  uint32_t tgi = cm_get_random_uint32();
  std::unique_lock lk(mtx, std::defer_lock);
  for(int i = 0; i < AddrN/NCore; i++){
    lk.lock();
    cv.wait(lk, [] { return buffer_record == 0;});
    for(int i = 0; i < NCore; i++){
      unsigned int index = thasher(tgi++) % AddrN;
      uint64_t addr = addr_pool[index];
      auto d = data_pool[index];
      data_type dt;
      int ran_num = thasher(tgi++);
      bool rw = (0 == (ran_num & 0x11)); // 25% write
      bool flush = (0 == (ran_num & 0x17)) ? 1 : 0; // 25% of write is flush
      bool is_inst = iflag[index];
      bool ic;
      if(is_inst && rw){
        ic = false;
        // flush = 1;
      } else{
        ic = is_inst ;
        if(is_inst) flush = 0; // read instruction
        if(flush)   rw = 0; 
      }
      if (!C_VOID(data_type) && rw){
        dt.write(0, i, 0xffffffffffffffffull);
        d->write(&dt);
        act.data.copy(&dt);
      }
      act.addr = addr;
      act.ic = ic;
      act.op_t = flush ? CACHE_OP_FLUSH : (rw ? CACHE_OP_WRITE : CACHE_OP_READ);
      xact_buffer[i] = act;
      buffer_record |= (1 << i);
    }
    lk.unlock();
    cv.notify_all();
  }
  lk.lock();
  cv.wait(lk, [] {return buffer_record == 0;});
  for(int i = 0; i <= NCore; i++) buffer_record |= (1 << i);
  lk.unlock();
  cv.notify_all();
  counter.fetch_add(1, std::memory_order_relaxed);
}

static void cache_server(int core, std::atomic<int>& counter){
  std::unique_lock lk(mtx, std::defer_lock);
  database.insert_id(get_thread_id);
  
  while(true){
    lk.lock();
    cv.wait(lk, [core]{ return (buffer_record & (1 << core)) == (1 << core);});
    lk.unlock();

    if((buffer_record & (1 << NCore)) == (1 << NCore)){
      break;
    }
    cache_xact<data_type> xact = xact_buffer[core];

    switch (xact.op_t) {
    case CACHE_OP_READ:
      if(xact.ic)
        core_inst[core]->read(xact.addr, nullptr);
      else
        core_data[core]->read(xact.addr, nullptr);
      break;
    case CACHE_OP_WRITE:
      core_data[core]->write(xact.addr, &(xact.data), nullptr);
      break;
    case CACHE_OP_FLUSH:
      core_data[core]->flush(xact.addr, nullptr);
      break;
    default:
      printf("op_t is %d\n", xact.op_t);
      assert(0 == "unknown op type");
    }
    lk.lock();
    buffer_record &= ~(1 << core);
    lk.unlock();

    cv.notify_all();
  }
  counter.fetch_add(1, std::memory_order_relaxed);
}


void PlanC(bool flush_cache, bool remap){
  init(true);

  int i = 0;
  double all_time = 0;
  while(i < REPE){
    std::atomic<int> server_counter(0);
    auto start = std::chrono::high_resolution_clock::now();

    std::thread add_thread(xact_queue_add, std::ref(server_counter));

    std::vector<std::thread> server_thread;

    for(int i = 0; i < NCore; i++){
      server_thread.emplace_back(cache_server, i, std::ref(server_counter));
    }

    while(server_counter.load(std::memory_order_relaxed) < NCore+1) {}

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> durationB = end - start;

    std::cout << "thread num  : " << NCore << std::endl;
    std::cout << "handle addr : " << AddrN << std::endl;
    std::cout << "cost time   : " << durationB.count() << "s" << std::endl;

    add_thread.join();

    for(auto &s : server_thread){
      s.join();
    }
    i++;
    all_time += durationB.count();
    buffer_record = 0;
    if(flush_cache) core_data[0]->flush_cache(nullptr);
    if(remap) remap_pool(true);
  }
  std::cout << "average time : " << (all_time / REPE) << "s" << std::endl;
}