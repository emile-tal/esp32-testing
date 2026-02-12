#include "sys_manager.h"
#include "sys_event.h"
#include "sys_mode.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SYS_MGR";

namespace sys {

static Mode s_current_mode = Mode::IDLE;

static const char *mode_str(Mode m) {
  switch (m) {
  case Mode::IDLE:
    return "IDLE";
  case Mode::WIFI_CONNECTING:
    return "WIFI_CONNECTING";
  case Mode::ONLINE:
    return "ONLINE";
  case Mode::ERROR:
    return "ERROR";
  case Mode::OTA_UPDATE:
    return "OTA_UPDATE";
  default:
    return "UNKNOWN";
  }
}

static void system_manager_task(void *) {
  QueueHandle_t q = event_queue();
  Event e;

  ESP_LOGI(TAG, "System manager started, mode=IDLE");

  while (true) {
    if (xQueueReceive(q, &e, portMAX_DELAY) == pdTRUE) {

      Mode next = compute_next_mode(s_current_mode, e);

      if (next != s_current_mode) {
        ESP_LOGI(TAG, "MODE CHANGE: %s -> %s (event=%d)",
                 mode_str(s_current_mode), mode_str(next), (int)e.type);

        s_current_mode = next;
        // broadcast will be added next
      }
    }
  }
}

void start_system_manager() {
  xTaskCreate(system_manager_task, "system_manager", 4096, nullptr, 10,
              nullptr);
}

} // namespace sys
