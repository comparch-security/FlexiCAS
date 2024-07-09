#ifndef CM_UTIL_DELAY_HPP
#define CM_UTIL_DELAY_HPP

class DelayBase
{
public:
  virtual void read(uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, uint64_t *delay) = 0;
  virtual void write(uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, uint64_t *delay) = 0;
  // probe, invalidate and writeback
  virtual void manage(uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, bool evict, bool writeback, uint64_t *delay) = 0;
};

// L1 delay estimation
// dhit:    latency for hit
// dreplay: latency for replay
// dtran:   block transfer latency between L1 and outer
template<unsigned int dhit, unsigned int dreplay, unsigned int dtran>
class DelayL1 : public DelayBase
{
public:
  virtual void read(uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, uint64_t *delay) override {
    *delay += hit ? dhit : dhit + dreplay;
  }

  virtual void write(uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, uint64_t *delay) override {
    *delay += hit ? dhit : dhit + dreplay;
  }

  virtual void manage(uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, bool evict, bool writeback, uint64_t *delay) override {
    *delay += (hit && writeback) ? dhit + dtran : dhit;
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
  virtual void read(uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, uint64_t *delay) override {
    *delay += dhit + dtranUp;
  }

  // write delay is hidden
  virtual void write(uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, uint64_t *delay) override {}

  virtual void manage(uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, bool evict, bool writeback, uint64_t *delay) override {
    *delay += (hit && writeback) ? dhit + dtranDown : dhit;
  }
};

// memory delay estimation
template<unsigned int dtran>
class DelayMemory : public DelayBase
{
public:
  virtual void read(uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, uint64_t *delay) override {
    *delay += dtran;
  }

  // write delay is hidden
  virtual void write(uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, uint64_t *delay) override {}

  // hidden
  virtual void manage(uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, bool evict, bool writeback, uint64_t *delay) override {}
};

#endif
