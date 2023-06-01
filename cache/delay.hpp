#ifndef CM_CACHE_DELAY_HPP
#define CM_CACHE_DELAY_HPP

class DelayBase
{
public:
  virtual void read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) = 0;
  virtual void write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) = 0;
  virtual void invalid(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool writeback, uint64_t *delay) = 0;
  virtual void probe(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool writeback, uint64_t *delay) = 0;
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
    *delay += hit ? dhit : dhit + dreplay;
  }

  virtual void write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) {
    *delay += hit ? dhit : dhit + dreplay;
  }

  virtual void invalid(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool writeback, uint64_t *delay) {
    *delay += writeback ? dhit + dtran : dhit;
  }

  virtual void probe(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool writeback, uint64_t *delay) {
    *delay += writeback ? dhit + dtran : dhit;
  }
};

// normal coherent cache delay estimation
// dhit:      latency for hit
// dtranUp:   block transfer latency to upper cache
// dtranDown: block transfer latency to lower cache
template<unsigned int dhit, unsigned int dtranUp, unsigned int dtranDown>
class DelayCoherentCache : public DelayBase
{
public:
  virtual void read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) {
    *delay += dhit + dtranUp;
  }

  // write delay is hidden
  virtual void write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) {}

  virtual void invalid(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool writeback, uint64_t *delay) {
    *delay += writeback ? dhit + dtranDown : dhit;
  }

  virtual void probe(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool writeback, uint64_t *delay) {
    *delay += writeback ? dhit + dtranDown : dhit;
  }
};

// memory delay estimation
template<unsigned int dtran>
class DelayMemory : public DelayBase
{
public:
  virtual void read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) {
    *delay += dtran;
  }

  // write delay is hidden
  virtual void write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) {}

private:
  // hidden
  virtual void invalid(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool writeback, uint64_t *delay) {}
  virtual void probe(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool writeback, uint64_t *delay) {}
};

#endif
