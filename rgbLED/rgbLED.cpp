extern "C" {
void app_main(void); // Forward declaration with C linkage
}

#define _USE_MATH_DEFINES

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "RGBLED";

static const gpio_num_t LED_GPIOS[] = {GPIO_NUM_5, GPIO_NUM_4,
                                       GPIO_NUM_2}; // 5 - R, 4 - G, 2 - B
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

struct RGB {
  int r;
  int g;
  int b;
};

static constexpr RGB colors[] = {
    {255, 0, 0}, // Red
    {0, 255, 0}, // Green
    {0, 0, 255}, // Blue
};

int get_rgb_component(const RGB &color, int channel) {
  switch (channel) {
  case 0:
    return color.r;
  case 1:
    return color.g;
  case 2:
    return color.b;
  default:
    return 0;
  }
}

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

int get_color_duty(int color) {
  return color * duty_max_for(LEDC_RESOLUTION) / 255;
}

void configure_ledc_timer() {
  ledc_timer_config_t timer_conf = {};
  timer_conf.speed_mode = LEDC_MODE;
  timer_conf.timer_num = LEDC_TIMER;
  timer_conf.duty_resolution = LEDC_RESOLUTION;
  timer_conf.freq_hz = LEDC_FREQUENCY;
  timer_conf.clk_cfg = LEDC_USE_APB_CLK;

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

void handle_rgb(void *pvParameter) {
  int index = 0;
  while (1) {
    while (index < 3) {
      for (int i = 0; i < LED_COUNT; i++) {
        const RGB current_color = colors[index];
        int color_value = get_rgb_component(current_color, i);
        // Set the duty cycle
        esp_err_t err = ledc_set_duty(LEDC_MODE, LEDC_CHANNELS[i],
                                      get_color_duty(color_value));
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "ledc_set_duty failed: %d", err);
        } else {
          ledc_update_duty(LEDC_MODE, LEDC_CHANNELS[i]);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      index++;
    }
    index = 0;
  }
}

extern "C" void app_main(void) {
  configure_ledc_timer();
  for (int i = 0; i < LED_COUNT; i++) {
    configure_ledc_channel(LEDC_CHANNELS[i], LED_GPIOS[i]);
  }
  xTaskCreate(handle_rgb, "handle_rgb", 10000, NULL, 5, NULL);
}