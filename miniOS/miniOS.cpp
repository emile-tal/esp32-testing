extern "C" {
void app_main(void); // Forward declaration with C linkage
}

#include "sys_event.h"
#include "sys_manager.h"

extern "C" void app_main(void) {
  sys::init_event_bus();
  sys::start_system_manager();

  // Simulate boot
  sys::post_event(sys::EventType::BOOT);

  // Simulate Wi-Fi lifecycle (temporary)
  sys::post_event(sys::EventType::WIFI_START);
}
