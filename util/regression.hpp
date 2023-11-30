#ifndef CM_UTIL_REGRESSION_HPP
#define CM_UTIL_REGRESSION_HPP

#include <cstdlib>
#include <vector>
#include <tuple>
#include <cmath>
#include "util/random.hpp"
#include "util/concept_macro.hpp"
#include "cache/metadata.hpp"

static const uint64_t addr_mask = 0x0ffffffffffc0ull;

// NC:     number of core
// EnIC:   whether to have an instruction cache
// PAddrN: number of private addresses per core
// SAddrN: number of shared address between cores
// DT:     data type
template<int NC, bool EnIC, unsigned int PAddrN, unsigned int SAddrN, typename DT>
class RegressionGen
{
  int64_t gi;
  CMHasher hasher;
  const unsigned int total;
  std::vector<uint64_t> addr_pool;   // random addresses
  std::map<uint64_t, int> addr_map;
  std::vector<DT>       data_pool;   // data copy
  std::vector<bool>     wflag;       // whether written
  std::vector<bool>     iflag;       // belong to instruction

public:
  RegressionGen()
    : gi(703), hasher(1201), total(NC*PAddrN+SAddrN)
  {
    addr_pool.resize(total);
    data_pool.resize(total);
    wflag.resize(total);
    iflag.resize(total);
    for(int i=0; i<total; i++) {
      addr_pool[i] = hasher(gi++) & addr_mask;
      addr_map[addr_pool[i]] = i;
      wflag[i] = false;
      if(EnIC)
        iflag[i] = 0 == (hasher(gi++) & 0x111); // 12.5% is instruction
      else
        iflag[i] = false;
    }
  }

  unsigned int locality_scale(unsigned int num, unsigned int mod, double rate) {
    num %= mod;
    double factor = (double)(num) / mod;
    double scale = rate + (1.0 - rate) * std::pow(factor, 3);
    assert(scale >= 0.0 && scale < 1);
    return (unsigned int)(std::floor(num * scale));
  }

  // <addr, data, r/w, core, i/d, flush>
  std::tuple<uint64_t, DT *, bool, int, bool, bool>
  gen() {
    int core = hasher(gi++)%NC;
    bool shared = SAddrN != 0 ? 0 == (hasher(gi++) & 0x111) : false; // 12.5% is shared
    unsigned int index = shared ?
      PAddrN*NC   + locality_scale(hasher(gi++), SAddrN, 0.2) :
      PAddrN*core + locality_scale(hasher(gi++), PAddrN, 0.2) ;
    uint64_t addr = addr_pool[index];
    DT *data = &(data_pool[index]);
    bool rw = 0 == (hasher(gi++) & 0x11); // 25% write
    if(!wflag[index]) rw = true; // always write first
    bool is_inst = iflag[index];
    bool ic, flush;

    if(is_inst && rw) { // write an instruction
      ic = false;       // write by a data cache and flush before write
      flush = true;
    } else {
      ic = is_inst ? 0 != (hasher(gi++) & 0x111) : false;
      flush = false;
    }

    if(rw) {
      data->write(0, hasher(gi++), 0xffffffffffffffffull);
      wflag[index] = true;
    }

    return std::make_tuple(addr, data, rw, core, ic, flush);
  }

  bool check(uint64_t addr, const DT *data) {
    assert(addr_map.count(addr));
    int index = addr_map[addr];
    assert(data_pool[index].read(0) == data->read(0));
    return data_pool[index].read(0) == data->read(0);
  }
};

#endif
