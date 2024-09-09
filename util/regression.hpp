#ifndef CM_UTIL_REGRESSION_HPP
#define CM_UTIL_REGRESSION_HPP

#include <cstdlib>
#include <vector>
#include <tuple>
#include <cmath>
#include <unordered_map>
#include <iostream>
#include "util/random.hpp"
#include "util/concept_macro.hpp"
#include "cache/metadata.hpp"

static const uint64_t addr_mask = 0x0ffffffffffc0ull;

// NC:        number of core
// EnIC:      whether to have an instruction cache
// TestFlush: whether to generate data flush operations
// PAddrN:    number of private addresses per core
// SAddrN:    number of shared address between cores
// DT:        data type
template<int NC, bool EnIC, bool TestFlush, unsigned int PAddrN, unsigned int SAddrN, typename DT>
class RegressionGen
{
protected:
  int64_t gi;
  CMHasher hasher;
  const unsigned int total;
  std::vector<uint64_t> addr_pool;   // random addresses
  std::unordered_map<uint64_t, int> addr_map;
  std::vector<DT* >       data_pool;   // data copy
  std::vector<bool>     wflag;       // whether written
  std::vector<bool>     iflag;       // belong to instruction

public:
  RegressionGen()
    : gi(703), hasher(1201), total(NC*PAddrN+SAddrN)
  {
    addr_pool.resize(total);
    if constexpr (!C_VOID<DT>) {
      data_pool.resize(total);
      for(auto &d:data_pool) d = new DT();
    }
    wflag.resize(total);
    iflag.resize(total);
    for(unsigned int i=0; i<total; i++) {
      auto addr = hasher(gi++) & addr_mask;
      while(addr_map.count(addr)) addr = hasher(gi++) & addr_mask;
      addr_pool[i] = addr;
      addr_map[addr_pool[i]] = i;
      wflag[i] = false;
      if(EnIC)
        iflag[i] = 0 == (hasher(gi++) & 0x111); // 12.5% is instruction
      else
        iflag[i] = false;
    }
  }

  ~RegressionGen(){
    if constexpr (!C_VOID<DT>) for(auto d:data_pool) delete d; 
  }

  unsigned int locality_scale(unsigned int num, unsigned int mod, double rate) {
    num %= mod;
    double factor = (double)(num) / mod;
    double scale = rate + (1.0 - rate) * std::pow(factor, 3);
    assert(scale >= 0.0 && scale < 1);
    return (unsigned int)(std::floor(num * scale));
  }

  // <addr, data, r/w, core, i/d, flush>
  std::tuple<uint64_t, DT *, bool, int, bool, int>
  gen() {
    int core = hasher(gi++)%NC;
    bool shared = SAddrN != 0 ? 0 == (hasher(gi++) & 0x111) : false; // 12.5% is shared
    unsigned int index = shared ?
      PAddrN*NC   + locality_scale(hasher(gi++), SAddrN, 0.2) :
      PAddrN*core + locality_scale(hasher(gi++), PAddrN, 0.2) ;
    uint64_t addr = addr_pool[index];
    DT *data;
    if constexpr (!C_VOID<DT>) data = data_pool[index];
    auto ran_num = hasher(gi++);
    bool rw = 0 == (ran_num & 0x11); // 25% write
    int flush = TestFlush && (0 == (ran_num & 0x17)) ? 3 : 0; // 25% of write is flush
    if(!wflag[index]) {rw = true; flush = 0;} // always write first
    bool is_inst = iflag[index];
    bool ic;

    if(is_inst && rw) { // write an instruction
      ic = false;       // write by a data cache and flush before write
      flush = shared ? 2 : 1;
    } else {
      ic = is_inst ? 0 != (hasher(gi++) & 0x111) : false;
      if(is_inst) flush = 0;
      if(flush) rw = 0;
    }

    if(rw) {
      if constexpr (!C_VOID<DT>) data->write(0, hasher(gi++), 0xffffffffffffffffull);
      wflag[index] = true;
    }

    return std::make_tuple(addr, data, rw, core, ic, flush);
  }

  bool check(uint64_t addr, const CMDataBase *data) {
    assert(addr_map.count(addr));
    if constexpr (!C_VOID<DT>) {
      int index = addr_map[addr];
      assert(data_pool[index]->read(0) == data->read(0));
      return data_pool[index]->read(0) == data->read(0);
    } else
      return true;
  }

  bool run(uint64_t TestN, std::vector<CoreInterfaceBase *>& core_inst, std::vector<CoreInterfaceBase *>& core_data) {
    for(unsigned int i=0; i<TestN; i++) {
      auto [addr, wdata, rw, nc, ic, flush] = gen();
      if(flush) {
        if(flush == 3)       core_data[nc]->flush(addr, nullptr);
        else if(flush == 2)  for( auto ci:core_inst) ci->flush(addr, nullptr); // shared instruction, flush all cores
        else                 core_inst[nc]->flush(addr, nullptr);

        if(rw)               core_data[nc]->write(addr, wdata, nullptr);
      } else if(rw) {
        core_data[nc]->write(addr, wdata, nullptr);
      } else {
        auto rdata = ic ? core_inst[nc]->read(addr, nullptr) : core_data[nc]->read(addr, nullptr);
        if(!check(addr, rdata)) return 1; // test failed!
      }
    }
    return 0;
  }
};

inline void delete_caches(std::vector<CoherentCacheBase *> &caches) {
  for(auto c : caches) delete c;
}

#endif
