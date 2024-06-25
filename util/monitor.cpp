#include "util/monitor.hpp"
#include "util/random.hpp"
#include <iostream>
#include <boost/format.hpp>
#include "cache/metadata.hpp"

void SimpleTracer::print(std::string& msg) {
  std::cout << msg << std::endl;
}

void SimpleTracer::read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data) {
  std::string msg;  msg.reserve(100);
  msg += (boost::format("%-10s read  %016x %02d %04d %02d %1x") % UniqueID::name(cache_id) % addr % ai % s % w % hit).str();

  if(meta)
    msg.append(" [").append(meta->to_string()).append("]");
  else if(data)
    msg.append("      ");

  if(data)
    msg.append(" ").append(compact_data ? (boost::format("%016x") % (data->read(0))).str() : data->to_string());

  print(msg);
}

void SimpleTracer::write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data) {
  std::string msg;  msg.reserve(100);
  msg += (boost::format("%-10s write %016x %02d %04d %02d %1x") % UniqueID::name(cache_id) % addr % ai % s % w % hit).str();

  if(meta)
    msg.append(" [").append(meta->to_string()).append("]");
  else if(data)
    msg.append("      ");

  if(data)
    msg.append(" ").append(compact_data ? (boost::format("%016x") % (data->read(0))).str() : data->to_string());

  print(msg);
}

void SimpleTracer::invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, const CMMetadataBase *meta, const CMDataBase *data) {
  std::string msg;  msg.reserve(100);
  msg += (boost::format("%-10s evict %016x %02d %04d %02d  ") % UniqueID::name(cache_id) % addr % ai % s % w).str() ;

  if(meta)
    msg.append(" [").append(meta->to_string()).append("]");
  else if(data)
    msg.append("      ");

  if(data)
    msg.append(" ").append(compact_data ? (boost::format("%016x") % (data->read(0))).str() : data->to_string());

  print(msg);
}
