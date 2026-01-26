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

static const char *TAG = "RGBLED";
static constexpr uint32_t period = 8000;
struct RgbLed {
  ledc_mode_t mode;
  ledc_timer_t timer;
  ledc_timer_bit_t resolution;
  uint32_t frequency;
  static constexpr int channel_count = 3;
  ledc_channel_t channels[channel_count];
  gpio_num_t gpios[channel_count];
  uint16_t gain_values[channel_count];
  uint8_t gamma_table[256];
};

struct Hsv {
  float h;
  float s;
  float v;
};

struct Rgb {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

Rgb hsv_to_rgb(const Hsv &hsv) {
  float h = fmod(hsv.h, 360);
  if (h < 0) {
    h += 360;
  }
  float s = hsv.s > 1 ? 1 : hsv.s < 0 ? 0 : hsv.s;
  float v = hsv.v > 1 ? 1 : hsv.v < 0 ? 0 : hsv.v;

  float c = v * s;
  float x = c * (1 - std::fabs(fmod(h / 60.0f, 2.0f) - 1));
  float m = v - c;

  uint8_t t = (c + m) * 255;
  uint8_t q = (x + m) * 255;
  uint8_t p = m * 255;

  int sector = (int)(h / 60.0f);

  switch (sector) {
  case 0:
    return {t, q, p};
  case 1:
    return {q, t, p};
  case 2:
    return {p, t, q};
  case 3:
    return {p, q, t};
  case 4:
    return {q, p, t};
  case 5:
    return {t, p, q};
  default:
    return {0, 0, 0};
  }
}

void initialize_gamma_table(RgbLed &rgb_led) {
  for (int i = 0; i < 256; i++) {
    rgb_led.gamma_table[i] = (uint8_t)(powf(i / 255.0f, 2.2) * 255.0f);
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

uint32_t get_color_duty(uint8_t color, ledc_timer_bit_t resolution) {
  return (uint32_t)color * duty_max_for(resolution) / 255;
}

void configure_ledc_timer(const RgbLed &rgb_led) {
  ledc_timer_config_t timer_conf = {};
  timer_conf.speed_mode = rgb_led.mode;
  timer_conf.timer_num = rgb_led.timer;
  timer_conf.duty_resolution = rgb_led.resolution;
  timer_conf.freq_hz = rgb_led.frequency;
  timer_conf.clk_cfg = LEDC_USE_APB_CLK;

  esp_err_t err = ledc_timer_config(&timer_conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure LEDC timer: %d", err);
  } else {
    ESP_LOGI(TAG, "LEDC timer configured: %u Hz, %d-bit",
             (unsigned)rgb_led.frequency, (int)rgb_led.resolution);
  }
}

void configure_ledc_channels(const RgbLed &rgb_led) {
  for (int i = 0; i < rgb_led.channel_count; i++) {
    ledc_channel_config_t ch_conf = {};
    ch_conf.speed_mode = rgb_led.mode;
    ch_conf.channel = rgb_led.channels[i];
    ch_conf.timer_sel = rgb_led.timer;
    ch_conf.intr_type = LEDC_INTR_DISABLE;
    ch_conf.gpio_num = rgb_led.gpios[i];
    ch_conf.duty = 0;
    ch_conf.hpoint = 0;

    esp_err_t err = ledc_channel_config(&ch_conf);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to configure LEDC channel: %d", err);
    } else {
      ESP_LOGI(TAG, "LEDC channel configured on GPIO %d",
               (int)rgb_led.gpios[i]);
    };
  }
}

uint8_t get_color_value(const Rgb &rgb_color, int channel_index) {
  switch (channel_index) {
  case 0:
    return rgb_color.r;
  case 1:
    return rgb_color.g;
  case 2:
    return rgb_color.b;
  }
  return 0;
}

void apply_color(const Rgb &rgb_color, const RgbLed &rgb_led) {
  for (int i = 0; i < rgb_led.channel_count; i++) {
    const uint8_t color_value = get_color_value(rgb_color, i);
    uint16_t scaled_value =
        ((uint32_t)color_value * rgb_led.gain_values[i]) >> 8;
    if (scaled_value > 255) {
      scaled_value = 255;
    }
    uint8_t corrected_value = rgb_led.gamma_table[scaled_value];
    const uint32_t duty_value =
        get_color_duty(corrected_value, rgb_led.resolution);
    esp_err_t err =
        ledc_set_duty(rgb_led.mode, rgb_led.channels[i], duty_value);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "ledc_set_duty failed: %d for channel %d", err,
               rgb_led.channels[i]);
    }
  }
  for (int i = 0; i < rgb_led.channel_count; i++) {
    ledc_update_duty(rgb_led.mode, rgb_led.channels[i]);
  }
}

void handle_rgb(void *pvParameter) {
  static long start_time = xTaskGetTickCount();
  RgbLed *led_ptr = (RgbLed *)pvParameter;
  // Get the pointer to the RgbLed struct (mandatory for
  // freeRTOS tasks to pass pointers as parameters)
  RgbLed &rgb_led = *led_ptr;
  // Dereference the pointer to get the reference to the
  // RgbLed struct, rgb_led is now a reference and can
  // safely be passed to other functions
  while (1) {
    long current_time = xTaskGetTickCount();
    long elapsed_time = pdTICKS_TO_MS(current_time - start_time);
    float hue = (float)(elapsed_time % period) * 360.0f / (float)period;
    Hsv hsv = {hue, 1, 0.3};
    Rgb rgb = hsv_to_rgb(hsv);
    apply_color(rgb, rgb_led);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

extern "C" void app_main(void) {

  static RgbLed rgb_led = {
      .mode = LEDC_LOW_SPEED_MODE,
      .timer = LEDC_TIMER_0,
      .resolution = LEDC_TIMER_10_BIT,
      .frequency = 1000,
      .channels = {LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2},
      .gpios = {GPIO_NUM_5, GPIO_NUM_4, GPIO_NUM_2},
      .gain_values = {256, 141, 179},
  };

  configure_ledc_timer(rgb_led);
  configure_ledc_channels(rgb_led);
  initialize_gamma_table(rgb_led);
  xTaskCreate(handle_rgb, "handle_rgb", 10000, &rgb_led, 5, NULL);
}