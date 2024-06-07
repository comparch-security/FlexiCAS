#ifndef CM_REPLAYER_STEVENT_HPP
#define CM_REPLAYER_STEVENT_HPP

#include <cstdint>
#include <type_traits>

typedef int16_t ThreadID; 
using CoreID = ThreadID; 
using StEventID = std::size_t;

enum class Tag : uint8_t
{
  UNDEFINED,     // Initial value.

  /** Replay events */
  COMPUTE,       // Computation event, a combination of iops and flops.

  MEMORY,        // A memory request, either read or write.

  MEMORY_COMM,   // An inter-thread communication memory request,
                 // implicitly read

  THREAD_API,    // Calls to thread library such as thread creation,
                       // join, barriers, and locks

  /** Meta events */
  INSN_MARKER,   // Marks a specific number of machine instructions
                 // passed in the trace.

  TRACE_EVENT_MARKER,  // New event in trace.

  END_OF_EVENTS  // The last event in the event stream.
};

enum class EventType {
  INVALID_EVENT = 0,
  MUTEX_LOCK = 1,
  MUTEX_UNLOCK = 2,
  THREAD_CREATE = 3,
  THREAD_JOIN = 4,
  BARRIER_WAIT = 5,
  COND_WAIT = 6,
  COND_SG = 7,
  COND_BR = 8,
  SPIN_LOCK = 9,
  SPIN_UNLOCK = 10,
  SEM_INIT = 11,
  SEM_WAIT = 12,
  SEM_POST = 13,
  SEM_GETV = 14,
  SEM_DEST = 15,
  NUM_TYPES,
};

const char* EventToString(EventType type) {
  switch (type) 
  {
    case EventType::INVALID_EVENT:
      return "INVALID_EVENT";
    case EventType::MUTEX_LOCK:
      return "MUTEX_LOCK";
    case EventType::MUTEX_UNLOCK:
      return "MUTEX_UNLOCK";
    case EventType::THREAD_CREATE:
      return "THREAD_CREATE";
    case EventType::THREAD_JOIN:
      return "THREAD_JOIN";
    case EventType::BARRIER_WAIT:
      return "BARRIER_WAIT";
    case EventType::COND_WAIT:
      return "COND_WAIT";
    case EventType::COND_SG:
      return "COND_SG";
    case EventType::COND_BR:
      return "COND_BR";
    case EventType::SPIN_LOCK:
      return "SPIN_LOCK";
    case EventType::SPIN_UNLOCK:
      return "SPIN_UNLOCK";
    default:
      std::cerr << "Unexpected Event Type" << std::endl;
      assert(0);
  }
}

/** Read/Write */
enum class ReqType: uint8_t {
    REQ_READ,
    REQ_WRITE
};

/**
 * A memory request for the SynchroTrace...
 */
struct MemoryRequest {
  /** Physical address */
  uint64_t addr;

  /** Size of the request starting at the address */
  uint64_t bytesRequested;

  ReqType type;
};


struct MemoryRequest_ThreadCommunication {
  /** Physical address */
  uint64_t addr;

  /** Size of the request starting at the address */
  uint32_t bytesRequested;

  /** Event ID for the sub event that this request is linked to */
  uint64_t sourceEventId;

  /** Thread ID corresponding to the trace used to generate this request */
  ThreadID sourceThreadId;
};


struct ComputeOps {
  uint32_t iops;
  uint32_t flops;
};

struct ThreadApi {
    /**
     * Address of the critical variable used in Pthread calls, for e.g. the
     * mutex lock address, barrier variable address, conditional variable
     * address, or address of the input variable that holds the thread
     * information when creating a new thread
     */
  uint64_t pthAddr;

    /**
     * Mutex lock address used in conjunction with conditional variable
     * address set in pthAddr
     */
  uint64_t mutexLockAddr;

    /**
     * The type within the THREAD_API class of events
     *
     * INVALID_EVENT    Initialization value
     *
     * MUTEX_LOCK       Mutex lock event simulating lock acquire
     *
     * MUTEX_UNLOCK     Mutex unlock event simulating lock release
     *
     * THREAD_CREATE    New thread creation event
     *
     * THREAD_JOIN      Thread join
     *
     * BARRIER_WAIT     Synchronisation barrier
     *
     * COND_WAIT        Pthread condition wait
     *
     * COND_SG          Pthread condition signal
     *
     * SPIN_LOCK        Pthread spin lock
     *
     * SPIN_UNLOCK      Pthread spin unlock
     *
     * SEM_INIT         Initialise a semaphore
     *
     * SEM_WAIT         Block on a semaphore count
     *
     * SEM_POST         Increment a semaphore
     *
     */
  EventType eventType;
};

struct End {
};

struct InsnMarker {
  /** number of instructions this marker represents */
  uint64_t insns;
};


/**
 * The original event id in the trace file.
 * Multiple replay events can be generated from the same event in the trace.
 * This marker groups replay events that were generated from the same trace
 * event.
 */
struct TraceEventMarker {
  const StEventID eventId;
};

struct StEvent{
  /**
  * Constants for tagged dispatch constructors.
  */
  using ComputeTagType = std::integral_constant<Tag, Tag::COMPUTE>;
  static constexpr auto ComputeTag = ComputeTagType{};

  using MemoryTagType = std::integral_constant<Tag, Tag::MEMORY>;
  static constexpr auto MemoryTag = MemoryTagType{};

  using MemoryCommTagType = std::integral_constant<Tag, Tag::MEMORY_COMM>;
  static constexpr auto MemoryCommTag = MemoryCommTagType{};

  using ThreadApiTagType = std::integral_constant<Tag, Tag::THREAD_API>;
  static constexpr auto ThreadApiTag = ThreadApiTagType{};

  using InsnMarkerTagType = std::integral_constant<Tag, Tag::INSN_MARKER>;
  static constexpr auto InsnMarkerTag = InsnMarkerTagType{};

  using TraceEventMarkerTagType = std::integral_constant<Tag, Tag::TRACE_EVENT_MARKER>;
  static constexpr auto TraceEventMarkerTag = TraceEventMarkerTagType{};

  using EndTagType = std::integral_constant<Tag, Tag::END_OF_EVENTS>;
  static constexpr auto EndTag = EndTagType{};

  const Tag tag = Tag::UNDEFINED;

  union
  {
    ComputeOps                        computeOps;
    MemoryRequest                     memoryReq;
    MemoryRequest_ThreadCommunication memoryReqComm;
    ThreadApi                         threadApi;
    InsnMarker                        insnMarker;
    TraceEventMarker                  traceEventMarker;
    End                               end;
  };

    /**
     * Use tagged dispatched constructors to create the variant.
     * Allow direct construction instead of static builder methods that may
     * cause additional copies/moves
     */
    StEvent(ComputeTagType,
            uint32_t iops,
            uint32_t flops) noexcept
      : computeOps{iops, flops},
        tag{Tag::COMPUTE}
    {}

    StEvent(MemoryTagType,
            const MemoryRequest& memReq) noexcept
      : memoryReq{memReq},
        tag{Tag::MEMORY}
    {}

    StEvent(MemoryCommTagType,
            uint64_t addr,
            uint32_t bytes,
            StEventID sourceEventId,
            ThreadID sourceThreadId) noexcept
      : memoryReqComm{addr, bytes, sourceEventId, sourceThreadId},
        tag{Tag::MEMORY_COMM}
    {}

    StEvent(ThreadApiTagType,
            EventType type,
            uint64_t pthAddr,
            uint64_t mutexLockAddr) noexcept
      : threadApi{pthAddr, mutexLockAddr, type},
        tag{Tag::THREAD_API}
    {}

    StEvent(InsnMarkerTagType,
            uint64_t insns) noexcept
      : insnMarker{insns},
        tag{Tag::INSN_MARKER}
    {}

    StEvent(TraceEventMarkerTagType,
            StEventID eventId) noexcept
      : traceEventMarker{eventId},
        tag{Tag::TRACE_EVENT_MARKER}
    {}

    StEvent(EndTagType) noexcept
      : end{},
        tag{Tag::END_OF_EVENTS}
    {}
};


#endif