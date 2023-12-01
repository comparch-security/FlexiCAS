#include "cache/metadata.hpp"
#include <boost/format.hpp>

static boost::format data_fmt("%016x %016x %016x %016x %016x %016x %016x %016x");

std::string Data64B::to_string() const {
  data_fmt % data[0] % data[1] % data[2] % data[3] % data[4] % data[5] % data[6] % data[7];
  return data_fmt.str();
}

std::string CMMetadataBase::to_string() const {
  std::string str_state;
  switch(state) {
  case state_invalid:   str_state = "I"; break;
  case state_shared:    str_state = "S"; break;
  case state_modified:  str_state = "M"; break;
  case state_exclusive: str_state = "E"; break;
  case state_owned:     str_state = "O"; break;
  default:              str_state = "X";
  }

  return str_state + (is_dirty() ? "d" : "c") + (allow_write() ? "W" : "R");
}
