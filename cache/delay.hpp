#ifndef CM_CACHE_DELAY_HPP
#define CM_CACHE_DELAY_HPP

class DelayBase
{
public:
  virtual void read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) = 0;
  virtual void write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) = 0;
  virtual void invalid(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, uint64_t *delay) = 0;
  virtual void probe(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool evict, uint64_t *delay) = 0;
};

// L1 delay estimation
// dhit:    latency for hit
// dreplay: latency for replay
// dtran:   block transfer latency between L1 and outer
template<unsigned int dhit, unsigned int dreplay, unsigned int dtran>
class DelayL1 : public DelayBase
{
public:
  virtual void read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) {
    *delay += hit ? dhit : dhit + dtran + dreplay;
  }

  virtual void write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) {
    *delay += hit ? dhit : dhit + dtran + dreplay;
  }

  virtual void invalid(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, uint64_t *delay) {
    *delay += dhit;
  }

  virtual void probe(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool evict, uint64_t *delay) {
    *delay += dhit;
  }
};


#endif
