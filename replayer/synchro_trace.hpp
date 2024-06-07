#ifndef CM_REPLAYER_SYNCHROTRACE_HPP
#define CM_REPLAYER_SYNCHROTRACE_HPP

#include "cache/coherence_multi.hpp"
#include "replayer/st_event.hpp"
#include "replayer/st_parser.hpp"
#include "replayer/thread.hpp"
#include "cache/coherence.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>

class SynchroTraceReplayerBase
{
protected:

 /**************************************************************************
   * Synchronization state
  */
  StTracePthreadMetadata pthMetadata;
  
  /** Holds each thread's context */
  std::vector<ThreadContext> threadContexts;

  /** Holds which threads currently wait for a lock */
  std::map<uint64_t, std::queue<ThreadID>> perMutexThread;

  /** Holds spin locks in use */
  std::set<uint64_t> spinLocks;

  /** Holds condition variables signaled by broadcasts and signals */
  std::vector<std::map<uint64_t, int>> condSignals;

  /** Holds which threads are waiting for a barrier */
  std::map<uint64_t, std::set<ThreadID>> threadBarrierMap;

  /** Holds which threads currently possess a mutex lock */
  std::vector<std::vector<uint64_t>> perThreadLocksHeld;

  /** Holds which threads waiting for the thread to complete */
  std::vector<std::vector<ThreadID>> joinList;

  /** Directory of Sigil Traces and Pthread metadata file */
  std::string eventDir;

  /** data cache */
  std::vector<CoreInterface *>& core_data;

  virtual bool isCommDependencyBlocked(const MemoryRequest_ThreadCommunication& comm) const = 0;
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
  virtual void wakeupDebugLog() = 0;

public:
  SynchroTraceReplayerBase(std::string eventDir, std::vector<CoreInterface *>& core_data) : 
   eventDir(eventDir), core_data(core_data), pthMetadata(eventDir) {

  }

  virtual void init()  = 0;
  virtual void start() = 0;
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
  virtual bool isCommDependencyBlocked(const MemoryRequest_ThreadCommunication& comm) const
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
    tcxt.evStream.pop();
    scheduler.getBlocked(tcxt, ThreadStatus::COMPLETED);
    for (ThreadID threadId : joinList[tcxt.threadId])
      scheduler.getReady(threadContexts[threadId]);
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

  virtual bool mutexTryLock(ThreadContext& tcxt, CoreID coreId, uint64_t pthaddr) {
    auto it = perMutexThread.find(pthaddr);
    if (it != perMutexThread.end()) {
      if (tcxt.status != ThreadStatus::ACTIVE_TRYLOCK) {
        it->second.push(tcxt.threadId);
      }
      return (tcxt.threadId == it->second.front());
    } else {
      perMutexThread[pthaddr].push(tcxt.threadId);
      return true;
    }
  }

  virtual void mutexUnlock(ThreadContext& tcxt, CoreID coreId, uint64_t pthaddr) {
    auto it = perMutexThread.find(pthaddr);
    assert(it != perMutexThread.end());
    assert(!(it->second.empty()));
    assert(it->second.front() == tcxt.threadId);
    it->second.pop();
    assert(it->second.front() != tcxt.threadId);
    if (!(it->second.empty())) {
      scheduler.getReady(threadContexts[it->second.front()]);
    }
  }

  virtual void replayThreadAPI(ThreadContext& tcxt, CoreID coreId) {
    assert(tcxt.evStream.peek().tag == Tag::THREAD_API);
    const StEvent& ev = tcxt.evStream.peek();
    const uint64_t pthAddr = ev.threadApi.pthAddr;

    switch (ev.threadApi.eventType) {
      case EventType::MUTEX_LOCK:
      {
        if (mutexTryLock(tcxt, coreId, pthAddr)) {
          perThreadLocksHeld[tcxt.threadId].push_back(pthAddr);
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
        std::vector<uint64_t>& locksHeld = perThreadLocksHeld[tcxt.threadId];
        auto v_it = std::find(locksHeld.begin(), locksHeld.end(), pthAddr);
        assert(v_it != locksHeld.end());
        locksHeld.erase(v_it);
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
        std::cout << "New Thread ID: " << workerThreadID << " " << threadContexts.size() << std::endl;
        assert(workerThreadID < threadContexts.size());
        assert(threadContexts[workerThreadID].status == ThreadStatus::INACTIVE);

        scheduler.getReady(threadContexts[workerThreadID]);
        
        tcxt.status = ThreadStatus::WAIT_THREAD;
        scheduler.schedule(tcxt, scheduler.pthCycles);
        
        tcxt.evStream.pop();

        break;
      }
      case EventType::THREAD_JOIN:
      {
        assert(pthMetadata.addressToIdMap().find(pthAddr) != pthMetadata.addressToIdMap().cend());

        const ThreadContext& workertcxt = threadContexts[pthMetadata.addressToIdMap().at(pthAddr)];
        if(workertcxt.completed()) {
          // reset to active, in case this thread was previously blocked
          tcxt.status = ThreadStatus::WAIT_THREAD;
          std::cout << "Thread " << workertcxt.threadId << " joined" << std::endl; 
          tcxt.evStream.pop();
          scheduler.schedule(tcxt, scheduler.pthCycles);
        } else if (workertcxt.running() || workertcxt.blocked()) {
          joinList[workertcxt.threadId].push_back(tcxt.threadId);
          scheduler.getBlocked(tcxt, ThreadStatus::BLOCKED_JOIN);
        } else {
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
        if(threadBarrierMap[pthAddr] == pthMetadata.barrierMap().at(pthAddr)) {
          for(auto tid : pthMetadata.barrierMap().at(pthAddr)) {
            scheduler.getReady(threadContexts[tid]);
            threadContexts[tid].evStream.pop();
          }
          assert(threadBarrierMap.erase(pthAddr));
          tcxt.status = ThreadStatus::WAIT_THREAD;
          scheduler.schedule(tcxt, scheduler.pthCycles);
        } else {
          scheduler.getBlocked(tcxt, ThreadStatus::BLOCKED_BARRIER);
        }
        break;
      }
      case EventType::COND_WAIT:
      {
        const uint64_t mtx = {ev.threadApi.mutexLockAddr};
        if (tcxt.status != ThreadStatus::ACTIVE_TRYLOCK)
          mutexUnlock(tcxt, coreId, mtx);

        auto it = condSignals[tcxt.threadId].find(pthAddr);
        if (it != condSignals[tcxt.threadId].end() && it->second > 0) {
          // decrement signal and reactivate thread
          if (mutexTryLock(tcxt, coreId, mtx)) {
            scheduler.getBlocked(tcxt, ThreadStatus::BLOCKED_COND);
          } else {
            it->second--;
            tcxt.evStream.pop();
            tcxt.status = ThreadStatus::WAIT_THREAD;
            scheduler.schedule(tcxt, scheduler.pthCycles);
          }
        } else {
          scheduler.getBlocked(tcxt, ThreadStatus::BLOCKED_COND);
        }
        break;
      }
      case EventType::COND_SG:
      case EventType::COND_BR:
      {
        assert(tcxt.status == ThreadStatus::ACTIVE || tcxt.status == ThreadStatus::ACTIVE_TRYLOCK);
        // post condition signal to all threads
        for (ThreadID tid = 0; tid < NT; tid++)
        {
          // If the condition doesn't exist yet for the thread,
          // it will be inserted and value-initialized, so the
          // mapped_type (Addr) will default to `0`.
          condSignals[tid][pthAddr]++;
          if (condSignals[tid][pthAddr] > 0 && threadContexts[tid].status == ThreadStatus::BLOCKED_COND)
            scheduler.getReady(threadContexts[tid]);
        }
        tcxt.evStream.pop();
        tcxt.status = ThreadStatus::WAIT_THREAD;
        scheduler.schedule(tcxt, scheduler.pthCycles);
        break;
      }
      case EventType::SPIN_LOCK:
      {
        assert(tcxt.status == ThreadStatus::ACTIVE);
        auto p = spinLocks.insert(pthAddr);
        if (p.second){
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
        
        auto it = spinLocks.find(pthAddr);
        if (it != spinLocks.end()) {
          spinLocks.erase(it);
          std::vector<uint64_t>& locksHeld = perThreadLocksHeld[tcxt.threadId];
          auto v_it = std::find(locksHeld.begin(), locksHeld.end(), pthAddr);
          assert(v_it != locksHeld.end());
          locksHeld.erase(v_it);
        }
        tcxt.evStream.pop();
        tcxt.status = ThreadStatus::WAIT_THREAD;
        scheduler.schedule(tcxt, scheduler.pthCycles);
        break;
      }
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

  virtual void wakeupCore(ThreadID threadId, CoreID coreId){
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

  virtual void wakeupDebugLog(){
    printf("clock:%ld\n", scheduler.curClock());
    for(const auto & cxt : threadContexts){
      printf("Thread<%d>:Event<%ld>:Status<%s>", cxt.threadId, cxt.currEventId, toString(cxt.status));
      if (scheduler.checkOnCore(cxt.threadId))
        printf(" OnCore=True\n");
      else
        printf(" OnCore=False\n");
    }
  }

public:
  SynchroTraceReplayer(std::string eventDir, float CPI_IOPS, float CPI_FLOPS, uint32_t cxtSwitchCycles, 
    uint32_t pthCycles, uint32_t schedSliceCycles, std::vector<CoreInterface *>& core_data) 
  : SynchroTraceReplayerBase(eventDir, core_data),
    scheduler(CPI_IOPS, CPI_FLOPS, cxtSwitchCycles, pthCycles, schedSliceCycles)
  {
    perThreadLocksHeld.resize(NT);
    joinList.resize(NT);
    condSignals.resize(NT);
  }

  virtual void init() {
    for(ThreadID i = 0; i < NT; i++){
      threadContexts.emplace_back(i, eventDir);
    }

    for(auto& tcxt : threadContexts)
      scheduler.init(tcxt);
    
    // Set master (first) thread as active.
    // Schedule first tick of the initial core.
    // (the other cores begin 'inactive', and
    //  expect the master thread to start them)
    scheduler.getReady(threadContexts[0]);
  }

  virtual void start(){
    while(true){
      if (std::all_of(threadContexts.cbegin(), threadContexts.cend(),
                      [](const ThreadContext& tcxt)
                      { return tcxt.completed(); })) 
        break;
      
      for(auto &tcxt : threadContexts) {
        switch (tcxt.status) 
        {
          case ThreadStatus::WAIT_LOCK:
            if(scheduler.curClock() == tcxt.wakeupClock){
              tcxt.status = ThreadStatus::ACTIVE_TRYLOCK;
            }
            break;
          case ThreadStatus::WAIT_COMM:
          case ThreadStatus::WAIT_COMPUTE:
          case ThreadStatus::WAIT_MEMORY:
          case ThreadStatus::WAIT_THREAD:
          case ThreadStatus::WAIT_SCHED:
            if(scheduler.curClock() == tcxt.wakeupClock){
              tcxt.status = ThreadStatus::ACTIVE;
            }
            break;
          case ThreadStatus::ACTIVE:
          case ThreadStatus::ACTIVE_TRYLOCK:  
          case ThreadStatus::BLOCKED_MUTEX:
          case ThreadStatus::BLOCKED_BARRIER:
          case ThreadStatus::BLOCKED_COND:
          case ThreadStatus::BLOCKED_JOIN:
          case ThreadStatus::INACTIVE:
          case ThreadStatus::COMPLETED:
            break;
          default:
            break;
        }
      }
      for (CoreID i = 0; i < NC; i++) {
        ThreadID tid = scheduler.findActive(i);
        if (tid >= 0)
          wakeupCore(tid, i);
      }

      if(scheduler.nextClock() % 100000 == 0) 
        wakeupDebugLog(); 
    }

    std::cout << "All threads completed at " << scheduler.curClock() << ".\n" << std::endl;
  }


};


#endif
