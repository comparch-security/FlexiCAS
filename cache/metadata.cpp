#include "cache/metadata.hpp"
#include <boost/format.hpp>

static boost::format fmt("%016x %016x %016x %016x %016x %016x %016x %016x");

std::string Data64B::to_string() const {
  fmt % data[0] % data[1] % data[2] % data[3] % data[4] % data[5] % data[6] % data[7];
  return fmt.str();
}
