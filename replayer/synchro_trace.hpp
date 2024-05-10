#ifndef CM_REPLAYER_SYNCHROTRACE_HPP
#define CM_REPLAYER_SYNCHROTRACE_HPP

#include "cache/coherence_multi.hpp"
#include "replayer/st_event.hpp"
#include "replayer/st_parser.hpp"
#include "cache/coherence.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
enum class ThreadStatus {
  INACTIVE,
  ACTIVE,
  WAIT_COMPUTE,
  WAIT_MEMORY,
  WAIT_THREAD,
  BLOCKED_COMM,
  BLOCKED_MUTEX,
  BLOCKED_BARRIER,
  BLOCKED_COND,
  BLOCKED_JOIN,
  COMPLETED,
  NUM_STATUSES
};

template <int BW>
struct ThreadContext
{
  ThreadID threadId;
  StEventID currEventId;
  StEventStream<BW> evStream;
  ThreadStatus status;
  uint64_t wakeupClock;

  ThreadContext(ThreadID threadId, const std::string& eventDir)
          : threadId{threadId}, currEventId{0}, 
            evStream{(ThreadID)(threadId+1), eventDir},
            status{ThreadStatus::INACTIVE},
            wakeupClock(0)
        {}

  // Note that a 'running' thread may be:
  // - blocked
  // - deadlocked
  //
  bool running() const { return status > ThreadStatus::INACTIVE && status < ThreadStatus::COMPLETED; }

  bool blocked() const { return status > ThreadStatus::ACTIVE && status < ThreadStatus::COMPLETED; }

  bool completed() const { return status == ThreadStatus::COMPLETED; }
};




// NT: num threads, CT: num cores, BW: buffer size
template <ThreadID NT, CoreID NC, int BW>
class SynchroTraceReplayer
{
private:

  int workerThreadCount;

  /** Abstract cpi estimation for integer ops */
  const float CPI_IOPS;

  /** Abstract cpi estimation for floating point ops */
  const float CPI_FLOPS;

  /** Cycles for pthread event */
  const uint32_t pthCycles;

  /** Cycles to simulate the time slice the scheduler gives to a thread */
  const uint32_t schedSliceCycles;

  /**************************************************************************
   * Synchronization state
  */
  StTracePthreadMetadata pthMetadata;

  std::vector<ThreadContext<BW>> threadContexts;

  std::vector<std::deque<std::reference_wrapper<ThreadContext<BW>>>>
        coreToThreadMap;

  /** stats: holds if thread can proceed past a barrier */
  std::vector<bool> perThreadBarrierBlocked;

  /** Holds which threads currently possess a mutex lock */
  std::vector<std::vector<uint64_t>> perThreadLocksHeld;

  /** Holds mutex locks in use */
  std::set<uint64_t> mutexLocks;

  /** Holds spin locks in use */
  std::set<uint64_t> spinLocks;

  /** Holds condition variables signaled by broadcasts and signals */
  std::vector<std::map<uint64_t, int>> condSignals;

  /** Holds which threads are waiting for a barrier */
  std::map<uint64_t, std::set<ThreadID>> threadBarrierMap;

  /** Directory of Sigil Traces and Pthread metadata file */
  std::string eventDir;

  /** clock */
  uint64_t clock;
  /** data cache */
  std::vector<CoreInterface *>& core_data;

public:
  SynchroTraceReplayer(std::string eventDir, float CPI_IOPS, float CPI_FLOPS, uint32_t pthCycles, uint32_t schedSliceCycles, std::vector<CoreInterface *>& core_data) 
  : eventDir(eventDir), CPI_IOPS(CPI_IOPS), CPI_FLOPS(CPI_FLOPS), pthCycles(pthCycles), perThreadLocksHeld(NT),
    schedSliceCycles(schedSliceCycles), clock(0), pthMetadata(eventDir), 
    core_data(core_data), coreToThreadMap(NC)
  {

  }

  const char* toString(ThreadStatus status) const{
    switch (status) 
    {
      case ThreadStatus::INACTIVE:
        return "INACTIVE";
      case ThreadStatus::ACTIVE:
        return "ACTIVE";
      case ThreadStatus::WAIT_THREAD:
        return "WAIT_THREAD";
      case ThreadStatus::WAIT_MEMORY:
        return "WAIT_MEMORY";
      case ThreadStatus::WAIT_COMPUTE:
        return "WAIT_COMPUTE";
      case ThreadStatus::BLOCKED_COMM:
        return "BLOCKED_COMM";
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

  uint64_t curClock() { return clock; }

  CoreID threadIdToCoreId(ThreadID threadId) const{
    return threadId % NC;
  }

  void init(){
    workerThreadCount = 0;

    for(ThreadID i = 0; i < NT; i++){
      threadContexts.emplace_back(i, eventDir);
    }
    for(auto& tcxt : threadContexts)
      coreToThreadMap.at(threadIdToCoreId(tcxt.threadId)).emplace_back(tcxt);

    // Set master (first) thread as active.
    // Schedule first tick of the initial core.
    // (the other cores begin 'inactive', and
    //  expect the master thread to start them)
    threadContexts[0].status = ThreadStatus::ACTIVE;
    workerThreadCount++;
  }

  /**
    * For a communication event, check to see if the producer has reached
    * the dependent event. This function also handles the case of a system
    * call. System calls are viewed as producer->consumer interactions with
    * the 'producer' system call having a ThreadID of 30000. For obvious
    * reasons, there are no 'dependencies' to enforce in the case of a system
    * call.
    */
  bool isCommDependencyBlocked(const MemoryRequest_ThreadCommunication& comm) const
  {
    // If the producer thread's EventID is greater than the dependent event
    // then the dependency is satisfied
    return (threadContexts[comm.sourceThreadId].currEventId > comm.sourceEventId);
  }

  uint64_t msgReqSend(CoreID coreId, uint64_t addr, uint32_t bytes, ReqType type){
    uint64_t delay = 0;
    if(type == ReqType::REQ_READ)
      core_data[coreId]->read(addr, &delay);
    else
      core_data[coreId]->write(addr, nullptr, &delay);
    return delay;
  }

  void processEventMarker(ThreadContext<BW>& tcxt, CoreID coreId)
  {
    tcxt.currEventId++;  // increment after, starts at 0
    tcxt.evStream.pop();
  }

  void processInsnMarker(ThreadContext<BW>& tcxt, CoreID coreId)
  {
    tcxt.evStream.pop();
  }

  void processEndMarker(ThreadContext<BW>& tcxt, CoreID coreId)
  {
    tcxt.status = ThreadStatus::COMPLETED;
    tcxt.evStream.pop();
  }


  void replayCompute(ThreadContext<BW>& tcxt, CoreID coreId)
  {
    assert(tcxt.evStream.peek().tag == Tag::COMPUTE);
    // simulate time for the iops/flops
    const ComputeOps& ops = tcxt.evStream.peek().computeOps;
    tcxt.wakeupClock = curClock() + 1 + CPI_IOPS * ops.iops + CPI_FLOPS * ops.flops;
    tcxt.status = ThreadStatus::WAIT_COMPUTE;
    tcxt.evStream.pop();
  }

  void replayMemory(ThreadContext<BW>& tcxt, CoreID coreId)
  {
    assert(tcxt.evStream.peek().tag == Tag::MEMORY);
    // Send the load/store
    const StEvent& ev = tcxt.evStream.peek();
    uint64_t delay = msgReqSend(coreId, ev.memoryReq.addr, ev.memoryReq.bytesRequested, ev.memoryReq.type);
    tcxt.wakeupClock = curClock() + delay;
    tcxt.status = ThreadStatus::WAIT_MEMORY;
    tcxt.evStream.pop();
  }

  void replayComm(ThreadContext<BW>& tcxt, CoreID coreId)
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
      tcxt.status = ThreadStatus::BLOCKED_COMM;
    }
  }

  void replayThreadAPI(ThreadContext<BW>& tcxt, CoreID coreID){
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
        }
        else
        {
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
        ThreadContext<BW>& workertcxt = threadContexts[workerThreadID];

        assert(workertcxt.status == ThreadStatus::INACTIVE);

        workertcxt.status = ThreadStatus::ACTIVE;
        const CoreID workerCoreID = {threadIdToCoreId(workerThreadID)};

        tcxt.status = ThreadStatus::WAIT_THREAD;
        tcxt.wakeupClock = curClock() + pthCycles;

        std::cout << "Thread " << workerThreadID << " created" << std::endl; 
        
        tcxt.evStream.pop();

        break;
      }
      case EventType::THREAD_JOIN:
      {
        assert(pthMetadata.addressToIdMap().find(pthAddr) != pthMetadata.addressToIdMap().cend());

        const ThreadContext<BW>& workertcxt = threadContexts[pthMetadata.addressToIdMap().at(pthAddr)];
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
        }
        else
        {
          tcxt.status = ThreadStatus::BLOCKED_BARRIER;
          tcxt.wakeupClock = curClock() + schedSliceCycles;
        }
        break;
      }
      case EventType::COND_WAIT:
      {
        const uint64_t mtx = {ev.threadApi.mutexLockAddr};
        const size_t erased = mutexLocks.erase(mtx);

        assert(erased);
        auto it = condSignals[tcxt.threadId].find(pthAddr);
        if(it != condSignals[tcxt.threadId].end() && it->second > 0)
        {
          // decrement signal and reactivate thread
          it->second--;
          tcxt.evStream.pop();
          tcxt.status = ThreadStatus::WAIT_THREAD;
          tcxt.wakeupClock = curClock() + pthCycles;
        }
        else
        {
          tcxt.status = ThreadStatus::BLOCKED_COND;
          tcxt.wakeupClock = curClock() + schedSliceCycles;
        }
        break;
      }
      case EventType::COND_SG:
      case EventType::COND_BR:
      {
        assert(tcxt.status == ThreadStatus::ACTIVE);
        // post condition signal to all threads
        for (ThreadID tid = 0; tid < NT; tid++)
        {
          // If the condition doesn't exist yet for the thread,
          // it will be inserted and value-initialized, so the
          // mapped_type (Addr) will default to `0`.
          condSignals[tid][pthAddr]++;
        }
        tcxt.evStream.pop();
        tcxt.status = ThreadStatus::WAIT_THREAD;
        tcxt.wakeupClock = curClock() + pthCycles;
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

  void wakeupCore(CoreID coreId){
    ThreadContext<BW>& tcxt = coreToThreadMap[coreId].front();
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

  void wakeupDebugLog(){
    printf("clock:%ld\n", curClock());
    for(const auto & cxt : threadContexts){
      printf("Thread<%d>:Event<%ld>:Status<%s>\n", cxt.threadId, cxt.currEventId, toString(cxt.status));
    }
  }

  void start(){
    while(true){
      if (std::all_of(threadContexts.cbegin(), threadContexts.cend(),
                      [](const ThreadContext<BW>& tcxt)
                      { return tcxt.completed(); })) 
        break;
      for(auto &tcxt : threadContexts)
      {
        switch (tcxt.status) 
        {
          case ThreadStatus::WAIT_COMPUTE:
          case ThreadStatus::WAIT_MEMORY:
          case ThreadStatus::WAIT_THREAD:
            if(curClock() == tcxt.wakeupClock){
              tcxt.status = ThreadStatus::ACTIVE;
            }
            break;
          case ThreadStatus::ACTIVE:
          case ThreadStatus::BLOCKED_COMM:
          case ThreadStatus::BLOCKED_MUTEX:
          case ThreadStatus::BLOCKED_BARRIER:
          case ThreadStatus::BLOCKED_COND:
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
      if(clock % 100000 == 0) wakeupDebugLog();
    }
  }


};


#endif