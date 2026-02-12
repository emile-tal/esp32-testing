#pragma once
#include "sys_event.h"
#include "sys_types.h"

namespace sys {

Mode compute_next_mode(Mode current, const Event &e);

} // namespace sys
