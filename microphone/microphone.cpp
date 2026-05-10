extern "C" {
void app_main(void);
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
}
#include "driver/gpio.h"
#include <string.h>

static constexpr gpio_num_t I2S_SCK = GPIO_NUM_4; // BCLK
static constexpr gpio_num_t I2S_WS = GPIO_NUM_5;  // WS / LRCLK
static constexpr gpio_num_t I2S_SD = GPIO_NUM_6;  // DATA

static constexpr uint16_t kTcpPort = 3333;

static const char *TAG = "MICROPHONE";

static i2s_chan_handle_t rx_handle = nullptr;

static constexpr int kSeconds = 5;
static constexpr int kSampleRate = 16000;
static constexpr int kChannels = 1;
static constexpr int kBitsPerSample = 16;
static constexpr int kTotalSamples = kSeconds * kSampleRate;
static constexpr int kChunkSamples = 256;

static int32_t raw_chunk[kChunkSamples];
static int16_t pcm_recording[kTotalSamples];

static EventGroupHandle_t s_wifi_event_group = nullptr;
static constexpr int WIFI_CONNECTED_BIT = BIT0;

static void i2s_init_mic() {
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle));

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                      I2S_SLOT_MODE_MONO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = I2S_SCK,
              .ws = I2S_WS,
              .dout = I2S_GPIO_UNUSED,
              .din = I2S_SD,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
  ESP_LOGI(TAG, "INMP441 microphone initialized");
}

static inline int16_t clamp_int16(int32_t x) {
  if (x > 32767)
    return 32767;
  if (x < -32768)
    return -32768;
  return (int16_t)x;
}

// INMP441 emits 24-bit signed audio MSB-justified in a 32-bit slot.
// Shift right by 14 to keep the top 18 bits then clamp to int16 — gives
// reasonable headroom for normal speech without the heavy clipping that
// >> 8 caused.
static inline int16_t sample32_to_16(int32_t sample) {
  return clamp_int16(sample >> 14);
}

static void read_mic_data() {
  int written_samples = 0;
  while (written_samples < kTotalSamples) {
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(rx_handle, raw_chunk, sizeof(raw_chunk),
                                     &bytes_read, portMAX_DELAY);

    if (err != ESP_OK) {
      ESP_LOGE(TAG, "i2s_channel_read failed: %s", esp_err_to_name(err));
      continue;
    }

    int samples_read = (int)(bytes_read / sizeof(int32_t));
    if (samples_read <= 0) {
      continue;
    }

    for (int i = 0; i < samples_read && written_samples < kTotalSamples; i++) {
      int32_t sample = raw_chunk[i];
      int16_t pcm_sample = sample32_to_16(sample);
      pcm_recording[written_samples++] = pcm_sample;
    }
  }
  ESP_LOGI(TAG, "Recorded %d samples", written_samples);
}

static bool send_all(int sock, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  while (len > 0) {
    int sent = send(sock, p, len, 0);
    if (sent < 0) {
      ESP_LOGE(TAG, "send failed: errno %d", errno);
      return false;
    }
    p += sent;
    len -= (size_t)sent;
  }
  return true;
}

static void serve_one_client(int listen_sock) {
  struct sockaddr_in client_addr;
  socklen_t addr_len = sizeof(client_addr);
  int client = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
  if (client < 0) {
    ESP_LOGE(TAG, "accept failed: errno %d", errno);
    return;
  }

  char ip_str[INET_ADDRSTRLEN] = {0};
  inet_ntoa_r(client_addr.sin_addr, ip_str, sizeof(ip_str));
  ESP_LOGI(TAG, "Client connected from %s", ip_str);

  char header[64];
  int header_len = snprintf(header, sizeof(header), "PCM16 %d %d %d\n",
                            kSampleRate, kChannels, kTotalSamples);

  bool ok = send_all(client, header, (size_t)header_len);
  if (ok) {
    ok = send_all(client, pcm_recording,
                  (size_t)kTotalSamples * sizeof(int16_t));
  }
  if (ok) {
    ESP_LOGI(TAG, "Sent %d bytes of PCM",
             (int)(kTotalSamples * sizeof(int16_t)));
  }
  close(client);
}

static void stream_pcm_over_tcp() {
  int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_sock < 0) {
    ESP_LOGE(TAG, "socket() failed: errno %d", errno);
    return;
  }

  int opt = 1;
  setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(kTcpPort);

  if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind failed: errno %d", errno);
    close(listen_sock);
    return;
  }

  if (listen(listen_sock, 1) < 0) {
    ESP_LOGE(TAG, "listen failed: errno %d", errno);
    close(listen_sock);
    return;
  }

  ESP_LOGI(TAG, "Listening on TCP port %d — connect with the capture script",
           kTcpPort);

  while (true) {
    serve_one_client(listen_sock);
    ESP_LOGI(TAG, "Re-recording for next client...");
    read_mic_data();
  }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    ESP_LOGW(TAG, "Wi-Fi disconnected, retrying...");
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

static void wifi_init_sta() {
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

  wifi_config_t wifi_config = {};
  strncpy((char *)wifi_config.sta.ssid, CONFIG_WIFI_STA_SSID,
          sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char *)wifi_config.sta.password, CONFIG_WIFI_STA_PASSWORD,
          sizeof(wifi_config.sta.password) - 1);

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "Connecting to SSID '%s'...", CONFIG_WIFI_STA_SSID);
  xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                      portMAX_DELAY);
  ESP_LOGI(TAG, "Wi-Fi connected");
}

static void record_task(void *arg) {
  (void)arg;
  read_mic_data();
  stream_pcm_over_tcp();
  vTaskDelete(nullptr);
}

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  wifi_init_sta();
  i2s_init_mic();
  xTaskCreate(record_task, "record_task", 10000, nullptr, 5, nullptr);
}
