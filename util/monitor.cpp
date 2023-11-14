#include "util/monitor.hpp"
#include <iostream>
#include <boost/format.hpp>

static boost::format    read_fmt("read  %016x %1x %04x %02x %1x");
static boost::format   write_fmt("write %016x %1x %04x %02x %1x");
static boost::format invalid_fmt("evict %016x %1x %04x %02x");

void SimpleTracer::read(uint64_t cache_id, uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit) {
  std::cout << (read_fmt % addr % ai % s % w % hit) << std::endl;
}

void SimpleTracer::write(uint64_t cache_id, uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit) {
  std::cout << (write_fmt % addr % ai % s % w % hit) << std::endl;
}

void SimpleTracer::invalid(uint64_t cache_id, uint64_t addr, uint32_t ai, uint32_t s, uint32_t w) {
  std::cout << (invalid_fmt % addr % ai % s % w) << std::endl;
}
