#ifndef CM_UTIL_LOG_HPP
#define CM_UTIL_LOG_HPP

#define lock_log_write(...) \
    extern FILE *lock_log_fp; \
    fprintf(lock_log_fp, __VA_ARGS__); \
    fflush(lock_log_fp);

#define WAIT_CV(cv, lk, s, status, wait_value, ...) \
    cv->wait(lk, [s, status, wait_value]{ return ((*status)[s] < wait_value); }); \
    extern FILE* lock_log_fp; \
    if(log_enable() && true){ \
      fprintf(lock_log_fp, __VA_ARGS__); \
      fflush(lock_log_fp); \
    } 

#define SET_LOCK(lk, ...) \
    lk.lock(); \
    extern FILE* lock_log_fp; \
    if(log_enable() && true){ \
      fprintf(lock_log_fp, __VA_ARGS__); \
      fflush(lock_log_fp); \
    } 

#define UNSET_LOCK(lk, ...) \
    lk.unlock(); \
    extern FILE* lock_log_fp; \
    if(log_enable()){ \
      fprintf(lock_log_fp, __VA_ARGS__); \
      fflush(lock_log_fp); \
    } 

#define SET_LOCK_PTR(lk, ...) \
    lk->lock(); \
    extern FILE* lock_log_fp; \
    if(log_enable()){ \
      fprintf(lock_log_fp, __VA_ARGS__); \
      fflush(lock_log_fp); \
    } 

#define UNSET_LOCK_PTR(lk, ...) \
    lk->unlock(); \
    extern FILE* lock_log_fp; \
    if(log_enable()) { \
      fprintf(lock_log_fp, __VA_ARGS__); \
      fflush(lock_log_fp); \
    } 


long long get_time();
void close_log();
bool log_enable();

/*

#ifndef CM_UTIL_LOG_HPP
#define CM_UTIL_LOG_HPP

// This file defines some macros to assist the debugger of the simulator in debugging during multi-thread execution.

#define log_write(...)        \
  do { \
    extern FILE *lock_log_fp; \
    if(log_enable()) { \
      fprintf(lock_log_fp, __VA_ARGS__); \
      fflush(lock_log_fp); \
    } \
  } while (0) 

#define lock_write(lk, ...) \
  do { \
    lk.lock(); \
    extern FILE* lock_log_fp; \
    if(log_enable()){ \
      fprintf(lock_log_fp, __VA_ARGS__); \
      fflush(lock_log_fp); \
    } \
  } while(0)

#define lock_ptr_write(lk, ...) \
  do { \
    lk->lock(); \
    extern FILE* lock_log_fp; \
    if(log_enable()){ \
      fprintf(lock_log_fp, __VA_ARGS__); \
      fflush(lock_log_fp); \
    } \
  } while(0)

#define unlock_write(lk, ...) \
  do { \
    lk.unlock(); \
    extern FILE* lock_log_fp; \
    if(log_enable()){ \
      fprintf(lock_log_fp, __VA_ARGS__); \
      fflush(lock_log_fp); \
    } \
  } while(0)

#define unlock_ptr_write(lk, ...) \
  do { \
    lk->unlock(); \
    extern FILE* lock_log_fp; \
    if(log_enable()){ \
      fprintf(lock_log_fp, __VA_ARGS__); \
      fflush(lock_log_fp); \
    } \
  } while(0)

#endif



*/

#endif