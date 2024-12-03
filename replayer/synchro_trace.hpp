#ifndef CM_REPLAYER_SYNCHROTRACE_HPP
#define CM_REPLAYER_SYNCHROTRACE_HPP

#include "replayer/st_event.hpp"
#include "replayer/st_parser.hpp"
#include "replayer/thread.hpp"
#include "cache/coherence.hpp"
#include "st_event.hpp"
#include "thread.hpp"
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

class SynchroTraceReplayerBase
{
protected:

 /**************************************************************************
   * Synchronization state
  */
  StTracePthreadMetadata pthMetadata;
  
  /** Holds each thread's context */
  std::vector<ThreadContext> threadContexts;

  /* record the time when each core starts running */
  std::vector<std::pair<bool, std::chrono::steady_clock::time_point >> wakeuptime;

  /** Holds which threads currently wait for a lock */
  std::map<uint64_t, std::queue<ThreadID>> perMutexThread;
  SpinLock mutexMtx;

  /** Holds spin locks in use */
  std::set<uint64_t> spinLocks;
  SpinLock spinLocksMtx;

  /** Holds which threads are waiting for a barrier */
  std::map<uint64_t, std::set<ThreadID>> threadBarrierMap;
  SpinLock barrierMtx;

  /** Holds which threads currently possess a mutex lock */
  std::vector<std::vector<uint64_t>> perThreadLocksHeld;

  /** Holds which threads waiting for the thread to complete */
  std::vector<std::vector<ThreadID>> joinList;
  SpinLock joinMtx;

  /** Directory of Sigil Traces and Pthread metadata file */
  std::string eventDir;

  /** data cache */
  std::vector<CoreInterfaceBase *>& core_data;

  /** mutex for cout */
  SpinLock outputMtx;

  /** threads for each core */
  std::vector<std::thread> replayCore;

  virtual bool isCommDependencyBlocked(const MemoryRequest_ThreadCommunication& comm) = 0;
  virtual uint64_t msgReqSend(CoreID coreId, uint64_t addr, uint32_t bytes, ReqType type) = 0;

  virtual void processEventMarker(ThreadContext& tcxt, CoreID coreId) = 0;
  virtual void processInsnMarker(ThreadContext& tcxt, CoreID coreId) = 0;
  virtual void processEndMarker(ThreadContext& tcxt, CoreID coreId) = 0;

  virtual bool mutexTryLock(ThreadContext& tcxt, CoreID coreId, uint64_t pthaddr) = 0;
  virtual void mutexUnlock(ThreadContext& tcxt, CoreID coreId, uint64_t pthaddr) = 0;

  virtual void replayCompute(ThreadContext& tcxt, CoreID coreId) = 0;
  virtual void replayComm(ThreadContext& tcxt, CoreID coreId) = 0;
  virtual void replayThreadAPI(ThreadContext& tcxt, CoreID coreId) = 0;
  virtual void wakeupCore(ThreadID threadId, CoreID coreId) = 0;
  virtual void wakeupDebugLog(CoreID coreId) = 0;

public:
  SynchroTraceReplayerBase(std::string eventDir, std::vector<CoreInterfaceBase *>& core_data) : 
   eventDir(eventDir), core_data(core_data), pthMetadata(eventDir) {

  }

  virtual void init()  = 0;
  virtual void start() = 0;

  virtual std::vector<std::pair<bool, std::chrono::steady_clock::time_point >>& get_wakeuptime() {
    return wakeuptime;
  }
};

// NT: num threads, NC: num cores
template <ThreadID NT, CoreID NC>
class SynchroTraceReplayer : public SynchroTraceReplayerBase
{
protected:

  ThreadScheduler<NT, NC> scheduler;


  /**
    * For a communication event, check to see if the producer has reached
    * the dependent event. This function also handles the case of a system
    * call. System calls are viewed as producer->consumer interactions with
    * the 'producer' system call having a ThreadID of 30000. For obvious
    * reasons, there are no 'dependencies' to enforce in the case of a system
    * call.
    */
  virtual bool isCommDependencyBlocked(const MemoryRequest_ThreadCommunication& comm)
  {
    // If the producer thread's EventID is greater than the dependent event
    // then the dependency is satisfied
    return scheduler.checkEvent(comm.sourceThreadId) > comm.sourceEventId;
  }

  virtual uint64_t msgReqSend(CoreID coreId, uint64_t addr, uint32_t bytes, ReqType type){
    uint64_t delay = 0;

    if(type == ReqType::REQ_READ)
      core_data[coreId]->read(addr, &delay);
    else
      core_data[coreId]->write(addr, nullptr, &delay);

    return delay;
  }

  virtual void processEventMarker(ThreadContext& tcxt, CoreID coreId)
  {
    tcxt.currEventId++;  // increment after, starts at 0
    tcxt.evStream.pop();
  }

  virtual void processInsnMarker(ThreadContext& tcxt, CoreID coreId)
  {
    tcxt.evStream.pop();
  }

  virtual void processEndMarker(ThreadContext& tcxt, CoreID coreId)
  {
    tcxt.evStream.pop();
    scheduler.recordEvent(tcxt);

    joinMtx.lock();
    scheduler.getBlocked(tcxt, ThreadStatus::COMPLETED);
    for (ThreadID threadId : joinList[tcxt.threadId])
      scheduler.sendReady(threadId);
    joinMtx.unlock();
  }

  virtual void replayCompute(ThreadContext& tcxt, CoreID coreId)
  {

    assert(tcxt.evStream.peek().tag == Tag::COMPUTE);
    // simulate time for the iops/flops
    const ComputeOps& ops = tcxt.evStream.peek().computeOps;

    scheduler.schedule(tcxt,
            1 + scheduler.CPI_IOPS * ops.iops + scheduler.CPI_FLOPS * ops.flops);
    tcxt.status = ThreadStatus::WAIT_COMPUTE;
    tcxt.evStream.pop();
  }

  virtual void replayMemory(ThreadContext& tcxt, CoreID coreId)
  {
    assert(tcxt.evStream.peek().tag == Tag::MEMORY);
    // Send the load/store
    const StEvent& ev = tcxt.evStream.peek();

    scheduler.schedule(tcxt, msgReqSend(coreId, 
                                      ev.memoryReq.addr, 
                                      ev.memoryReq.bytesRequested, 
                                      ev.memoryReq.type));
    tcxt.status = ThreadStatus::WAIT_MEMORY;
    tcxt.evStream.pop();
  }

  virtual void replayComm(ThreadContext& tcxt, CoreID coreId)
  {
    assert(tcxt.evStream.peek().tag == Tag::MEMORY_COMM);
    const StEvent& ev = tcxt.evStream.peek();

    if(isCommDependencyBlocked(ev.memoryReqComm) || 
      !perThreadLocksHeld[tcxt.threadId].empty())
    {
      scheduler.schedule(tcxt, msgReqSend(coreId, 
                                        ev.memoryReqComm.addr, 
                                        ev.memoryReqComm.bytesRequested, 
                                        ReqType::REQ_READ));
      tcxt.status = ThreadStatus::WAIT_MEMORY;
      tcxt.evStream.pop();
    }
    else
    {
      tcxt.status = ThreadStatus::WAIT_COMM;
      if (!scheduler.tryCxtSwapAndSchedule(coreId))
        scheduler.schedule(tcxt, 1);
      else
        scheduler.schedule(tcxt, scheduler.schedSliceCycles);
    }
  }

  virtual bool mutexTryLock(ThreadContext& tcxt, CoreID coreId, uint64_t pthaddr) 
  {
    mutexMtx.lock();

    auto it = perMutexThread.find(pthaddr);
    if (it != perMutexThread.end()) { // if no other thread holds the lock
      if (tcxt.status != ThreadStatus::ACTIVE_TRYLOCK) {
        it->second.push(tcxt.threadId);
      }
      bool getMutex = (tcxt.threadId == it->second.front());
      if (getMutex) perThreadLocksHeld[tcxt.threadId].push_back(pthaddr); 
      mutexMtx.unlock();
      return getMutex;
    } else {
      perMutexThread[pthaddr].push(tcxt.threadId);
      perThreadLocksHeld[tcxt.threadId].push_back(pthaddr);

      mutexMtx.unlock();
      return true;
    }
  }

  virtual void mutexUnlock(ThreadContext& tcxt, CoreID coreId, uint64_t pthaddr) 
  {
    mutexMtx.lock();

    std::vector<uint64_t>& locksHeld = perThreadLocksHeld[tcxt.threadId];
    auto v_it = std::find(locksHeld.begin(), locksHeld.end(), pthaddr);
    assert(v_it != locksHeld.end());
    locksHeld.erase(v_it);

    auto it = perMutexThread.find(pthaddr);
    assert(it != perMutexThread.end());
    assert(!(it->second.empty()));
    assert(it->second.front() == tcxt.threadId);
    it->second.pop();
    assert(((!it->second.empty()) && (it->second.front() != tcxt.threadId))
          || (it->second.empty()));
    // If a thread is blocked on this lock, a notification is sent to unlock the thread.
    if (!(it->second.empty())) {
      scheduler.sendReady(it->second.front());
    }

    mutexMtx.unlock();
  }

  virtual void replayThreadAPI(ThreadContext& tcxt, CoreID coreId) {
    assert(tcxt.evStream.peek().tag == Tag::THREAD_API);
    const StEvent& ev = tcxt.evStream.peek();
    const uint64_t pthAddr = ev.threadApi.pthAddr;

    switch (ev.threadApi.eventType) {
      case EventType::MUTEX_LOCK:
      {
        if (mutexTryLock(tcxt, coreId, pthAddr)) {
          tcxt.evStream.pop();
          tcxt.status = ThreadStatus::WAIT_THREAD;
          scheduler.schedule(tcxt, scheduler.pthCycles);
        } else {
          scheduler.getBlocked(tcxt, ThreadStatus::BLOCKED_MUTEX);
        }
        break;
      }
      case EventType::MUTEX_UNLOCK:
      {
        mutexUnlock(tcxt, coreId, pthAddr);
        tcxt.evStream.pop();
        tcxt.status = ThreadStatus::WAIT_THREAD;
        scheduler.schedule(tcxt, scheduler.pthCycles);
        break;
      }
      case EventType::THREAD_CREATE:
      {
        assert(tcxt.status == ThreadStatus::ACTIVE);
        assert(pthMetadata.addressToIdMap().find(pthAddr) != pthMetadata.addressToIdMap().cend());

        const ThreadID workerThreadID = {pthMetadata.addressToIdMap().at(pthAddr)};

        outputMtx.lock();
        std::cout << "New Thread ID: " << workerThreadID << " " << threadContexts.size() << std::endl;
        outputMtx.unlock();

        assert(workerThreadID < threadContexts.size());
        assert(threadContexts[workerThreadID].status == ThreadStatus::INACTIVE);

        if (wakeuptime[workerThreadID].first == false) wakeuptime[workerThreadID] = std::make_pair(true, std::chrono::steady_clock::now());

        scheduler.sendReady(workerThreadID);
        
        tcxt.status = ThreadStatus::WAIT_THREAD;
        scheduler.schedule(tcxt, scheduler.pthCycles);
        
        tcxt.evStream.pop();

        break;
      }
      case EventType::THREAD_JOIN:
      {
        assert(pthMetadata.addressToIdMap().find(pthAddr) != pthMetadata.addressToIdMap().cend());

        const ThreadContext& workertcxt = threadContexts[pthMetadata.addressToIdMap().at(pthAddr)];

        joinMtx.lock();

        if (workertcxt.completed()) {

          joinMtx.unlock();

          // reset to active, in case this thread was previously blocked
          tcxt.status = ThreadStatus::WAIT_THREAD;

          outputMtx.lock();
          std::cout << "Thread " << workertcxt.threadId << " joined" << std::endl; 
          outputMtx.unlock();

          tcxt.evStream.pop();
          scheduler.schedule(tcxt, scheduler.pthCycles);
        } else if (workertcxt.running() || workertcxt.blocked() || scheduler.checkReady(workertcxt.threadId)) {
          joinList[workertcxt.threadId].push_back(tcxt.threadId);

          joinMtx.unlock();
          scheduler.getBlocked(tcxt, ThreadStatus::BLOCKED_JOIN);
        } else {

          joinMtx.unlock();

          // failed joined Thread
          assert(0);
        }
        break;
      }
      case EventType::BARRIER_WAIT:
      {
        barrierMtx.lock();

        auto p = threadBarrierMap[pthAddr].insert(tcxt.threadId);
        // Check if this is the last thread to enter the barrier,
        // in which case, unblock all the threads.
        if (threadBarrierMap[pthAddr] == pthMetadata.barrierMap().at(pthAddr)) {
          for(auto tid : pthMetadata.barrierMap().at(pthAddr)) {
            threadContexts[tid].evStream.pop();
            scheduler.sendReady(tid);
          }
          assert(threadBarrierMap.erase(pthAddr));

          barrierMtx.unlock();
          tcxt.status = ThreadStatus::WAIT_THREAD;
          scheduler.schedule(tcxt, scheduler.pthCycles);
        } else {          
          barrierMtx.unlock();
          scheduler.getBlocked(tcxt, ThreadStatus::BLOCKED_BARRIER);
        }
        break;
      }
      case EventType::SPIN_LOCK:
      {
        assert(tcxt.status == ThreadStatus::ACTIVE);

        spinLocksMtx.lock();
        auto p = spinLocks.insert(pthAddr);
        spinLocksMtx.unlock();

        if (p.second) {
          tcxt.evStream.pop();
          perThreadLocksHeld[tcxt.threadId].push_back(pthAddr);
        }

        // Reschedule regardless of whether the lock was acquired.
        // If the lock wasn't acquired, we spin and try again the next cycle.
        tcxt.status = ThreadStatus::WAIT_THREAD;
        scheduler.schedule(tcxt, scheduler.pthCycles);
        break;
      }
      case EventType::SPIN_UNLOCK:
      {
        assert(tcxt.status == ThreadStatus::ACTIVE);
        
        spinLocksMtx.lock();

        auto it = spinLocks.find(pthAddr);
        if (it != spinLocks.end()) {
          spinLocks.erase(it);
          std::vector<uint64_t>& locksHeld = perThreadLocksHeld[tcxt.threadId];
          auto v_it = std::find(locksHeld.begin(), locksHeld.end(), pthAddr);
          assert(v_it != locksHeld.end());
          locksHeld.erase(v_it);
        }

        spinLocksMtx.unlock();

        tcxt.evStream.pop();
        tcxt.status = ThreadStatus::WAIT_THREAD;
        scheduler.schedule(tcxt, scheduler.pthCycles);
        break;
      }
      case EventType::COND_WAIT:
      case EventType::COND_SG:
      case EventType::COND_BR:
      case EventType::SEM_INIT:
      case EventType::SEM_WAIT:
      case EventType::SEM_POST:
      case EventType::SEM_GETV:
      case EventType::SEM_DEST:
        std::cerr << "Unsupported Semaphore Event Type encountered" << std::endl; 
        break;
      default:
        std::cerr << "Unexpected Thread Event Type encountered" << std::endl;
        break;
    }
  }

  virtual void wakeupCore(ThreadID threadId, CoreID coreId) {
    ThreadContext& tcxt = threadContexts[threadId];
    assert(tcxt.running());

    switch (tcxt.evStream.peek().tag) 
    {
      case Tag::COMPUTE:
        replayCompute(tcxt, coreId);
        break;
      case Tag::MEMORY:
        replayMemory(tcxt, coreId);
        break;
      case Tag::MEMORY_COMM:
        replayComm(tcxt, coreId);
        break;
      case Tag::THREAD_API:
        replayThreadAPI(tcxt, coreId);
        break;
      case Tag::TRACE_EVENT_MARKER:
        processEventMarker(tcxt, coreId);
        break;
      case Tag::INSN_MARKER:
        processInsnMarker(tcxt, coreId);
        break;
      case Tag::END_OF_EVENTS:
        processEndMarker(tcxt, coreId);
        break;
      default:
        assert(0);
    }
  }

  virtual void wakeupDebugLog(CoreID coreId){
    // outputMtx.lock();
    // printf("CoreID<%d>:clock:%ld\n", coreId, scheduler.curClock(coreId));
    // for(const auto & cxt : threadContexts){
    //   if (scheduler.threadIdToCoreId(cxt.threadId) != coreId)
    //     continue;
    //   printf("Thread<%d>:Event<%ld>:Status<%s>", cxt.threadId, cxt.currEventId, toString(cxt.status));

    //   if (scheduler.checkOnCore(cxt.threadId))
    //     printf(" OnCore=True");
    //   else
    //     printf(" OnCore=False");

    //   if (cxt.waiting())
    //     printf(" wakeupClock=%lu\n", cxt.wakeupClock);
    //   else
    //     printf("\n");
    // }
    // outputMtx.unlock();
  }

  void replay(CoreID coreId) {
    while(true) {
      if (std::all_of(threadContexts.cbegin(), threadContexts.cend(),
                      [this, coreId](const ThreadContext& tcxt)
                      { return (this->scheduler.threadIdToCoreId(tcxt.threadId) != coreId) ||
                                tcxt.completed(); })) 
        break;
      
      for(auto &tcxt : threadContexts) {
        if (scheduler.threadIdToCoreId(tcxt.threadId) != coreId)
          continue;
        
        switch (tcxt.status) 
        {
          case ThreadStatus::WAIT_LOCK:
            if (scheduler.curClock(coreId) == tcxt.wakeupClock) {
              tcxt.status = ThreadStatus::ACTIVE_TRYLOCK;
            }
            break;
          case ThreadStatus::WAIT_COMPUTE:
          case ThreadStatus::WAIT_MEMORY:
          case ThreadStatus::WAIT_THREAD:
          case ThreadStatus::WAIT_SCHED:
          case ThreadStatus::WAIT_COMM:
            if (scheduler.curClock(coreId) == tcxt.wakeupClock) {
              tcxt.status = ThreadStatus::ACTIVE;
            }
            break;
          case ThreadStatus::ACTIVE:
          case ThreadStatus::ACTIVE_TRYLOCK: 
            break;
          case ThreadStatus::BLOCKED_MUTEX:
          case ThreadStatus::BLOCKED_BARRIER:
          case ThreadStatus::BLOCKED_JOIN:
          case ThreadStatus::INACTIVE:
            if (scheduler.curClock(coreId) % 10 == 0 && scheduler.checkReady(tcxt.threadId))
              scheduler.getReady(tcxt, coreId);
            break;
          case ThreadStatus::COMPLETED:
          default:
            break;
        }

        if (scheduler.curClock(coreId) % 10 == 0) {
          scheduler.recordEvent(tcxt);
        }
      }

      ThreadID tid = scheduler.findActive(coreId);
      if (tid >= 0) {
        wakeupCore(tid, coreId);
      }

      if (scheduler.nextClock(coreId) % 1000000 == 0) {
        wakeupDebugLog(coreId);
      } 
    }

    outputMtx.lock();
    std::cout << "All threads on Core " << coreId << " completed at " << scheduler.curClock(coreId) << ".\n" << std::endl;
    outputMtx.unlock();
  }

public:
  
  SynchroTraceReplayer(std::string eventDir, float CPI_IOPS, float CPI_FLOPS, uint32_t cxtSwitchCycles, 
    uint32_t pthCycles, uint32_t schedSliceCycles, std::vector<CoreInterfaceBase *>& core_data) 
  : SynchroTraceReplayerBase(eventDir, core_data),
    scheduler(CPI_IOPS, CPI_FLOPS, cxtSwitchCycles, pthCycles, schedSliceCycles)
  {
    perThreadLocksHeld.resize(NT);
    joinList.resize(NT);
    wakeuptime.resize(NC);
    replayCore.resize(NC);
  }

  virtual void init() {
    for (ThreadID i = 0; i < NT; i++){
      threadContexts.emplace_back(i, eventDir);
    }

    for (CoreID i = 0; i < NC; i++) {
      wakeuptime[i] = std::make_pair(false, std::chrono::steady_clock::now());
    }

    for(auto& tcxt : threadContexts)
      scheduler.init(tcxt);
    
    // Set master (first) thread as active.
    // Schedule first tick of the initial core.
    // (the other cores begin 'inactive', and
    //  expect the master thread to start them)
    threadContexts[0].restSliceCycles = scheduler.schedSliceCycles;
    scheduler.sendReady(0);
    wakeuptime[0] = std::make_pair(true, std::chrono::steady_clock::now());
  }

  virtual void start() {
    // the main thread
    for (CoreID i = 0; i < NC; i++) {
      replayCore[i] = std::thread(std::bind(&SynchroTraceReplayer::replay, this, i));
    }

    for (auto &c : replayCore) {
      c.join();
    }

    wakeuptime.push_back(std::make_pair(true, std::chrono::steady_clock::now()));

    std::cout << "Replay Completed!" << std::endl;
  }
};


// NT: num threads, NC: num cores, Single Thread Replayer
template <ThreadID NT, CoreID NC>
class SynchroTraceReplayerST : public SynchroTraceReplayerBase
{
protected:

  /** Abstract cpi estimation for integer ops */
  const float CPI_IOPS;

  /** Abstract cpi estimation for floating point ops */
  const float CPI_FLOPS;

  /** Cycles for pthread event */
  const uint32_t pthCycles;

  /** Cycles to simulate the time slice the scheduler gives to a thread */
  const uint32_t schedSliceCycles;

  /** Cycles for context switch on a core */
  const uint32_t cxtSwitchCycles;

  uint64_t clock;

  uint64_t curClock() { return clock; }

  /** Holds mutex locks in use */
  std::set<uint64_t> mutexLocks;

  /** Holds condition variables signaled by broadcasts and signals */
  std::vector<std::map<uint64_t, int>> condSignals;

  std::vector<std::deque<std::reference_wrapper<ThreadContext>>>
        coreToThreadMap;

  int workerThreadCount;

  CoreID threadIdToCoreId(ThreadID threadId) const{
    return threadId % NC;
  }

  /**
    * For a communication event, check to see if the producer has reached
    * the dependent event. This function also handles the case of a system
    * call. System calls are viewed as producer->consumer interactions with
    * the 'producer' system call having a ThreadID of 30000. For obvious
    * reasons, there are no 'dependencies' to enforce in the case of a system
    * call.
    */
  virtual bool isCommDependencyBlocked(const MemoryRequest_ThreadCommunication& comm)
  {
    // If the producer thread's EventID is greater than the dependent event
    // then the dependency is satisfied
    return (threadContexts[comm.sourceThreadId].currEventId > comm.sourceEventId);
  }

  virtual uint64_t msgReqSend(CoreID coreId, uint64_t addr, uint32_t bytes, ReqType type){
    uint64_t delay = 0;
    if(type == ReqType::REQ_READ)
      core_data[coreId]->read(addr, &delay);
    else
      core_data[coreId]->write(addr, nullptr, &delay);
    return delay;
  }

  virtual void processEventMarker(ThreadContext& tcxt, CoreID coreId)
  {
    tcxt.currEventId++;  // increment after, starts at 0
    tcxt.evStream.pop();
  }

  virtual void processInsnMarker(ThreadContext& tcxt, CoreID coreId)
  {
    tcxt.evStream.pop();
  }

  virtual void processEndMarker(ThreadContext& tcxt, CoreID coreId)
  {
    tcxt.status = ThreadStatus::COMPLETED;
    tcxt.evStream.pop();
  }


  virtual void replayCompute(ThreadContext& tcxt, CoreID coreId)
  {
    assert(tcxt.evStream.peek().tag == Tag::COMPUTE);
    // simulate time for the iops/flops
    const ComputeOps& ops = tcxt.evStream.peek().computeOps;
    tcxt.wakeupClock = curClock() + 1 + CPI_IOPS * ops.iops + CPI_FLOPS * ops.flops;
    tcxt.status = ThreadStatus::WAIT_COMPUTE;
    tcxt.evStream.pop();
  }

  virtual void replayMemory(ThreadContext& tcxt, CoreID coreId)
  {
    assert(tcxt.evStream.peek().tag == Tag::MEMORY);
    // Send the load/store
    const StEvent& ev = tcxt.evStream.peek();
    uint64_t delay = msgReqSend(coreId, ev.memoryReq.addr, ev.memoryReq.bytesRequested, ev.memoryReq.type);
    tcxt.wakeupClock = curClock() + delay;
    tcxt.status = ThreadStatus::WAIT_MEMORY;
    tcxt.evStream.pop();
  }

  virtual void replayComm(ThreadContext& tcxt, CoreID coreId)
  {
    assert(tcxt.evStream.peek().tag == Tag::MEMORY_COMM);
    const StEvent& ev = tcxt.evStream.peek();
    if(isCommDependencyBlocked(ev.memoryReqComm) || 
      !perThreadLocksHeld[tcxt.threadId].empty())
    {
      uint64_t delay = msgReqSend(coreId, ev.memoryReqComm.addr, ev.memoryReqComm.bytesRequested, ReqType::REQ_READ);
      tcxt.status = ThreadStatus::WAIT_MEMORY;
      tcxt.wakeupClock = curClock() + delay; 
      tcxt.evStream.pop();
    }
    else
    {
      tcxt.status = ThreadStatus::WAIT_COMM;
      tcxt.wakeupClock = curClock() + 1;
    }
  }

  virtual void replayThreadAPI(ThreadContext& tcxt, CoreID coreID){
    assert(tcxt.evStream.peek().tag == Tag::THREAD_API);
    const StEvent& ev = tcxt.evStream.peek();
    const uint64_t pthAddr = ev.threadApi.pthAddr;

    switch (ev.threadApi.eventType)
    {
      case EventType::MUTEX_LOCK:
      {
        auto p = mutexLocks.insert(pthAddr);
        if (p.second) // insert success
        {
          tcxt.status = ThreadStatus::WAIT_THREAD;
          tcxt.wakeupClock = curClock() + pthCycles;
          perThreadLocksHeld[tcxt.threadId].push_back(pthAddr);
          tcxt.evStream.pop();
        } else {
          tcxt.status = ThreadStatus::BLOCKED_MUTEX;
          tcxt.wakeupClock = curClock() + schedSliceCycles;
        }
        break;
      }
      case EventType::MUTEX_UNLOCK:
      {
        std::vector<uint64_t>& locksHeld = perThreadLocksHeld[tcxt.threadId];
        auto s_it = mutexLocks.find(pthAddr);
        auto v_it = std::find(locksHeld.begin(), locksHeld.end(), pthAddr);
        assert(s_it != mutexLocks.end());
        assert(v_it != locksHeld.end());
        mutexLocks.erase(s_it);
        locksHeld.erase(v_it);
        tcxt.evStream.pop();
        tcxt.status = ThreadStatus::WAIT_THREAD;
        tcxt.wakeupClock = curClock() + pthCycles;

        break;
      }
      case EventType::THREAD_CREATE:
      {
        assert(tcxt.status == ThreadStatus::ACTIVE);
        assert(pthMetadata.addressToIdMap().find(pthAddr) != pthMetadata.addressToIdMap().cend());

        workerThreadCount++;

        const ThreadID workerThreadID = {pthMetadata.addressToIdMap().at(pthAddr)};
        assert(workerThreadID < threadContexts.size());
        ThreadContext& workertcxt = threadContexts[workerThreadID];

        assert(workertcxt.status == ThreadStatus::INACTIVE);

        workertcxt.status = ThreadStatus::ACTIVE;
        const CoreID workerCoreID = {threadIdToCoreId(workerThreadID)};

        if (wakeuptime[workerThreadID].first == false) wakeuptime[workerThreadID] = std::make_pair(true, std::chrono::steady_clock::now());

        tcxt.status = ThreadStatus::WAIT_THREAD;
        tcxt.wakeupClock = curClock() + pthCycles;

        std::cout << "Thread " << workerThreadID << " created" << std::endl; 
        
        tcxt.evStream.pop();

        break;
      }
      case EventType::THREAD_JOIN:
      {
        assert(pthMetadata.addressToIdMap().find(pthAddr) != pthMetadata.addressToIdMap().cend());

        const ThreadContext& workertcxt = threadContexts[pthMetadata.addressToIdMap().at(pthAddr)];
        if(workertcxt.completed())
        {
          // reset to active, in case this thread was previously blocked
          tcxt.status = ThreadStatus::WAIT_THREAD;
          std::cout << "Thread " << workertcxt.threadId << " joined" << std::endl; 
          workerThreadCount--;
          tcxt.evStream.pop();
          tcxt.wakeupClock = curClock() + pthCycles;
        }
        else if (workertcxt.running())
        {
          tcxt.status = ThreadStatus::BLOCKED_JOIN;
          tcxt.wakeupClock = curClock() + schedSliceCycles;
        }
        else
        {
          // failed joined Thread
          assert(0);
        }
        break;
      }
      case EventType::BARRIER_WAIT:
      {
        auto p = threadBarrierMap[pthAddr].insert(tcxt.threadId);
        // Check if this is the last thread to enter the barrier,
        // in which case, unblock all the threads.
        if(threadBarrierMap[pthAddr] == pthMetadata.barrierMap().at(pthAddr))
        {
          for(auto tid : pthMetadata.barrierMap().at(pthAddr))
          {
            threadContexts[tid].status = ThreadStatus::ACTIVE;
            threadContexts[tid].evStream.pop();
          }
          assert(threadBarrierMap.erase(pthAddr));
          tcxt.status = ThreadStatus::WAIT_THREAD;
          tcxt.wakeupClock = curClock() + pthCycles;
        } else {
          tcxt.status = ThreadStatus::BLOCKED_BARRIER;
          tcxt.wakeupClock = curClock() + schedSliceCycles;
        }
        break;
      }
      case EventType::SPIN_LOCK:
      {
        assert(tcxt.status == ThreadStatus::ACTIVE);
        auto p = spinLocks.insert(pthAddr);
        if (p.second) tcxt.evStream.pop();

        // Reschedule regardless of whether the lock was acquired.
        // If the lock wasn't acquired, we spin and try again the next cycle.
        tcxt.status = ThreadStatus::WAIT_THREAD;
        tcxt.wakeupClock = curClock() + pthCycles;
        break;
      }
      case EventType::SPIN_UNLOCK:
      {
        assert(tcxt.status == ThreadStatus::ACTIVE);
        
        auto it = spinLocks.find(pthAddr);
        assert(it != spinLocks.end());
        spinLocks.erase(it);
        tcxt.evStream.pop();
        tcxt.status = ThreadStatus::WAIT_THREAD;
        tcxt.wakeupClock = curClock() + pthCycles;
        break;
      }
      case EventType::COND_WAIT:
      case EventType::COND_SG:
      case EventType::COND_BR:
      case EventType::SEM_INIT:
      case EventType::SEM_WAIT:
      case EventType::SEM_POST:
      case EventType::SEM_GETV:
      case EventType::SEM_DEST:
        std::cerr << "Unsupported Semaphore Event Type encountered" << std::endl; 
        break;
      default:
        std::cerr << "Unexpected Thread Event Type encountered" << std::endl;
        break;
    }
  }

  virtual void wakeupCore(CoreID coreId){
    ThreadContext& tcxt = coreToThreadMap[coreId].front();
    assert(tcxt.running());

    switch (tcxt.evStream.peek().tag) 
    {
      case Tag::COMPUTE:
        replayCompute(tcxt, coreId);
        break;
      case Tag::MEMORY:
        replayMemory(tcxt, coreId);
        break;
      case Tag::MEMORY_COMM:
        replayComm(tcxt, coreId);
        break;
      case Tag::THREAD_API:
        replayThreadAPI(tcxt, coreId);
        break;
      case Tag::TRACE_EVENT_MARKER:
        processEventMarker(tcxt, coreId);
        break;
      case Tag::INSN_MARKER:
        processInsnMarker(tcxt, coreId);
        break;
      case Tag::END_OF_EVENTS:
        processEndMarker(tcxt, coreId);
        break;
      default:
        assert(0);
    }
  }

  virtual void wakeupDebugLog(){
    // printf("clock:%ld\n", curClock());
    // for(const auto & cxt : threadContexts){
    //   printf("Thread<%d>:Event<%ld>:Status<%s>\n", cxt.threadId, cxt.currEventId, toString(cxt.status));
    // }
  }

  virtual bool mutexTryLock(ThreadContext& tcxt, CoreID coreId, uint64_t pthaddr) {}
  virtual void mutexUnlock(ThreadContext& tcxt, CoreID coreId, uint64_t pthaddr) {}

  virtual void wakeupCore(ThreadID threadId, CoreID coreId) {}
  virtual void wakeupDebugLog(CoreID coreId) {}

public:
  SynchroTraceReplayerST(std::string eventDir, float CPI_IOPS, float CPI_FLOPS, uint32_t cxtSwitchCycles, uint32_t pthCycles, 
    uint32_t schedSliceCycles, std::vector<CoreInterfaceBase *>& core_data) 
  : SynchroTraceReplayerBase(eventDir, core_data), CPI_IOPS(CPI_IOPS), CPI_FLOPS(CPI_FLOPS), cxtSwitchCycles(cxtSwitchCycles), 
    pthCycles(pthCycles), schedSliceCycles(schedSliceCycles), clock(0)
  {
    perThreadLocksHeld.resize(NT);
    coreToThreadMap.resize(NC);
    wakeuptime.resize(NC); 
  }

  virtual void init(){
    workerThreadCount = 0;

    for(ThreadID i = 0; i < NT; i++){
      threadContexts.emplace_back(i, eventDir);
    }
    for(auto& tcxt : threadContexts)
      coreToThreadMap.at(threadIdToCoreId(tcxt.threadId)).emplace_back(tcxt);
    
    for (CoreID i = 0; i < NC; i++) {
      wakeuptime[i] = std::make_pair(false, std::chrono::steady_clock::now());
    }

    // Set master (first) thread as active.
    // Schedule first tick of the initial core.
    // (the other cores begin 'inactive', and
    //  expect the master thread to start them)
    threadContexts[0].status = ThreadStatus::ACTIVE;
    workerThreadCount++;
    wakeuptime[0] = std::make_pair(true, std::chrono::steady_clock::now());
  }

  virtual void start(){
    while(true){
      if (std::all_of(threadContexts.cbegin(), threadContexts.cend(),
                      [](const ThreadContext& tcxt)
                      { return tcxt.completed(); })) 
        break;
      for(auto &tcxt : threadContexts)
      {
        switch (tcxt.status) 
        {
          case ThreadStatus::WAIT_COMPUTE:
          case ThreadStatus::WAIT_MEMORY:
          case ThreadStatus::WAIT_THREAD:
          case ThreadStatus::WAIT_COMM:
            if(curClock() == tcxt.wakeupClock){
              tcxt.status = ThreadStatus::ACTIVE;
            }
            break;
          case ThreadStatus::ACTIVE:
          case ThreadStatus::BLOCKED_MUTEX:
          case ThreadStatus::BLOCKED_BARRIER:
          case ThreadStatus::BLOCKED_JOIN:
            wakeupCore(tcxt.threadId);
            break;
          case ThreadStatus::INACTIVE:
          case ThreadStatus::COMPLETED:
            break;
          default:
            break;
        }
      }
      clock++;
      if(clock % 10000 == 0) wakeupDebugLog();
    }
    wakeuptime.push_back(std::make_pair(true, std::chrono::steady_clock::now()));
  }


};

#endif
