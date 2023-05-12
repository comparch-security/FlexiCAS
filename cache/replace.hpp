#ifndef CM_REPLACE_HPP_
#define CM_REPLACE_HPP_

#include "util/random.hpp"

///////////////////////////////////
// Base class

class ReplaceFuncBase
{
public:
  virtual uint32_t replace(uint32_t s, uint32_t *w) = 0;
  virtual void read(uint32_t ai, uint32_t s, uint32_t w) = 0;
  virtual void access(uint32_t ai, uint32_t s, uint32_t w) = 0;
  virtual void write(uint32_t ai, uint32_t s, uint32_t w) = 0;
  virtual void invalid(uint32_t ai, uint32_t s, uint32_t w) = 0;
  virtual ~ReplaceFuncBase() {}
};

#endif
