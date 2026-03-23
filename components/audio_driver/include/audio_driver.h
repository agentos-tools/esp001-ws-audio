/**
 * ESP001 Audio Driver
 * 
 * I2S audio driver for ES7210 (ADC) and ES8311 (DAC)
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"

/* Audio configuration */
#define AUDIO_SAMPLE_RATE       16000
#define AUDIO_BITS_PER_SAMPLE   16
#define AUDIO_CHANNELS          1
#define AUDIO_FRAME_SIZE        320     /* 20ms at 16kHz */

/* I2C configuration */
#define I2C_MASTER_NUM          I2C_NUM_0
#define I2C_MASTER_SDA_IO       11
#define I2C_MASTER_SCL_IO       10
#define I2C_MASTER_FREQ_HZ      100000
#define I2C_MASTER_TX_BUF_LEN   0
#define I2C_MASTER_RX_BUF_LEN   0

/* ES7210 I2C address */
#define ES7210_I2C_ADDR         0x40

/* ES8311 I2C address */
#define ES8311_I2C_ADDR         0x18

/* I2S configuration */
#define I2S_NUM                 I2S_NUM_1  /* Changed from NUM_0 - ESP32-S3 only has NUM_1 */
#define I2S_MCLK_IO             12
#define I2S_BCLK_IO             13
#define I2S_WS_IO               14
#define I2S_DO_IO               16      /* DAC data input */
#define I2S_DI_IO               15      /* ADC data output */

/* DMA buffer configuration */
#define I2S_DMA_BUF_COUNT       8
#define I2S_DMA_BUF_LEN         1024

/* Expose I2C bus handle for use by other components (e.g., TCA9555) */
extern i2c_master_bus_handle_t i2c_bus_handle;

/**
 * Audio driver context
 */
typedef struct {
    i2s_chan_handle_t tx_handle;    /* I2S TX channel (playback) */
    i2s_chan_handle_t rx_handle;    /* I2S RX channel (capture) */
    bool initialized;
} audio_driver_t;

/**
 * Initialize audio driver
 * - Initialize I2C for ES7210/ES8311 communication
 * - Initialize I2S for audio capture and playback
 * - Configure ES7210 ADC
 * - Configure ES8311 DAC
 * 
 * @return ESP_OK on success
 */
esp_err_t audio_driver_init(void);

/**
 * Deinitialize audio driver
 * 
 * @return ESP_OK on success
 */
esp_err_t audio_driver_deinit(void);

/**
 * Read audio data from microphone (ES7210)
 * 
 * @param data Buffer to store audio data
 * @param len Length of data to read (in bytes)
 * @param bytes_read Actual bytes read
 * @param timeout_ms Timeout in milliseconds
 * @return ESP_OK on success
 */
esp_err_t audio_read(void *data, size_t len, size_t *bytes_read, uint32_t timeout_ms);

/**
 * Write audio data to speaker (ES8311)
 * 
 * @param data Audio data to write
 * @param len Length of data (in bytes)
 * @param bytes_written Actual bytes written
 * @param timeout_ms Timeout in milliseconds
 * @return ESP_OK on success
 */
esp_err_t audio_write(const void *data, size_t len, size_t *bytes_written, uint32_t timeout_ms);

/**
 * Set output volume
 * 
 * @param volume Volume level (0-100)
 * @return ESP_OK on success
 */
esp_err_t audio_set_volume(uint8_t volume);

/**
 * Get output volume
 * 
 * @param volume Pointer to store volume level (0-100)
 * @return ESP_OK on success
 */
esp_err_t audio_get_volume(uint8_t *volume);

/**
 * Start audio capture and playback
 * 
 * @return ESP_OK on success
 */
esp_err_t audio_start(void);

/**
 * Stop audio capture and playback
 * 
 * @return ESP_OK on success
 */
esp_err_t audio_stop(void);
