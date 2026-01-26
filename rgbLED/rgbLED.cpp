extern "C" {
void app_main(void); // Forward declaration with C linkage
}

#define _USE_MATH_DEFINES

#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "string.h"
#include <cmath>

static const char *TAG = "RGBLED";
static bool connected = false;
static QueueHandle_t color_queue = nullptr;

static wifi_config_t wifi_config = {};

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

struct Rgb {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

// static const char *authmode_to_str(wifi_auth_mode_t a) {
//   switch (a) {
//   case WIFI_AUTH_OPEN:
//     return "OPEN";
//   case WIFI_AUTH_WEP:
//     return "WEP";
//   case WIFI_AUTH_WPA_PSK:
//     return "WPA";
//   case WIFI_AUTH_WPA2_PSK:
//     return "WPA2";
//   case WIFI_AUTH_WPA_WPA2_PSK:
//     return "WPA/WPA2";
//   case WIFI_AUTH_WPA2_ENTERPRISE:
//     return "WPA2-ENT";
//   case WIFI_AUTH_WPA3_PSK:
//     return "WPA3";
//   case WIFI_AUTH_WPA2_WPA3_PSK:
//     return "WPA2/WPA3";
//   default:
//     return "UNKNOWN";
//   }
// }

// static void wifi_scan_once() {
//   ESP_LOGI(TAG, "Starting WiFi scan...");
//   // Configure scan: scan all channels, include hidden networks if you want
//   wifi_scan_config_t scan_cfg = {};
//   scan_cfg.ssid = NULL;        // NULL = scan all SSIDs
//   scan_cfg.bssid = NULL;       // NULL = any BSSID
//   scan_cfg.channel = 0;        // 0 = all channels
//   scan_cfg.show_hidden = true; // include hidden SSIDs
//   // Blocking scan: this function will return when scan is complete
//   esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
//   if (err != ESP_OK) {
//     ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
//     return;
//   }
//   uint16_t ap_count = 0;
//   ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
//   ESP_LOGI(TAG, "Scan done. APs found: %u", (unsigned)ap_count);
//   if (ap_count == 0) {
//     return;
//   }
//   // Limit how many we print (keeps logs readable)
//   const uint16_t max_to_print = 20;
//   uint16_t to_fetch = (ap_count > max_to_print) ? max_to_print : ap_count;
//   wifi_ap_record_t ap_records[max_to_print];
//   memset(ap_records, 0, sizeof(ap_records));
//   ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&to_fetch, ap_records));
//   for (int i = 0; i < to_fetch; i++) {
//     // SSID is a uint8_t[33], safe to print as a string
//     // ESP_LOGI(TAG, "[%2d] SSID='%s' RSSI=%d CH=%d AUTH=%s", i,
//     //          (const char *)ap_records[i].ssid, (int)ap_records[i].rssi,
//     //          (int)ap_records[i].primary,
//     //          authmode_to_str(ap_records[i].authmode));
//     ESP_LOGI(TAG,
//              "[%2d] SSID='%s' RSSI=%d CH=%d AUTH=%s "
//              "BSSID=%02X:%02X:%02X:%02X:%02X:%02X",
//              i, (char *)ap_records[i].ssid, ap_records[i].rssi,
//              ap_records[i].primary, authmode_to_str(ap_records[i].authmode),
//              ap_records[i].bssid[0], ap_records[i].bssid[1],
//              ap_records[i].bssid[2], ap_records[i].bssid[3],
//              ap_records[i].bssid[4], ap_records[i].bssid[5]);
//   }
//   if (ap_count > max_to_print) {
//     ESP_LOGI(TAG, "... (printed %u of %u)", (unsigned)max_to_print,
//              (unsigned)ap_count);
//   }
// }

// static void wifi_scan_task(void *pv) {
//   // Wait a moment to let WiFi start cleanly
//   vTaskDelay(pdMS_TO_TICKS(1500));
//   // Scan a few times (useful if hotspot appears slightly later)
//   for (int i = 0; i < 3; i++) {
//     wifi_scan_once();
//     vTaskDelay(pdMS_TO_TICKS(3000));
//   }
//   ESP_LOGI(TAG, "Scan task done.");
//   vTaskDelete(NULL);
// }

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  // arg is an optional pointer provided when registering (user context)
  // event_base is the event family (WIFI uses WIFI_EVENT)
  // event_id defines the specific event
  // event_data is a pointer to a struct with the event data

  if (event_base == WIFI_EVENT) {
    if (event_id == WIFI_EVENT_STA_START) {
      ESP_LOGI(TAG, "Station started");
      ESP_ERROR_CHECK(esp_wifi_connect());
    }

    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
      wifi_event_sta_disconnected_t *evt =
          (wifi_event_sta_disconnected_t *)event_data;
      ESP_LOGI(TAG, "Station disconnected");
      ESP_LOGI(TAG, "Reason: %d", evt->reason);
      connected = false;
      esp_err_t e = esp_wifi_connect();
      ESP_LOGI(TAG, "esp_wifi_connect() -> %s", esp_err_to_name(e));
    }
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Station got IP");
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    connected = true;
  }
}

static void wifi_init_sta() {
  esp_err_t err = nvs_flash_init();
  // NVS = non-volatile storage
  // used to store configuration data that needs to persist across reboots
  // NVS is a simple key-value store that is stored in flash memory
  // NVS is a good choice for storing small amounts of data that need to persist
  // across reboots (quick and easy to access)
  // wifi uses NVS internally

  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  // Canonical pattern to handle nvs init errors (erase and re-flash)

  ESP_ERROR_CHECK(esp_netif_init()); // initialize the network interface
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  // default ESP-IDF event loop that wifi posts into

  esp_netif_create_default_wifi_sta();
  // creates default STA network interface object
  // includes network interface (eg. sta0), IP, DHCP server

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  // ESP-IDF macro that expands into a struct initialized with default values
  // for the wifi driver
  // includes task priorities, buffer sizes, internal stack sizes, feature
  // toggles, etc.

  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  // initialize the wifi driver with default init parameters
  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                      &event_handler, NULL, NULL);
  // register event handler for wifi events
  // WIFI_EVENT is the event type (constant provided by esp_wifi.h /
  // esp_event.h)
  // ESP_EVENT_ANY_ID is a wildcard event ID that matches any event
  // &event_handler is a pointer to the event handler function (ie. callback
  // function)
  // NULL is a pointer to the user data (optional)
  // NULL is handler instance output (optional)
  esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                      &event_handler, NULL, NULL);
  // register event handler for IP events as well

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  // set the wifi mode to STA (station)

  memset(&wifi_config, 0, sizeof(wifi_config));
  strncpy((char *)wifi_config.sta.ssid, "Demengetal1",
          sizeof(wifi_config.sta.ssid));
  strncpy((char *)wifi_config.sta.password, "Enfin11217",
          sizeof(wifi_config.sta.password));
  wifi_config.sta.bssid_set = 1;
  uint8_t target_bssid[6] = {0x3C, 0x28, 0x6D, 0x54, 0xF5, 0x9B};
  memcpy(wifi_config.sta.bssid, target_bssid, 6);

  wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  // set the wifi configuration

  ESP_ERROR_CHECK(esp_wifi_start());
  // start the wifi driver (begin broadcasting)

  // xTaskCreate(wifi_scan_task, "wifi_scan_task", 4096, NULL, 5, NULL);
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

int parse_hex_digit(char digit) {
  if (digit >= '0' && digit <= '9') {
    return digit - '0';
  }
  if (digit >= 'a' && digit <= 'f') {
    return digit - 'a' + 10;
  }
  if (digit >= 'A' && digit <= 'F') {
    return digit - 'A' + 10;
  }
  ESP_LOGE(TAG, "Invalid hex digit: %c", digit);
  return -1;
}

bool parse_hex_color(const char *hex_color, Rgb &out) {
  if (!hex_color) {
    ESP_LOGE(TAG, "Invalid hex color: %s", hex_color);
    return false;
  }
  while (*hex_color == ' ' || *hex_color == '\t' || *hex_color == '\n') {
    hex_color++;
  }

  if (*hex_color == '#') {
    hex_color++;
  }

  for (int i = 0; i < 6; i++) {
    if (!hex_color[i]) {
      ESP_LOGE(TAG, "Hex color too short");
      return false;
    }
  }

  int r1 = parse_hex_digit(hex_color[0]);
  int r2 = parse_hex_digit(hex_color[1]);
  int g1 = parse_hex_digit(hex_color[2]);
  int g2 = parse_hex_digit(hex_color[3]);
  int b1 = parse_hex_digit(hex_color[4]);
  int b2 = parse_hex_digit(hex_color[5]);

  if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0) {
    ESP_LOGE(TAG, "Invalid hex color");
    return false;
  }

  out.r = (r1 << 4) | r2;
  out.g = (g1 << 4) | g2;
  out.b = (b1 << 4) | b2;

  // using << 4 to shift the first digit to the left by 4 bits (same as
  // multiplying by 16 but in binary) | is used to combine the two digits into a
  // single byte (because second digit is goes in the lower 4 bits)
  // ie. | means combine independent bit fields
  // out.r = (r1 << 4) | r2 is the same as out.r = r1 * 16 + r2

  // example:
  // r1 = 0xA  → 1010
  // r2 = 0xF  →      1111
  // (r1 << 4) → 1010 0000
  // | r2      → 0000 1111
  // -------------------
  // result    → 1010 1111 = 0xAF

  return true;
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

uint32_t get_color_duty(uint8_t color, ledc_timer_bit_t resolution) {
  return (uint32_t)color * duty_max_for(resolution) / 255;
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
  RgbLed *led_ptr = (RgbLed *)pvParameter;
  // Get the pointer to the RgbLed struct (mandatory for
  // freeRTOS tasks to pass pointers as parameters)
  RgbLed &rgb_led = *led_ptr;
  // Dereference the pointer to get the reference to the
  // RgbLed struct, rgb_led is now a reference and can
  // safely be passed to other functions
  Rgb current_color = {255, 0, 0};
  while (1) {
    Rgb incoming;
    if (xQueueReceive(color_queue, &incoming, 0) == pdTRUE) {
      // pdTRUE is a freeRTOS constant that just means true
      current_color = incoming;
    }
    apply_color(current_color, rgb_led);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

extern "C" void app_main(void) {
  wifi_init_sta();
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
  color_queue = xQueueCreate(1, sizeof(Rgb));
  Rgb test_color = {0, 255, 0};
  xQueueSend(color_queue, &test_color, 0);
  xTaskCreate(handle_rgb, "handle_rgb", 10000, &rgb_led, 5, NULL);
}