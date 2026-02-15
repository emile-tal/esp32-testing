extern "C" {
void app_main(void);
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}
#include "driver/gpio.h"

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

// WAV header struct
// struct __attribute__((packed)) WavHeader {
//   char riff[4];             // "RIFF"
//   uint32_t riff_size;       // 36 + data_size
//   char wave[4];             // "WAVE"
//   char fmt[4];              // "fmt "
//   uint32_t fmt_size;        // 16 (PCM)
//   uint16_t audio_format;    // 1 (PCM)
//   uint16_t num_channels;    // 1
//   uint32_t sample_rate;     // 16000
//   uint32_t byte_rate;       // sample_rate * channels * bits/8
//   uint16_t block_align;     // channels * bits/8
//   uint16_t bits_per_sample; // 16
//   char data[4];             // "data"
//   uint32_t data_size;       // num_samples * channels * bits/8
// };

// static void spiffs_init() {
//   // exposing SPI flash filesystem at /spiffs so standard C file APIs work
//   esp_vfs_spiffs_conf_t conf = {
//       .base_path = "/spiffs",
//       .partition_label = nullptr,
//       .max_files = 4,
//       .format_if_mount_failed = true,
//   };
//   ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

//   size_t total = 0, used = 0;
//   ESP_ERROR_CHECK(esp_spiffs_info(nullptr, &total, &used));
//   ESP_LOGI(TAG, "SPIFFS mounted: total=%u bytes, used=%u bytes",
//            (unsigned)total, (unsigned)used);
// }

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
  int32_t shifted =
      (int16_t)(sample >> 8); // Shift right by 8 bits to convert to 16-bit
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

    for (int i = 0; i < samples_read; i++) {
      int32_t sample = raw_chunk[i];
      int16_t pcm_sample = sample32_to_16(sample);
      pcm_recording[written_samples++] = pcm_sample;
    }
  }
  ESP_LOGI(TAG, "Recorded %d samples", written_samples);
}

// static esp_err_t write_wav_to_spiffs(const char *path, const int16_t *pcm,
//                                      int num_samples, int sample_rate) {
//   FILE *f = fopen(path, "wb");
//   if (!f) {
//     ESP_LOGE(TAG, "Failed to open %s for writing", path);
//     return ESP_FAIL;
//   }
//   const uint32_t data_size =
//       (uint32_t)(num_samples * kChannels * (kBitsPerSample / 8));
//   WavHeader hdr;
//   memcpy(hdr.riff, "RIFF", 4);
//   hdr.riff_size = 36 + data_size;
//   memcpy(hdr.wave, "WAVE", 4);
//   memcpy(hdr.fmt, "fmt ", 4);
//   hdr.fmt_size = 16;
//   hdr.audio_format = 1; // PCM
//   hdr.num_channels = kChannels;
//   hdr.sample_rate = (uint32_t)sample_rate;
//   hdr.bits_per_sample = kBitsPerSample;
//   hdr.block_align = (uint16_t)(kChannels * (kBitsPerSample / 8));
//   hdr.byte_rate = hdr.sample_rate * hdr.block_align;
//   memcpy(hdr.data, "data", 4);
//   hdr.data_size = data_size;
//   // Write header
//   size_t w1 = fwrite(&hdr, 1, sizeof(hdr), f);
//   if (w1 != sizeof(hdr)) {
//     ESP_LOGE(TAG, "Failed writing WAV header (wrote %u)", (unsigned)w1);
//     fclose(f);
//     return ESP_FAIL;
//   }
//   // Write PCM data
//   size_t w2 = fwrite(pcm, 1, data_size, f);
//   fclose(f);
//   if (w2 != data_size) {
//     ESP_LOGE(TAG, "Failed writing WAV data (wrote %u / %u)", (unsigned)w2,
//              (unsigned)data_size);
//     return ESP_FAIL;
//   }
//   ESP_LOGI(TAG, "WAV saved: %s (data=%u bytes, total=%u bytes)", path,
//            (unsigned)data_size, (unsigned)(data_size +
//            (uint32_t)sizeof(hdr)));
//   return ESP_OK;
// }

static void record_task(void *arg) {
  (void)arg;
  read_mic_data();
  // ESP_ERROR_CHECK(write_wav_to_spiffs("/spiffs/recording.wav", pcm_recording,
  //      kTotalSamples, kSampleRate));

  vTaskDelete(nullptr);
}

extern "C" void app_main(void) {
  // spiffs_init();
  i2s_init_mic();
  xTaskCreate(record_task, "record_task", 10000, nullptr, 5, nullptr);
}