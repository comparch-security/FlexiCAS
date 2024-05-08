#ifndef CM_REPLAYER_SYNCHROTRACE_HPP
#define CM_REPLAYER_SYNCHROTRACE_HPP

#include "cache/coherence_multi.hpp"
#include "replayer/st_event.hpp"
#include "replayer/st_parser.hpp"
#include "cache/coherence.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
enum class ThreadStatus {
  INACTIVE,
  ACTIVE,
  WAIT_COMPUTE,
  WAIT_MEMORY,
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

  /** Abstract cpi estimation for integer ops */
  const float CPI_IOPS;

  /** Abstract cpi estimation for floating point ops */
  const float CPI_FLOPS;

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
  SynchroTraceReplayer(std::string eventDir, float CPI_IOPS, float CPI_FLOPS, std::vector<CoreInterface *>& core_data) 
  : eventDir(eventDir), CPI_IOPS(CPI_IOPS), CPI_FLOPS(CPI_FLOPS), clock(0), pthMetadata(eventDir), core_data(core_data)
    ,coreToThreadMap(NC)
  {

  }

  uint64_t curClock() { return clock; }

  CoreID threadIdToCoreId(ThreadID threadId) const{
    return threadId % NC;
  }

  void init(){
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
    return (threadContexts[comm.sourceThreadId-1].currEventId > comm.sourceEventId);
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
      tcxt.evStream.pop();
    }
    else
    {
      tcxt.status = ThreadStatus::BLOCKED_COMM;
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
            if(curClock() == tcxt.wakeupClock){
              tcxt.status = ThreadStatus::ACTIVE;
            }
            break;
          case ThreadStatus::ACTIVE:
          case ThreadStatus::BLOCKED_COMM:
            wakeupCore(tcxt.threadId);
            break;
          default:
            break;
        }
      }
      clock++;
    }
  }


};


#endif