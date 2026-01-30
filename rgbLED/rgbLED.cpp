extern "C" {
void app_main(void); // Forward declaration with C linkage
}

#define _USE_MATH_DEFINES

#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
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
const char *ssid = CONFIG_WIFI_STA_SSID;
const char *password = CONFIG_WIFI_STA_PASSWORD;
static bool connected = false;
static QueueHandle_t color_queue = nullptr;

static wifi_config_t wifi_config = {};
static httpd_handle_t server = NULL;

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

// Forward declaration - allows parse_hex_color to be used before it's defined
bool parse_hex_color(const char *hex_color, Rgb &out);

static esp_err_t color_get_handler(httpd_req_t *req) {
  // Allocating 128 bytes on the stack (query is a fixed size character array)
  // Stores a C-string (null terminated text)
  char query[128];

  // Finds the query (everything after ?) in the url, copies to query (defined
  // above) and adds a \0 at the end (null terminator)
  // '\0' is a special character whose numeric value is 0, it is called a null
  // terminator and signals the end of a string in C. Without this, the code
  // will try to continue reading through memory assuming the string continues
  // past buffer
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query string");
    return ESP_OK;
  }

  // Creates a buffer just for the value (16 bytes) - max 15 characters (16th is
  // null terminator)
  char hex[16];

  // Looks for key-value (key being 'hex') in the query string and null
  // terminates
  if (httpd_query_key_value(query, "hex", hex, sizeof(hex)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ?hex=");
    return ESP_OK;
  }

  Rgb rgb;
  if (!parse_hex_color(hex, rgb)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad hex");
    return ESP_OK;
  }

  // Queue length is 1 — overwrite to replace previous value
  xQueueOverwrite(color_queue, &rgb);

  httpd_resp_sendstr(req, "OK\n");
  return ESP_OK;
}

static httpd_handle_t start_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 8080;

  httpd_handle_t server = NULL;
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return NULL;
  }

  httpd_uri_t color_uri = {};
  color_uri.uri = "/color";
  color_uri.method = HTTP_GET;
  color_uri.handler = color_get_handler;
  httpd_register_uri_handler(server, &color_uri);

  ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
  return server;
}

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
    if (server == NULL) {
      server = start_http_server();
    }
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
  // ESP_ERROR_CHECK(err);
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

  esp_wifi_set_ps(WIFI_PS_NONE);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  // set the wifi mode to STA (station)

  memset(&wifi_config, 0, sizeof(wifi_config));
  strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
  strncpy((char *)wifi_config.sta.password, password,
          sizeof(wifi_config.sta.password));
  //   wifi_config.sta.bssid_set = 1;
  //   uint8_t target_bssid[6] = {0x3C, 0x28, 0x6D, 0x54, 0xF5, 0x9B};
  //   memcpy(wifi_config.sta.bssid, target_bssid, 6);

  //   wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  //   wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  //   wifi_config.sta.pmf_cfg.capable = true;
  //   wifi_config.sta.pmf_cfg.required = false;
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

// Returning a boolean so that http can either send an error or continue
// Passing out as a parameter that is changed directly so that the rgb is
// returned too
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