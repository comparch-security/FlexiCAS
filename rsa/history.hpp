#ifndef CM_RSA_HISTORY_HPP_
#define CM_RSA_HISTORY_HPP_

#include <unordered_map>
#include "util/concept_macro.hpp"

template<typename RT>
class HistoryRecorder {
  typedef typename std::conditional<C_VOID<RT>, void *, RT>::type map_record_t;
  const uint32_t len;
  std::unordered_map<uint64_t, std::pair<unsigned int, map_record_t> > rd_map;
  std::deque<uint64_t> rd_queue;
  uint64_t total, error;

  void pop() {
    auto key = rd_queue.front();
    rd_queue.pop_front();
    if(rd_map[key].first > 1)
      rd_map[key].first--;
    else
      rd_map.erase(key);
  }

public:
  HistoryRecorder(uint32_t history_length): len(history_length), total(0), error(0) {}

  map_record_t& insert(uint64_t key) {
    if(rd_queue.size() == len) pop();
    rd_queue.push_back(key); // do the insert
    if(!rd_map.count(key)) rd_map[key].first = 1;
    else                   rd_map[key].first++;
    return rd_map[key].second;
  }

  std::pair<bool, map_record_t*> count(uint64_t key) {
    bool rv = rd_map.count(key);
    if(rv) return std::make_pair(true, &(rd_map[key].second));
    else   return std::make_pair(false, nullptr);
  }

  void check(uint64_t key, bool hit) {
    total++;
    if(hit != rd_map.count(key)) error++;
  }

  double rate() const {
    return (double)(error)/total;
  }

  void clear() {
    rd_map.clear();
    rd_queue.clear();
    total = 0;
    error = 0;
  }
};

typedef HistoryRecorder<void> AddrHistoryRecorder;

#endif
