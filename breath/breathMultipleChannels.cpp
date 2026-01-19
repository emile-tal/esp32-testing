extern "C" {
void app_main(void); // Forward declaration with C linkage
}

#define _USE_MATH_DEFINES

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>

static const char *TAG = "BREATH";

static const gpio_num_t LED_GPIOS[] = {GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7};
static const ledc_channel_t LEDC_CHANNELS[] = {LEDC_CHANNEL_0, LEDC_CHANNEL_1,
                                               LEDC_CHANNEL_2};

static constexpr int LED_COUNT =
    sizeof(LED_GPIOS) /
    sizeof(LED_GPIOS[0]); // sizeof returns the number of memory bytes 'this'
                          // takes. ie. this is a smart way of always ensuring
                          // we have the right number of LEDs

static constexpr ledc_mode_t LEDC_MODE = LEDC_LOW_SPEED_MODE;
static constexpr ledc_timer_t LEDC_TIMER = LEDC_TIMER_0;
static constexpr ledc_timer_bit_t LEDC_RESOLUTION = LEDC_TIMER_10_BIT;
static constexpr uint32_t LEDC_FREQUENCY = 1000;

static constexpr double pi = M_PI;
static const long periods[] = {4000, 2000, 1000};

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

void configure_ledc_channel(ledc_channel_t channel, gpio_num_t gpio) {
  ledc_channel_config_t ch_conf = {};
  ch_conf.speed_mode = LEDC_MODE;
  ch_conf.channel = channel;
  ch_conf.timer_sel = LEDC_TIMER;
  ch_conf.intr_type = LEDC_INTR_DISABLE;
  ch_conf.gpio_num = gpio;
  ch_conf.duty = 0;
  ch_conf.hpoint = 0;

  esp_err_t err = ledc_channel_config(&ch_conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure LEDC channel: %d", err);
  } else {
    ESP_LOGI(TAG, "LEDC channel configured on GPIO %d", (int)gpio);
  }
}

void handle_breath(void *pvParameter) {
  const uint32_t max_duty = (1 << LEDC_RESOLUTION) - 1;
  while (1) {
    long current_time = xTaskGetTickCount();
    long elapsed_time = pdTICKS_TO_MS(current_time - start_time);
    for (int i = 0; i < LED_COUNT; i++) {
      double phase =
          2 * pi * ((double)(elapsed_time % periods[i]) / periods[i]);
      double brightness = (sin(phase) + 1) / 2;
      uint32_t duty = (uint32_t)(max_duty * brightness);
      // Set the duty cycle
      esp_err_t err = ledc_set_duty(LEDC_MODE, LEDC_CHANNELS[i], duty);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_set_duty failed: %d", err);
      } else {
        ledc_update_duty(LEDC_MODE, LEDC_CHANNELS[i]);
      }
    }

    // Wait for the next iteration
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

extern "C" void app_main(void) {
  static long start_time = xTaskGetTickCount();
  configure_ledc_timer();
  for (int i = 0; i < 3; i++) {
    configure_ledc_channel(LEDC_CHANNELS[i], LED_GPIOS[i]);
  }
  xTaskCreate(handle_breath, "handle_breath", 10000, NULL, 5, NULL);
}