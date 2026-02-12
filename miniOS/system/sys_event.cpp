#include "sys_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static QueueHandle_t s_event_queue = nullptr;
// Static variable enforces single global event bus because no other file can
// touch this (encapsulation)
static const char *TAG = "SYS_EVENT";

namespace sys {

void init_event_bus() {
  s_event_queue = xQueueCreate(16, sizeof(Event));
  configASSERT(s_event_queue);
}

bool post_event(EventType type, int32_t arg0 = 0, int32_t arg1 = 0) {
  Event e{.type = type,
          .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
          .arg0 = arg0,
          .arg1 = arg1};

  BaseType_t ok = xQueueSend(s_event_queue, &e, 0);
  if (ok != pdTRUE) {
    ESP_LOGE(TAG, "Event queue full (lost event %d)", (int)type);
    return false;
  }
  return true;
}

QueueHandle_t event_queue() { return s_event_queue; }

} // namespace sys
