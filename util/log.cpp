#include "util/log.hpp"
#include <chrono>
#include <ctime>

bool loge = true;

long long get_time(){
  auto now = std::chrono::system_clock::now();
  return now.time_since_epoch().count();
}

bool log_enable(){
  return loge;
}

void close_log(){
  loge = false;
}