#include "util/monitor.hpp"
#include "util/random.hpp"
#include <iostream>
#include <boost/format.hpp>
#include "cache/metadata.hpp"

static boost::format    read_fmt("%-10s read  %016x %02d %04d %02d %1x");
static boost::format   write_fmt("%-10s write %016x %02d %04d %02d %1x");
static boost::format invalid_fmt("%-10s evict %016x %02d %04d %02d  ");
static boost::format    data_fmt("%016x");

void SimpleTracer::print(const std::string& msg) {
  std::cout << msg << std::endl;
}

void SimpleTracer::read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data) {
  std::string msg;  msg.reserve(100);
  msg += (read_fmt % UniqueID::name(cache_id) % addr % ai % s % w % hit).str();

  if(meta)
    msg.append(" [").append(meta->to_string()).append("]");
  else if(data)
    msg.append("      ");

  if(data)
    msg.append(" ").append(compact_data ? (data_fmt % (data->read(0))).str() : data->to_string());

  print(msg);
}

void SimpleTracer::write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data) {
  std::string msg;  msg.reserve(100);
  msg += (write_fmt % UniqueID::name(cache_id) % addr % ai % s % w % hit).str();

  if(meta)
    msg.append(" [").append(meta->to_string()).append("]");
  else if(data)
    msg.append("      ");

  if(data)
    msg.append(" ").append(compact_data ? (data_fmt % (data->read(0))).str() : data->to_string());

  print(msg);
}

void SimpleTracer::invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, const CMMetadataBase *meta, const CMDataBase *data) {
  std::string msg;  msg.reserve(100);
  msg += (invalid_fmt % UniqueID::name(cache_id) % addr % ai % s % w).str() ;

  if(meta)
    msg.append(" [").append(meta->to_string()).append("]");
  else if(data)
    msg.append("      ");

  if(data)
    msg.append(" ").append(compact_data ? (data_fmt % (data->read(0))).str() : data->to_string());

  print(msg);
}
