#ifndef CM_REPLAYER_THREAD_HPP
#define CM_REPLAYER_THREAD_HPP

#include "replayer/st_event.hpp"
#include "util/multithread.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <queue>
#include <mutex>
#include <vector>

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
    case ThreadStatus::BLOCKED_JOIN:
      return "BLOCKED_JOIN";
    case ThreadStatus::COMPLETED:
      return "COMPLETED";
    default:
      std::cerr << "Unexpected Thread Status" << std::endl;
      assert(0);
      return "Unexpected";
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
  bool active() const { return status > ThreadStatus::INACTIVE && status < ThreadStatus::WAIT_LOCK; }

  bool running() const { return status > ThreadStatus::INACTIVE && status < ThreadStatus::BLOCKED_MUTEX; }

  bool waiting() const { return status > ThreadStatus::ACTIVE_TRYLOCK && status < ThreadStatus::BLOCKED_MUTEX; }

  bool blocked() const { return status > ThreadStatus::WAIT_SCHED && status < ThreadStatus::COMPLETED; }

  bool completed() const { return status == ThreadStatus::COMPLETED; }
};

class ThreadSchedulerBase
{
protected:

  /** clock */
  std::vector<uint64_t> clock;

  /** Holds how many threads are runnning on each core */
  std::vector<int> workerThreadCount;
  
  std::vector<std::deque<std::reference_wrapper<ThreadContext>>>
        coreToThreadMap;

  /** Holds each threads current eventId */
  std::vector<StEventID> curEvent;
  std::vector<SpinLock*> curMtx;

  /** Holds wakeup signal for each thread */
  std::vector<bool> wakeupSig;
  SpinLock wakeupMtx;

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

  uint64_t curClock(CoreID coreId) const { return clock[coreId]; }
  uint64_t nextClock(CoreID coreId) { return ++clock[coreId]; }

  virtual CoreID threadIdToCoreId(ThreadID threadId) const = 0;

  virtual void init(ThreadContext& tcxt) = 0;

  virtual void recordEvent(ThreadContext& tcxt) = 0;
  virtual StEventID checkEvent(ThreadID threadId) = 0;

  virtual void sendReady(ThreadID threadId) = 0;
  virtual bool checkReady(ThreadID threadId) = 0;
  virtual void getReady(ThreadContext& tcxt, CoreID coreId) = 0;
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

public:
  ThreadScheduler(float CPI_IOPS, float CPI_FLOPS, uint32_t cxtSwitchCycles, 
    uint32_t pthCycles, uint32_t schedSliceCycles)
  : ThreadSchedulerBase(CPI_IOPS, CPI_FLOPS, cxtSwitchCycles, pthCycles, schedSliceCycles) 
  {
    clock.resize(NC);
    coreToThreadMap.resize(NC); 
    workerThreadCount.resize(NC);
    wakeupSig.resize(NT);
    curEvent.resize(NT);
    curMtx.resize(NT);

    for (CoreID i = 0; i < NC; i++) {
      workerThreadCount[i] = 0;
      clock[i] = 0;
    }
    for (ThreadID i = 0; i < NT; i++) {
      wakeupSig[i] = false;
      curEvent[i] = 0;
      curMtx[i] = new SpinLock();
    }
  }

  CoreID threadIdToCoreId(ThreadID threadId) const{
    return threadId % NC;
  }

  ~ThreadScheduler(){
    for (auto m : curMtx) delete m;
  }

  virtual void init(ThreadContext& tcxt) {
    coreToThreadMap.at(threadIdToCoreId(tcxt.threadId)).emplace_back(tcxt);
  }

  virtual void recordEvent(ThreadContext& tcxt) {
    curMtx[tcxt.threadId]->lock();
    curEvent[tcxt.threadId] = tcxt.currEventId;
    curMtx[tcxt.threadId]->unlock();
  }

  virtual StEventID checkEvent(ThreadID threadId) {
    curMtx[threadId]->lock();
    StEventID eventId = curEvent[threadId];
    curMtx[threadId]->unlock();
    return eventId;
  }

  virtual void sendReady(ThreadID threadId) {
    wakeupMtx.lock();
    wakeupSig[threadId] = true;
    wakeupMtx.unlock();
  }

  virtual bool checkReady(ThreadID threadId) {
    wakeupMtx.lock();
    bool ready = wakeupSig[threadId];
    wakeupMtx.unlock();
    return ready;
  }

  virtual void getReady(ThreadContext& tcxt, CoreID coreId) {
    if (tcxt.running())
      return;

    wakeupMtx.lock();
    wakeupSig[tcxt.threadId] = false;
    wakeupMtx.unlock();

    if (tcxt.status == ThreadStatus::BLOCKED_MUTEX)
      tcxt.status = ThreadStatus::WAIT_LOCK;
    else
      tcxt.status = ThreadStatus::WAIT_SCHED;

    workerThreadCount[coreId]++;

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

    CoreID coreId = threadIdToCoreId(tcxt.threadId);
    tcxt.wakeupClock = curClock(coreId) + cycles;
    if (tcxt.status != ThreadStatus::WAIT_SCHED && !tcxt.blocked())
      tcxt.restSliceCycles -= cycles;
  }

  virtual ThreadID findActive(CoreID coreId)
  {
    if (workerThreadCount[coreId] == 0)
      return -1;

    ThreadContext& tcxt = coreToThreadMap[coreId].front().get();
    if (!tcxt.active())
      return -1;

    if (tcxt.status == ThreadStatus::ACTIVE) {
      if (tcxt.restSliceCycles <= 0) {                   // if the thread has used up its slice
        tcxt.restSliceCycles = schedSliceCycles;
        if (tryCxtSwapAndSchedule(coreId)) {
          tcxt.status = ThreadStatus::WAIT_SCHED;
          tcxt.wakeupClock = curClock(coreId) + schedSliceCycles;
        }
      }
    }

    // if the thread replays an unsuccessful COMM/MUTEX_LOCK, it must haven't used up its slice
    // so there is no need to check it.

    return tcxt.threadId;
  }

  virtual bool checkOnCore(ThreadID threadId) {
    CoreID coreId = threadIdToCoreId(threadId);
    return coreToThreadMap[coreId].front().get().threadId == threadId;
  }
};

#endif