#include "util/monitor.hpp"
#include "util/random.hpp"
#include <iostream>
#include <boost/format.hpp>
#include "cache/metadata.hpp"

static boost::format    read_fmt("%-10s read  %016x %02d %04d %02d %1x");
static boost::format   write_fmt("%-10s write %016x %02d %04d %02d %1x");
static boost::format invalid_fmt("%-10s evict %016x %02d %04d %02d  ");
static boost::format    data_fmt("%016x");

void SimpleTracer::read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data) {
  std::cout << (read_fmt % UniqueID::name(cache_id) % addr % ai % s % w % hit);

  if(meta)
    std::cout << " [" << meta->to_string() << "]";
  else if(data)
    std::cout << "      ";

  if(data) {
    std::cout << " ";
    if(compact_data) std::cout << (data_fmt % (data->read(0)));
    else             std::cout << data->to_string();
  }
  std::cout << std::endl;
}

void SimpleTracer::write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data) {
  std::cout << (write_fmt % UniqueID::name(cache_id) % addr % ai % s % w % hit);

  if(meta)
    std::cout << " [" << meta->to_string() << "]";
  else if(data)
    std::cout << "      ";

  if(data) {
    std::cout << " ";
    if(compact_data) std::cout << (data_fmt % (data->read(0)));
    else             std::cout << data->to_string();
  }
  std::cout << std::endl;
}

void SimpleTracer::invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, const CMMetadataBase *meta, const CMDataBase *data) {
  std::cout << (invalid_fmt % UniqueID::name(cache_id) % addr % ai % s % w) ;

  if(meta)
    std::cout << " [" << meta->to_string() << "]";
  else if(data)
    std::cout << "      ";

  if(data) {
    std::cout << " ";
    if(compact_data) std::cout << (data_fmt % (data->read(0)));
    else             std::cout << data->to_string();
  }
  std::cout << std::endl;
}
