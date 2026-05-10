extern "C" {
void app_main(void);
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}
#include "driver/gpio.h"
#include "driver/uart.h"

static constexpr gpio_num_t I2S_SCK = GPIO_NUM_4; // BCLK
static constexpr gpio_num_t I2S_WS = GPIO_NUM_5;  // WS / LRCLK
static constexpr gpio_num_t I2S_SD = GPIO_NUM_6;  // DATA

static const char *TAG = "MICROPHONE";

static i2s_chan_handle_t rx_handle = nullptr;
// Driver uses dynamic allocation internally, so unavoidable

static constexpr int kSeconds = 5;
static constexpr int kSampleRate = 16000;
static constexpr int kChannels = 1;
static constexpr int kBitsPerSample = 16;
static constexpr int kTotalSamples = kSeconds * kSampleRate;
static constexpr int kChunkSamples = 256;

static int32_t raw_chunk[kChunkSamples];
static int16_t pcm_recording[kTotalSamples];

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

static inline int16_t sample32_to_16(int32_t sample) {
  int32_t shifted = sample >> 8; // Shift right by 8 bits to convert to 16-bit
  return clamp_int16(shifted);
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

// Passing pointer to data and length (number of bytes to write)
static void uart_write_all(const void *data, size_t len) {
  // This function is used because sometimes uart_write_bytes doesn't write all
  // the bytes in one call, so we need to loop until all bytes are written.

  const uint8_t *p = (const uint8_t *)data;
  // Getting access to bytes (p points to the first byte of the data)
  while (len > 0) {
    int written = uart_write_bytes(UART_NUM_0, (const char *)p, len);
    if (written > 0) {
      // when bytes are written, advance p by written and decrease len by
      // written
      p += written;
      len -= (size_t)written;
    } else {
      // if no bytes are written, delay 1 tick to yield to other tasks
      vTaskDelay(1);
    }
  }
  // Ensure bytes are physically transmitted before we continue/reset (2
  // seconds)
  uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(2000));
}

static void stream_pcm16_over_uart() {
  const int sample_rate = kSampleRate;
  const int channels = 1;
  const int samples = kTotalSamples;
  const int bytes = samples * (int)sizeof(int16_t);

  // 1) A clean header line the Mac script can parse
  printf("PCM16 %d %d %d\n", sample_rate, channels, samples);

  // 2) Raw binary PCM samples (little-endian int16)
  uart_write_all(pcm_recording, (size_t)bytes);

  // 3) A footer line (optional, but helpful for debugging)
  printf("\nDONE\n");
}

static void stream_pcm_only() {
  // Turn off all logs so nothing corrupts the binary stream
  esp_log_level_set("*", ESP_LOG_NONE);

  vTaskDelay(pdMS_TO_TICKS(200));

  fwrite(pcm_recording, 1, kTotalSamples * sizeof(int16_t), stdout);
  fflush(stdout);
}

static void record_task(void *arg) {
  (void)arg;
  read_mic_data();
  // stream_pcm16_over_uart();
  stream_pcm_only();
  vTaskDelete(nullptr);
}

extern "C" void app_main(void) {
  // spiffs_init();
  ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 2048, 0, 0, nullptr, 0));
  i2s_init_mic();
  xTaskCreate(record_task, "record_task", 10000, nullptr, 5, nullptr);
}