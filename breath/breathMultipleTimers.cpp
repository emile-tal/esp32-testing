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

static const gpio_num_t LED_GPIOS[] = {GPIO_NUM_5, GPIO_NUM_2};
static const ledc_channel_t LEDC_CHANNELS[] = {LEDC_CHANNEL_0, LEDC_CHANNEL_1};

static constexpr int LED_COUNT =
    sizeof(LED_GPIOS) /
    sizeof(LED_GPIOS[0]); // sizeof returns the number of memory bytes 'this'
                          // takes. ie. this is a smart way of always ensuring
                          // we have the right number of LEDs

static constexpr ledc_mode_t LEDC_MODE = LEDC_LOW_SPEED_MODE;
static constexpr ledc_timer_t LEDC_TIMERS[] = {LEDC_TIMER_0, LEDC_TIMER_1};
static constexpr ledc_timer_bit_t LEDC_RESOLUTIONS[] = {LEDC_TIMER_10_BIT,
                                                        LEDC_TIMER_10_BIT};
static constexpr uint32_t LEDC_FREQUENCYS[] = {1000, 200};

static constexpr uint32_t duty_max_for(ledc_timer_bit_t res) {
  switch (res) {
  case LEDC_TIMER_5_BIT:
    return (1u << 5) - 1;
  case LEDC_TIMER_8_BIT:
    return (1u << 8) - 1;
  case LEDC_TIMER_10_BIT:
    return (1u << 10) - 1;
  default:
    return 0;
  }
}

static constexpr double pi = M_PI;
static const long period = 6000;

void configure_ledc_timer(ledc_timer_t timer, ledc_timer_bit_t resolution,
                          uint32_t frequency) {
  ledc_timer_config_t timer_conf = {};
  timer_conf.speed_mode = LEDC_MODE;
  timer_conf.timer_num = timer;
  timer_conf.duty_resolution = resolution;
  timer_conf.freq_hz = frequency;
  timer_conf.clk_cfg = LEDC_USE_APB_CLK;

  esp_err_t err = ledc_timer_config(&timer_conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure LEDC timer: %d", err);
  } else {
    ESP_LOGI(TAG, "LEDC timer configured: %u Hz, %d-bit", (unsigned)frequency,
             (int)resolution);
  }
}

void configure_ledc_channel(ledc_channel_t channel, gpio_num_t gpio,
                            ledc_timer_t timer) {
  ledc_channel_config_t ch_conf = {};
  ch_conf.speed_mode = LEDC_MODE;
  ch_conf.channel = channel;
  ch_conf.timer_sel = timer;
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
  static long start_time = xTaskGetTickCount();
  uint32_t
      max_dutys[LED_COUNT]; // Better to use actual max duty rather than enum
                            // values (unless you know what enum values are)

  for (int i = 0; i < LED_COUNT; i++) {
    max_dutys[i] = duty_max_for(LEDC_RESOLUTIONS[i]);
  }

  while (1) {
    long current_time = xTaskGetTickCount();
    long elapsed_time = pdTICKS_TO_MS(current_time - start_time);
    for (int i = 0; i < LED_COUNT; i++) {
      double phase = 2 * pi * ((double)(elapsed_time % period) / period);
      double brightness = (sin(phase) + 1) / 2;
      uint32_t duty = (uint32_t)(max_dutys[i] * brightness);
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
  for (int i = 0; i < LED_COUNT; i++) {
    configure_ledc_timer(LEDC_TIMERS[i], LEDC_RESOLUTIONS[i],
                         LEDC_FREQUENCYS[i]);
  }
  for (int i = 0; i < LED_COUNT; i++) {
    configure_ledc_channel(LEDC_CHANNELS[i], LED_GPIOS[i], LEDC_TIMERS[i]);
  }
  xTaskCreate(handle_breath, "handle_breath", 10000, NULL, 5, NULL);
}