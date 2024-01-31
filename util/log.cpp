#include "util/log.hpp"
#include <time.h>

bool loge = true;

long long get_time(){
  struct timespec currentTime;
  clock_gettime(CLOCK_REALTIME, &currentTime);
  return currentTime.tv_nsec;
}

bool log_enable(){
  return loge;
}

void close_log(){
  loge = false;
}