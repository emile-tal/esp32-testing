extern "C" {
void app_main(void); // Forward declaration with C linkage
}

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BLINK";
static constexpr gpio_num_t LED_GPIO = GPIO_NUM_8;

extern "C" void app_main(void) {
  gpio_config_t io_conf = {};
  io_conf.intr_type = GPIO_INTR_DISABLE;   // no interrupts
  io_conf.mode = GPIO_MODE_OUTPUT;         // output mode
  io_conf.pin_bit_mask = 1ULL << LED_GPIO; // which pin(s) to affect
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&io_conf);

  ESP_LOGI(TAG, "Configured GPIO%d as output for onboard LED", LED_GPIO);

  bool led_on = false;

  // 2) Blink forever
  while (true) {
    led_on = !led_on; // toggle state

    // Set the GPIO level: 1 = HIGH (LED on), 0 = LOW (LED off)
    gpio_set_level(LED_GPIO, led_on ? 1 : 0);

    ESP_LOGI(TAG, "LED is now %s", led_on ? "ON" : "OFF");

    // Sleep this task for 500 ms (let the OS run other stuff)
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}