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
#include <thread>

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
  std::mutex mutexMtx;

  /** Holds spin locks in use */
  std::set<uint64_t> spinLocks;
  std::mutex spinLocksMtx;

  /** Holds which threads are waiting for a barrier */
  std::map<uint64_t, std::set<ThreadID>> threadBarrierMap;
  std::mutex barrierMtx;

  /** Holds which threads currently possess a mutex lock */
  std::vector<std::vector<uint64_t>> perThreadLocksHeld;

  /** Holds which threads waiting for the thread to complete */
  std::vector<std::vector<ThreadID>> joinList;
  std::mutex joinMtx;

  /** Directory of Sigil Traces and Pthread metadata file */
  std::string eventDir;

  /** data cache */
  std::vector<CoreInterfaceBase *>& core_data;
  std::mutex cacheMtx;

  /** mutex for cout */
  std::mutex outputMtx;

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
    if (it != perMutexThread.end()) {
      if (tcxt.status != ThreadStatus::ACTIVE_TRYLOCK) {
        it->second.push(tcxt.threadId);
      }
      bool getMutex = (tcxt.threadId == it->second.front());

      mutexMtx.unlock();
      return getMutex;
    } else {
      perMutexThread[pthaddr].push(tcxt.threadId);

      mutexMtx.unlock();
      return true;
    }
  }

  virtual void mutexUnlock(ThreadContext& tcxt, CoreID coreId, uint64_t pthaddr) 
  {
    mutexMtx.lock();

    auto it = perMutexThread.find(pthaddr);
    assert(it != perMutexThread.end());
    assert(!(it->second.empty()));
    assert(it->second.front() == tcxt.threadId);
    it->second.pop();
    assert(((!it->second.empty()) && (it->second.front() != tcxt.threadId))
          || (it->second.empty()));
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

        outputMtx.lock();
        std::cout << "New Thread ID: " << workerThreadID << " " << threadContexts.size() << std::endl;
        outputMtx.unlock();

        assert(workerThreadID < threadContexts.size());
        assert(threadContexts[workerThreadID].status == ThreadStatus::INACTIVE);

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

        if(workertcxt.completed()) {

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
        if(threadBarrierMap[pthAddr] == pthMetadata.barrierMap().at(pthAddr)) {
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

  virtual void wakeupDebugLog(CoreID coreId){
    outputMtx.lock();
    printf("CoreID<%d>:clock:%ld\n", coreId, scheduler.curClock(coreId));
    for(const auto & cxt : threadContexts){
      if (scheduler.threadIdToCoreId(cxt.threadId) != coreId)
        continue;
      printf("Thread<%d>:Event<%ld>:Status<%s>", cxt.threadId, cxt.currEventId, toString(cxt.status));

      if (scheduler.checkOnCore(cxt.threadId))
        printf(" OnCore=True");
      else
        printf(" OnCore=False");

      if (cxt.waiting())
        printf(" wakeupClock=%lu\n", cxt.wakeupClock);
      else
        printf("\n");
    }
    outputMtx.unlock();
  }

  static void replay(CoreID coreId, SynchroTraceReplayer* replayer) {
    while(true){
      if (std::all_of(replayer->threadContexts.cbegin(), replayer->threadContexts.cend(),
                      [replayer, coreId](const ThreadContext& tcxt)
                      { return (replayer->scheduler.threadIdToCoreId(tcxt.threadId) != coreId) ||
                                tcxt.completed(); })) 
        break;
      
      for(auto &tcxt : replayer->threadContexts) {
        if (replayer->scheduler.threadIdToCoreId(tcxt.threadId) != coreId)
          continue;
        
        switch (tcxt.status) 
        {
          case ThreadStatus::WAIT_LOCK:
            if(replayer->scheduler.curClock(coreId) == tcxt.wakeupClock) {
              tcxt.status = ThreadStatus::ACTIVE_TRYLOCK;
            }
            break;
          case ThreadStatus::WAIT_COMPUTE:
          case ThreadStatus::WAIT_MEMORY:
          case ThreadStatus::WAIT_THREAD:
          case ThreadStatus::WAIT_SCHED:
          case ThreadStatus::WAIT_COMM:
            if(replayer->scheduler.curClock(coreId) == tcxt.wakeupClock) {
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
            if (replayer->scheduler.curClock(coreId) % 10 == 0 && replayer->scheduler.checkReady(tcxt.threadId))
              replayer->scheduler.getReady(tcxt, coreId);
            break;
          case ThreadStatus::COMPLETED:
          default:
            break;
        }

        if (replayer->scheduler.curClock(coreId) % 10 == 0) {
          replayer->scheduler.recordEvent(tcxt);
        }
      }

      ThreadID tid = replayer->scheduler.findActive(coreId);
      if (tid >= 0) {
        replayer->wakeupCore(tid, coreId);
      }

      if(replayer->scheduler.nextClock(coreId) % 1000000 == 0) {
        replayer->wakeupDebugLog(coreId);
      } 
    }

    replayer->outputMtx.lock();
    std::cout << "All threads on Core " << coreId << " completed at " << replayer->scheduler.curClock(coreId) << ".\n" << std::endl;
    replayer->outputMtx.unlock();
  }

public:
  SynchroTraceReplayer(std::string eventDir, float CPI_IOPS, float CPI_FLOPS, uint32_t cxtSwitchCycles, 
    uint32_t pthCycles, uint32_t schedSliceCycles, std::vector<CoreInterfaceBase *>& core_data) 
  : SynchroTraceReplayerBase(eventDir, core_data),
    scheduler(CPI_IOPS, CPI_FLOPS, cxtSwitchCycles, pthCycles, schedSliceCycles)
  {
    perThreadLocksHeld.resize(NT);
    joinList.resize(NT);
    replayCore.resize(NC);
  }

  virtual void init() {
    for (ThreadID i = 0; i < NT; i++){
      threadContexts.emplace_back(i, eventDir);
    }

    for(auto& tcxt : threadContexts)
      scheduler.init(tcxt);
    
    // Set master (first) thread as active.
    // Schedule first tick of the initial core.
    // (the other cores begin 'inactive', and
    //  expect the master thread to start them)
    threadContexts[0].restSliceCycles = scheduler.schedSliceCycles;
    scheduler.sendReady(0);
  }

  virtual void start() {
    // the main thread
    for (CoreID i = 0; i < NC; i++) {
      replayCore[i] = std::thread(std::bind(&(this->replay), i, this));
    }

    for (auto &c : replayCore) {
      c.join();
    }

    std::cout << "Replay Completed!" << std::endl;
  }
};


#endif
