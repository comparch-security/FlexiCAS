#ifndef CM_REPLAYER_THREAD_HPP
#define CM_REPLAYER_THREAD_HPP

#include "replayer/st_event.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <queue>

enum class ThreadStatus {
  INACTIVE,
  ACTIVE,
  ACTIVE_TRYLOCK,
  WAIT_LOCK,
  WAIT_COMM,
  WAIT_COMPUTE,
  WAIT_MEMORY,
  WAIT_THREAD,
  WAIT_SCHED,
  BLOCKED_MUTEX,
  BLOCKED_BARRIER,
  BLOCKED_COND,
  BLOCKED_JOIN,
  COMPLETED,
  NUM_STATUSES
};

const char* toString(ThreadStatus status) {
  switch (status) 
  {
    case ThreadStatus::INACTIVE:
      return "INACTIVE";
    case ThreadStatus::ACTIVE:
       return "ACTIVE";
    case ThreadStatus::ACTIVE_TRYLOCK:
      return "ACTIVE_TRYLOCK";
    case ThreadStatus::WAIT_LOCK:
      return "WATI_LOCK";
    case ThreadStatus::WAIT_THREAD:
      return "WAIT_THREAD";
    case ThreadStatus::WAIT_MEMORY:
      return "WAIT_MEMORY";
    case ThreadStatus::WAIT_COMPUTE:
      return "WAIT_COMPUTE";
    case ThreadStatus::WAIT_SCHED:
      return "WAIT_SCHED";
    case ThreadStatus::WAIT_COMM:
      return "WAIT_COMM";
    case ThreadStatus::BLOCKED_MUTEX:
      return "BLOCKED_MUTEX";
    case ThreadStatus::BLOCKED_BARRIER:
      return "BLOCKED_BARRIER";
    case ThreadStatus::BLOCKED_COND:
      return "BLOCKED_COND";
    case ThreadStatus::BLOCKED_JOIN:
      return "BLOCKED_JOIN";
    case ThreadStatus::COMPLETED:
      return "COMPLETED";
    default:
      std::cerr << "Unexpected Thread Status" << std::endl;
      assert(0);
  }
}

struct ThreadContext
{
  ThreadID threadId;
  StEventID currEventId;
  StEventStream evStream;
  ThreadStatus status;
  uint64_t wakeupClock;
  uint64_t restSliceCycles;

  ThreadContext(ThreadID threadId, const std::string& eventDir)
        : threadId{threadId}, currEventId{0}, 
            evStream{(ThreadID)(threadId+1), eventDir},
            status{ThreadStatus::INACTIVE},
            wakeupClock(0),
            restSliceCycles(0)
        {}

  // Note that a 'running' thread may be:
  // - deadlocked
  //
  bool running() const { return status > ThreadStatus::INACTIVE && status < ThreadStatus::BLOCKED_MUTEX; }

  bool blocked() const { return status > ThreadStatus::WAIT_SCHED && status < ThreadStatus::COMPLETED; }

  bool completed() const { return status == ThreadStatus::COMPLETED; }
};

class ThreadSchedulerBase
{
protected:

  /** clock */
  uint64_t clock;

  /** Holds how many threads are runnning on each core */
  std::vector<int> workerThreadCount;
  
  std::vector<std::deque<std::reference_wrapper<ThreadContext>>>
        coreToThreadMap;

  virtual CoreID threadIdToCoreId(ThreadID threadId) const = 0;

public:

  /** Abstract cpi estimation for integer ops */
  const float CPI_IOPS;

  /** Abstract cpi estimation for floating point ops */
  const float CPI_FLOPS;

  /** Cycles for context switch on a core */
  const uint32_t cxtSwitchCycles;

  /** Cycles for pthread event */
  const uint32_t pthCycles;

  /** Cycles to simulate the time slice the scheduler gives to a thread */
  const uint32_t schedSliceCycles;
  
  ThreadSchedulerBase(float CPI_IOPS, float CPI_FLOPS, uint32_t cxtSwitchCycles, uint32_t pthCycles, uint32_t schedSliceCycles) :
   CPI_IOPS(CPI_IOPS), CPI_FLOPS(CPI_FLOPS), cxtSwitchCycles(cxtSwitchCycles), pthCycles(pthCycles),  
   schedSliceCycles(schedSliceCycles), clock(0) {

  }

  uint64_t curClock() const { return clock; }
  uint64_t nextClock() { return ++clock; }

  virtual void init(ThreadContext& tcxt) = 0;

  virtual void getReady(ThreadContext& tcxt) = 0;
  virtual void getBlocked(ThreadContext& tcxt, ThreadStatus status) = 0;

  virtual bool tryCxtSwapAndSchedule(CoreID coreId) = 0;
  virtual void schedule(ThreadContext& tcxt, uint64_t cycles) = 0;

  virtual ThreadID findActive(CoreID coreId) = 0;
  virtual bool checkOnCore(ThreadID threadId) = 0;
};

// NT: num threads, NC: num cores
template <ThreadID NT, CoreID NC>
class ThreadScheduler : public ThreadSchedulerBase
{
protected:
  CoreID threadIdToCoreId(ThreadID threadId) const{
    return threadId % NC;
  }

public:
  ThreadScheduler(float CPI_IOPS, float CPI_FLOPS, uint32_t cxtSwitchCycles, 
    uint32_t pthCycles, uint32_t schedSliceCycles)
  : ThreadSchedulerBase(CPI_IOPS, CPI_FLOPS, cxtSwitchCycles, pthCycles, schedSliceCycles) 
  {
    coreToThreadMap.resize(NC); 
    workerThreadCount.resize(NC);

    for (CoreID i = 0; i < NC; i++)
      workerThreadCount[i] = 0;
  }

  virtual void init(ThreadContext& tcxt) {
    coreToThreadMap.at(threadIdToCoreId(tcxt.threadId)).emplace_back(tcxt);
  }

  virtual void getReady(ThreadContext& tcxt) {
    if (tcxt.running())
      return;

    if (tcxt.status == ThreadStatus::BLOCKED_COND || tcxt.status == ThreadStatus::BLOCKED_MUTEX)
      tcxt.status = ThreadStatus::WAIT_LOCK;
    else
      tcxt.status = ThreadStatus::WAIT_SCHED;

    const CoreID coreId = threadIdToCoreId(tcxt.threadId);
    workerThreadCount[coreId]++;

    // If this is the only active thread on this core, try to swap it to the front of the working queue.

    if (workerThreadCount[coreId] == 1) {
      auto& threadsOnCore = coreToThreadMap[coreId];
      assert(threadsOnCore.size() > 0);

      ThreadID tid = tcxt.threadId;
      auto it = std::find_if(threadsOnCore.begin(),
                             threadsOnCore.end(),
                             [tid](const ThreadContext &thread)
                             { return thread.threadId == tid; });
      assert(it != threadsOnCore.end());
      std::rotate(threadsOnCore.begin(), it, threadsOnCore.end());
    }

    schedule(tcxt, pthCycles);
  }

  virtual void getBlocked(ThreadContext& tcxt, ThreadStatus status) {
    if (tcxt.blocked()) 
      return;

    tcxt.status = status;
    CoreID coreId = threadIdToCoreId(tcxt.threadId);
    workerThreadCount[coreId]--;
    (void)tryCxtSwapAndSchedule(coreId);
  }

  virtual bool tryCxtSwapAndSchedule(CoreID coreId)
  {
    auto& threadsOnCore = coreToThreadMap[coreId];
    assert(threadsOnCore.size() > 0);

    auto it = std::find_if(std::next(threadsOnCore.begin()),
                           threadsOnCore.end(),
                           [](const ThreadContext &tcxt)
                           { return tcxt.running(); });

    // if no threads were found that could be swapped
    if (it == threadsOnCore.end())
      return false;

    // else we found a thread to swap.
    // Rotate threads round-robin and schedule the context swap.
    std::rotate(threadsOnCore.begin(), it, threadsOnCore.end());
    ThreadContext &tcxt = threadsOnCore.front().get();
    if (tcxt.status == ThreadStatus::ACTIVE_TRYLOCK || tcxt.status == ThreadStatus::WAIT_LOCK)
      tcxt.status = ThreadStatus::WAIT_LOCK;
    else
      tcxt.status = ThreadStatus::WAIT_SCHED;
    schedule(tcxt, cxtSwitchCycles);
    return true;
  }

  virtual void schedule(ThreadContext& tcxt, uint64_t cycles) {
    assert(tcxt.threadId < NT);

    tcxt.wakeupClock = curClock() + cycles;
    if (tcxt.status != ThreadStatus::WAIT_SCHED && !tcxt.blocked())
      tcxt.restSliceCycles -= cycles;
  }


  virtual ThreadID findActive(CoreID coreId)
  {
    if (workerThreadCount[coreId] == 0)
      return -1;

    ThreadContext& tcxt = coreToThreadMap[coreId].front();

    switch (tcxt.status)
    {
      case ThreadStatus::ACTIVE:
      case ThreadStatus::ACTIVE_TRYLOCK:
        if (tcxt.restSliceCycles <= 0) {                   // if the thread has used up its slice
          tcxt.restSliceCycles = schedSliceCycles;
          if (tryCxtSwapAndSchedule(coreId)) {
            tcxt.status = ThreadStatus::WAIT_SCHED;
            tcxt.wakeupClock = curClock() + schedSliceCycles;
          }
        }
        break;
      case ThreadStatus::WAIT_LOCK:
      case ThreadStatus::WAIT_COMPUTE:
      case ThreadStatus::WAIT_MEMORY:
      case ThreadStatus::WAIT_THREAD:
      case ThreadStatus::WAIT_SCHED:
      case ThreadStatus::WAIT_COMM:
      case ThreadStatus::BLOCKED_MUTEX:
      case ThreadStatus::BLOCKED_BARRIER:
      case ThreadStatus::BLOCKED_COND:
      case ThreadStatus::BLOCKED_JOIN:
      case ThreadStatus::INACTIVE:
      case ThreadStatus::COMPLETED:
        return -1;
      default:
        break;
    }

    return tcxt.threadId;
  }

  virtual bool checkOnCore(ThreadID threadId) {
    CoreID coreId = threadIdToCoreId(threadId);
    return coreToThreadMap[coreId].front().get().threadId == threadId;
  }

};

#endif