#ifndef CM_REPLAYER_STPARSER_HPP
#define CM_REPLAYER_STPARSER_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <regex>
#include <cassert>
#include <zlib.h>
#include <iostream>
#include <unordered_map>
#include <set>
#include <fstream>
#include "util/zfstream.hpp"
#include "st_event.hpp"

class StTraceParser
{
protected:
  /** Parse tokens */
  static constexpr char COMP_EVENT_TOKEN = '@';
  static constexpr char COMM_EVENT_TOKEN = '#';
  static constexpr char THREADAPI_EVENT_TOKEN = '^';
  static constexpr char MARKER_EVENT_TOKEN = '!';
  static constexpr char WRITE_ADDR_DELIM = '$';
  static constexpr char READ_ADDR_DELIM = '*';

public:
  StTraceParser(){

  }

  void parseTo(std::vector<StEvent>& buffer, std::string& line, ThreadID threadId, StEventID& eventId){
    switch (line[0])
    {
    case COMP_EVENT_TOKEN:
        buffer.emplace_back(StEvent::TraceEventMarkerTag, eventId);
        parseCompEventTo(buffer, line, threadId, eventId);
        ++eventId;
        break;
    case COMM_EVENT_TOKEN:
        buffer.emplace_back(StEvent::TraceEventMarkerTag, eventId);
        parseCommEventTo(buffer, line, threadId, eventId);
        ++eventId;
        break;
    case THREADAPI_EVENT_TOKEN:
        buffer.emplace_back(StEvent::TraceEventMarkerTag, eventId);
        parseThreadEventTo(buffer, line, threadId, eventId);
        ++eventId;
        break;
    case MARKER_EVENT_TOKEN:
        parseMarkerEventTo(buffer, line, threadId, eventId);
        break;
    default:
        printf("Invalid line detected in thread: %d\n", threadId);
    }
  }

private:
  void parseCompEventTo(std::vector<StEvent>& buffer, std::string& line, ThreadID threadId, StEventID eventId){
    // Compute events are iops/flops and intra-thread reads/writes that have
    // been combined into a single "event".
    // Example line:
    //
    // @ 661,0,100,44 $ 0x402b110 0x402b113
    // @ 661,0,100,44 * 0x402b110 0x402b113
    std::regex pattern(R"(@ (\d+),(\d+),(\d+),(\d+))");
    std::smatch matches;
    std::string::const_iterator line_start(line.cbegin());
    uint64_t iops, flops, reads, writes, start, end;
    if(std::regex_search(line_start, line.cend(), matches, pattern)){
      iops = std::stoi(matches[1]);
      flops = std::stoi(matches[2]);
      reads = std::stoi(matches[3]);
      writes = std::stoi(matches[4]);
      std::regex addr_pattern(R"( [\$\*] 0x([A-Fa-f0-9]+) 0x([A-Fa-f0-9]+))");
      line_start = matches.suffix().first;
      if(std::regex_search(line_start, line.cend(), matches, addr_pattern)){
        start = std::stoul(matches[1], nullptr, 16);
        end = std::stoul(matches[2], nullptr, 16);
      }
    }else{
        // TODO: add fatal match handle
    }
    if(reads == 0 && writes == 0){
      buffer.emplace_back(StEvent::ComputeTag, iops, flops);
    }else{
      ReqType type = writes > 0 ? ReqType::REQ_WRITE : ReqType::REQ_READ;
      // Create a compute sub-event to "wait" before issuing the
      // following memory request.
      buffer.emplace_back(StEvent::ComputeTag, iops, flops);
      
      switch (type) 
      {
        case ReqType::REQ_READ:
          buffer.emplace_back(StEvent::MemoryTag, MemoryRequest{start, end-start, ReqType::REQ_READ});
          break;
        case ReqType::REQ_WRITE:
          buffer.emplace_back(StEvent::MemoryTag, MemoryRequest{start, end-start, ReqType::REQ_WRITE});
          break;
        default:
          // TODO: handle this;
          break;
      }

    }

  }


  void parseCommEventTo(std::vector<StEvent>& buffer, std::string& line, ThreadID threadId, StEventID eventId){
    // For communication events, create read-based sub events for each
    // dependency.
    // Example line:
    //
    // # 5 8388778 0x403e290 0x403e297
    std::regex pattern(R"(# (\d+) (\d+) 0x([A-Fa-f0-9]+) 0x([A-Fa-f0-9]+))");

    std::smatch matches;
    if(std::regex_search(line, matches, pattern)){
      ThreadID  prodThreadId = std::stoi(matches[1])-1;
      uint64_t prodEventId = std::stoi(matches[2]);
      uint64_t addr = std::stoul(matches[3], nullptr, 16);
      uint64_t bytes = std::stoul(matches[4], nullptr, 16) - addr;

      buffer.emplace_back(StEvent::MemoryCommTag, addr, bytes, prodEventId, prodThreadId);
    }else{
        // TODO: add fatal match handle
      std::cerr << "match error" << std::endl;
      assert(0);
    }
  }
  void parseThreadEventTo(std::vector<StEvent>& buffer, std::string& line, ThreadID threadId, StEventID eventId){
    // example line:
    //
    // ^ 4^0xad97700
    //
    // or for conditional wait/signal special case:
    //
    // ^ 6^0xad97700&0xdeadbeef

    using IntegralType = std::underlying_type<EventType>::type;
    constexpr auto max = static_cast<IntegralType>(EventType::NUM_TYPES);

    std::regex pattern(R"(^\^ (\d+)\^0x([A-Fa-f0-9]+))");
    std::smatch matches;
    uint64_t typeVal, pthAddr;
    if(std::regex_search(line, matches, pattern)){
      typeVal = std::stoi(matches[1]);
      pthAddr = std::stoul(matches[2], nullptr, 16);
    }else{
      // TODO: add fatal match handle
      std::cerr << "match error" << std::endl;
      assert(0); 
    }

    // pthread event type
    assert(typeVal < max);
    const EventType type = static_cast<EventType>(typeVal);

    // the lock variable for the conditional signal/wait
    uint64_t mutexLockAddr = 0;
    std::regex patternM(R"(&0x([A-Fa-f0-9]+))");
    if (std::regex_search(line, matches, patternM)) mutexLockAddr = std::stoi(matches[1]);

    buffer.emplace_back(StEvent::ThreadApiTag, type, pthAddr, mutexLockAddr);
  }

  void parseMarkerEventTo(std::vector<StEvent>& buffer, std::string& line, ThreadID threadId, StEventID eventId){
    // example line:
    //
    // ! 4096

    uint64_t insns = 0;

    std::regex pattern(R"(! (\d+))");
    std::smatch match;
    if(std::regex_search(line, match, pattern)) 
      insns = std::stoi(match[1]);
    else{
      // TODO: add fatal match handle
      std::cerr << "match error" << std::endl;
      assert(0);
    }
    buffer.emplace_back(StEvent::InsnMarkerTag, insns);
  }


};

class StEventStream
{
protected:
  /** Trace attributes */
  ThreadID  threadId;
  StEventID lastEventIdParsed;
  StEventID lastLineParsed;

  /** File streams */
  std::string filename;
  std::string line;
  gzifstream traceFile;

  /** The actual parser and underlying buffer */
  StTraceParser parser;
  std::vector<StEvent> buffer;
  std::vector<StEvent>::const_iterator buffer_current;
  std::vector<StEvent>::const_iterator buffer_end;
  size_t eventsPerFill;
  size_t BufferSize;

public:
  StEventStream(ThreadID threadId, const std::string& eventDir, size_t BSize = 1024) : 
  threadId(threadId), filename(eventDir + "/sigil.events.out-" + std::to_string(threadId) + ".gz"), 
  lastLineParsed(0), traceFile(filename.c_str()), BufferSize(BSize){
    if(!traceFile){
      std::cerr << "Error opening file " << filename << std::endl;
      assert(0);
    }
    buffer.reserve(BufferSize);
    buffer_current = buffer.cbegin();
    buffer_end = buffer.cend();
    eventsPerFill = BufferSize/8;
    refill();
  }

  const StEvent& peek(){
    return *buffer_current;
  }

  void pop(){
    buffer_current++;
    if(buffer_current == buffer_end)
      refill();
  }

  /**
   * Get the current last line number parsed
   */
  StEventID getLineNo() const { return lastLineParsed; }
  StEventID getEventNo() const { return lastEventIdParsed; }


private:
    /**
     * Refills the buffer with more events from the trace file.
     */
  void refill(){
    buffer.clear();
    for(size_t i = 0; i < eventsPerFill; i++){
      if(std::getline(traceFile, line)){
        ++lastLineParsed;
        parser.parseTo(buffer, line, threadId, lastEventIdParsed);

      }else{
        buffer.emplace_back(StEvent::EndTag);
        break;
      }
    }
    buffer_current = buffer.cbegin();
    buffer_end = buffer.cend();
  }
};

/**
 * Parses the pthread metadata file, which contains information on thread
 * creation and barriers. Used for proper synchronization during replay.
 */
class StTracePthreadMetadata
{
    /** Parse tokens */
    static constexpr char ADDR_TO_TID_TOKEN = '#';
    static constexpr char BARRIER_TOKEN = '*';

    /** Map converting each slave thread's pthread address to thread ID */
    std::unordered_map<uint64_t, ThreadID> m_addressToIdMap;

    /** Holds barriers used in application */
    std::unordered_map<uint64_t, std::set<ThreadID>> m_barrierMap;

  public:
    StTracePthreadMetadata(const std::string& eventDir){
      std::string filename = eventDir + "/sigil.pthread.out";
      std::ifstream pthFile{filename};

      parsePthreadFile(pthFile);
    }

    const decltype(m_addressToIdMap)& addressToIdMap() const
    {
        return m_addressToIdMap;
    }

    const decltype(m_barrierMap)& barrierMap() const
    {
        return m_barrierMap;
    }

  private:
    /** Parses Sigil Pthread file for Pthread meta-data */
    void parsePthreadFile(std::ifstream& pthFile){
      assert(pthFile.good());

      std::string line;
      while (std::getline(pthFile, line))
      {
        assert(line.length() > 2);
        if (line[0] == ADDR_TO_TID_TOKEN &&
            line[1] == ADDR_TO_TID_TOKEN)
            parseAddressToID(line);
        else if (line[0] == BARRIER_TOKEN &&
                 line[1] == BARRIER_TOKEN)
            parseBarrierEvent(line);
      }
    }

    /** Creates pthread address to thread ID map */
    void parseAddressToID(std::string& line){
      // example:
      // ##80881984,2
      uint64_t pthAddr, threadId;
      std::regex pattern(R"(##(\d+),(\d+))");
      std::smatch matches;
      if(std::regex_search(line, matches, pattern)){
        pthAddr  = std::stoi(matches[1]);
        threadId = std::stoi(matches[2]); 
      }else{
        std::cerr << "match error" << std::endl;
        assert(0);
      }
      auto p = m_addressToIdMap.insert({pthAddr, threadId-1});
    }

    /** Creates set of barriers used in application */
    void parseBarrierEvent(std::string& line){
      // example:
      // **67117104,1,2,3,4,5,6,7,8,

      uint64_t pthAddr, threadId;
      std::regex pattern(R"(\*\*(\d+),([0-9,]+))");
      std::smatch matches;
      if(std::regex_search(line, matches, pattern)){
        pthAddr = std::stoi(matches[1]);
        std::string sub_str = matches[2];

        std::regex npattern(R"(\d+)");
        std::sregex_iterator next(sub_str.begin(), sub_str.end(), npattern);
        std::sregex_iterator end;

        while(next != end){
          std::smatch nmatch = *next;
          m_barrierMap[pthAddr].insert(std::stoi(nmatch.str())-1);
          next++;
        }
      }else{
        std::cerr << "match error" << std::endl;
        assert(0);
      }
    }
};


#endif