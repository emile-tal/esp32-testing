#include "sys_mode.h"

namespace sys {

Mode compute_next_mode(Mode current, const Event &e) {
  // Single writer principle prevents race conditions, inconsistent system
  // behavior and difficult debugging. In this setup, only this functin can
  // modify mode
  switch (current) {

  case Mode::IDLE:
    if (e.type == EventType::WIFI_START)
      return Mode::WIFI_CONNECTING;
    break;

  case Mode::WIFI_CONNECTING:
    if (e.type == EventType::WIFI_GOT_IP)
      return Mode::ONLINE;
    if (e.type == EventType::TIMEOUT)
      return Mode::ERROR;
    break;

  case Mode::ONLINE:
    if (e.type == EventType::WIFI_LOST)
      return Mode::ERROR;
    break;

  case Mode::ERROR:
    if (e.type == EventType::INTERNAL_RECOVERED)
      return Mode::IDLE;
    break;

  case Mode::OTA_UPDATE:
    // transitions added later
    break;
  }

  return current; // explicit "no transition"
}

} // namespace sys
