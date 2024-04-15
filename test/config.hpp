#include "cache/memory.hpp"
#include "cache/metadata.hpp"
#include "util/cache_type.hpp"
#include "util/common.hpp"
#include "util/log.hpp"
#include "util/parallel_regression.hpp"
#include "util/random.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <deque>
#include <condition_variable>
#include <thread>


#define NCore 4
#define AddrN 128000

#define NCoreM 4

#define L1IW 4
#define L1WN 4

#define L2IW 5
#define L2WN 8

#define L3IW 5
#define L3WN 16

#define REPE 20

#define XACT_QUEUE_HIGH    100
#define XACT_QUEUE_LOW     10

#define CACHE_OP_READ        0
#define CACHE_OP_WRITE       1
#define CACHE_OP_FLUSH       2

#define USE_DATA

#ifdef USE_DATA
typedef Data64B data_type; 
#else
typedef void data_type;
#endif

extern FILE *lock_log_fp;
extern std::vector<std::unique_ptr<std::mutex>> xact_queue_op_mutex_array, xact_queue_full_mutex_array, xact_queue_empty_mutex_array;
extern std::vector<std::unique_ptr<std::condition_variable>> xact_non_empty_notify_array, xact_non_full_notify_array;
extern std::vector<std::deque<cache_xact>> xact_queue;
extern std::vector<uint64_t>    addr_pool; 
extern std::unordered_map<uint64_t, int> addr_map;
// extern std::vector<DTContainer<NCore,data_type>* >  data_pool;   
extern std::vector<bool>        iflag;       // belong to instruction
extern int64_t gi;
extern CMHasher hasher;
extern std::vector<CoreInterface *> core_data, core_inst;
extern SimpleMemoryModel<data_type,void,false>* mem;