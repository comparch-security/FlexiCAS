#ifndef CM_SPIKE_FLEXICAS_HPP
#define CM_SPIKE_FLEXICAS_HPP

// the header file expose to spike

namespace flexicas {
  extern int  ncore();
  extern int  cache_level();
  extern int  cache_set(int level, bool ic);
  extern int  cache_way(int level, bool ic);
  extern void init();
  extern void read(uint64_t addr, int core, bool ic);
  extern void write(uint64_t addr, int core);
  extern void flush(uint64_t addr, int core);
  extern void writeback(uint64_t addr, int core);
}

#endif
