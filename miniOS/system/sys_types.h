#pragma once
#include <stdint.h>

namespace sys {

enum class Mode : uint8_t {
  IDLE = 0,
  WIFI_CONNECTING,
  ONLINE,
  ERROR,
  OTA_UPDATE,
};

enum class EventType : uint8_t {
  BOOT,
  WIFI_START,
  WIFI_GOT_IP,
  WIFI_LOST,
  TIMEOUT,
  INTERNAL_ERROR,
  INTERNAL_RECOVERED,
  REQUEST_MODE_CHANGE,
};

} // namespace sys
