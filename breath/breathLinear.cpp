extern "C" {
void app_main(void); // Forward declaration with C linkage
}

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BREATH";

static constexpr gpio_num_t LED_GPIO = GPIO_NUM_8;
static constexpr ledc_mode_t LEDC_MODE = LEDC_LOW_SPEED_MODE;
static constexpr ledc_timer_t LEDC_TIMER = LEDC_TIMER_0;
static constexpr ledc_channel_t LEDC_CHANNEL = LEDC_CHANNEL_0;
static constexpr ledc_timer_bit_t LEDC_RESOLUTION = LEDC_TIMER_10_BIT;
static constexpr uint32_t LEDC_FREQUENCY = 1000;

void configure_ledc_timer() {
  ledc_timer_config_t timer_conf = {};
  timer_conf.speed_mode = LEDC_MODE;
  timer_conf.timer_num = LEDC_TIMER;
  timer_conf.duty_resolution = LEDC_RESOLUTION;
  timer_conf.freq_hz = LEDC_FREQUENCY;
  timer_conf.clk_cfg = LEDC_AUTO_CLK;

  esp_err_t err = ledc_timer_config(&timer_conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure LEDC timer: %d", err);
  } else {
    ESP_LOGI(TAG, "LEDC timer configured: %u Hz, %d-bit",
             (unsigned)LEDC_FREQUENCY, (int)LEDC_RESOLUTION);
  }
}

void configure_ledc_channel() {
  ledc_channel_config_t ch_conf = {};
  ch_conf.speed_mode = LEDC_MODE;
  ch_conf.channel = LEDC_CHANNEL;
  ch_conf.timer_sel = LEDC_TIMER;
  ch_conf.intr_type = LEDC_INTR_DISABLE;
  ch_conf.gpio_num = LED_GPIO;
  ch_conf.duty = 0;
  ch_conf.hpoint = 0;

  esp_err_t err = ledc_channel_config(&ch_conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure LEDC channel: %d", err);
  } else {
    ESP_LOGI(TAG, "LEDC channel configured on GPIO %d", (int)LED_GPIO);
  }
}

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "Starting LEDC fixed-brightness demo");
  // Configure the LEDC timer and channel
  configure_ledc_timer();
  configure_ledc_channel();

  const uint32_t max_duty = (1 << LEDC_RESOLUTION) - 1;
  uint32_t duty = 0;
  int direction = 1;
  uint32_t step = 8;
  const uint32_t delay = 20;

  while (true) {
    esp_err_t err = ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "ledc_set_duty failed: %d", err);
    } else {
      ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
      ESP_LOGI(TAG, "Duty = %lu / %lu", (unsigned long)duty,
               (unsigned long)max_duty);
    }
    vTaskDelay(pdMS_TO_TICKS(delay));
    int32_t next = (int32_t)duty + step * direction;
    if (next <= 0) {
      next = 0;
      direction = 1;
    } else if (next >= (int32_t)max_duty) {
      next = max_duty;
      direction = -1;
    }
    duty = next;
  }
}