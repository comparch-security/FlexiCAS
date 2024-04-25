#ifndef CM_UTIL_PARALLEL_REGRESSION_HPP
#define CM_UTIL_PARALLEL_REGRESSION_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include <queue>
#include "cache/metadata.hpp"
#include "util/regression.hpp"
#include "cache/coherence_multi.hpp"
#include <thread>

class cache_xact 
{
public:
  bool rw;
  int core;
  bool ic;
  int flush;
  uint64_t addr;
  Data64B data;
};

/** 
 * When two threads write an address at the same time, it is impossible to determine which 
 * thread's data was last saved in the cache (depending on who wrote it later), so all possible 
 * data will be saved for checking
 */
class DataQueue
{
protected:
  uint64_t addr;
  std::deque<Data64B> data_deque;
  std::mutex* mtx;
  int NC;
public:
  DataQueue(int NCore, uint64_t waddr) : NC(NCore), addr(waddr) {
    mtx = new std::mutex();
  }
  virtual ~DataQueue() { delete mtx; }

  void write(const CMDataBase* wdata){
    Data64B tdata;
    tdata.copy(wdata);
    std::unique_lock lk(*mtx);
    data_deque.push_back(tdata);
    if(data_deque.size() > NC) data_deque.pop_front();
  }
  bool check(uint64_t caddr, const CMDataBase* data){
    assert(caddr == addr);
    std::unique_lock lk(*mtx);
    if(data_deque.size() == 0) return true;
    for(auto d : data_deque){
      if(d.read(0) == data->read(0)) return true;
    }
    return false;
  }
};

class ParallelRegressionSupport
{
public:
  virtual void xact_queue_add(int test_num) = 0;
  virtual std::pair<bool, cache_xact> get_xact(int core) = 0;
  virtual bool check(uint64_t addr, const CMDataBase *data) = 0;
  virtual void write_dq(uint64_t addr, CMDataBase* data) = 0;
};


template<int NC, bool EnIC, bool TestFlush, unsigned int PAddrN, unsigned int SAddrN, typename DT>
class ParallelRegressionGen : public RegressionGen<NC, EnIC, TestFlush, PAddrN, SAddrN, DT>, public ParallelRegressionSupport
{
  typedef RegressionGen<NC, EnIC, TestFlush, PAddrN, SAddrN, DT> ReT;
protected:
  using ReT::addr_pool;
  using ReT::addr_map;
  using ReT::wflag;
  using ReT::iflag;
  using ReT::total;
  using ReT::hasher;
  using ReT::gi;
  using ReT::gen;

  std::vector<DataQueue* > dq_pool;
  std::vector<std::deque<cache_xact >> xact_queue;
  std::vector<std::mutex *> xact_mutux;
public:
  ParallelRegressionGen() : ReT(), ParallelRegressionSupport() {
    xact_queue.resize(NC);
    dq_pool.resize(addr_pool.size());
    for(int i = 0; i < addr_pool.size(); i++){
      dq_pool[i] = new DataQueue(NC, addr_pool[i]);
    }
    xact_mutux.resize(NC);
    for(int i = 0; i < NC; i++) xact_mutux[i] = new std::mutex();
  }

  virtual ~ParallelRegressionGen() {
    for(auto dq : dq_pool) delete dq;
    for(auto m : xact_mutux) delete m;
  }

  virtual void xact_queue_add(int test_num){
    cache_xact act;
    int num = 0;
    act.core = hasher(gi++) % NC;
    while(num < test_num){
      auto [addr, data, rw, core, ic, flush] = gen();
      int index = addr_map[addr];
      Data64B d;
      d.copy(data);
      act = cache_xact{rw, core, ic, flush, addr, d};
      if(flush == 2){ // share instruction flush
        for(int i = 0; i < NC; i++){
          std::unique_lock lk(*xact_mutux[i]);
          xact_queue[core].push_back(act);
        }
      }else{
        std::unique_lock lk(*xact_mutux[core]);
        xact_queue[core].push_back(act);
      }
      num++;
    }
  }

  virtual void write_dq(uint64_t addr, CMDataBase* data){
    int index = addr_map[addr];
    auto dq = dq_pool[index];
    dq->write(data);
  }

  static void cache_producer(int test_num, ParallelRegressionSupport* pgr){
    pgr->xact_queue_add(test_num);
  }

  virtual std::pair<bool, cache_xact> get_xact(int core){
    std::unique_lock lk(*xact_mutux[core]);
    cache_xact act;
    if(xact_queue[core].empty()) return std::make_pair(false, act);
    else{
      act = xact_queue[core].front();
      xact_queue[core].pop_front();
      return std::make_pair(true, act);
    }
  }

  virtual bool check(uint64_t addr, const CMDataBase *data){
    assert(addr_map.count(addr));
    int index = addr_map[addr];
    assert(dq_pool[index]->check(addr, data));
    return true;
  }

  static void cache_server(int core, ParallelRegressionSupport* prg, std::vector<CoreMultiThreadSupport *>* core_inst, std::vector<CoreMultiThreadSupport *>* core_data, bool* exit)
  {
    while(true){
      auto act = prg->get_xact(core);
      if(*exit && !act.first) break;
      else if(act.first){
        auto [rw, act_core, ic, flush, addr, data] = act.second;
        if(flush){
          if(flush == 3)  (*core_data)[core]->flush(addr, nullptr);
          else            (*core_inst)[core]->flush(addr, nullptr);
          if(rw && act_core == core){
            (*core_data)[core]->write(addr, &data, nullptr);
            prg->write_dq(addr, &data);
          }
        } else if(rw){
          (*core_data)[core]->write(addr, &data, nullptr);
          prg->write_dq(addr, &data);
        }else{
          auto rdata = ic ? (*core_inst)[core]->read(addr, nullptr) : (*core_data)[core]->read(addr, nullptr);
          prg->check(addr, rdata);
        }
      }
    }
  }

  void run(int test_num, std::vector<CoreMultiThreadSupport *>* core_inst, std::vector<CoreMultiThreadSupport *>* core_data){
    std::thread add_thread(cache_producer, test_num, this);
    std::vector<std::thread> server_thread;
    bool exit = false;
    for(int i = 0; i < NC; i++){
      server_thread.emplace_back(cache_server, i, this, core_inst, core_data, &exit);
    }
    add_thread.join();
    exit = true;
    for(auto &s : server_thread){
      s.join();
    }
  } 

};


#endif