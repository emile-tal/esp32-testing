#pragma once
// Ensures file isn't duplicated (prevent duplicate definition errors)

#include "sys_types.h"

namespace sys {
// Prevents name collision, everything within namespace sys becomes sys::...
struct Event {
  EventType type;
  uint32_t timestamp_ms;

  int32_t arg0; // generic payload
  int32_t arg1;
};

} // namespace sys
