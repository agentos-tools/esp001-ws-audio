/**
 * @file opus_codec.h
 * @brief Opus audio codec wrapper for ESP32-S3
 * 
 * Provides encoder and decoder for Opus codec optimized for:
 * - Sample rate: 16000 Hz
 * - Channels: 1 (Mono)
 * - Frame duration: 20ms (320 samples)
 * - Bitrate: 16 kbps
 * - Complexity: 5 (ESP32-S3 recommended)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opus codec sample rate
 */
#define OPUS_SAMPLE_RATE         16000

/**
 * Opus codec channels (mono)
 */
#define OPUS_CHANNEL_NUM         1

/**
 * Frame duration in milliseconds
 */
#define OPUS_FRAME_DURATION_MS   20

/**
 * Samples per frame (16kHz * 20ms = 320)
 */
#define OPUS_SAMPLES_PER_FRAME   (OPUS_SAMPLE_RATE * OPUS_FRAME_DURATION_MS / 1000)

/**
 * Bytes per PCM frame (320 samples * 2 bytes * 1 channel = 640)
 */
#define OPUS_PCM_FRAME_SIZE      (OPUS_SAMPLES_PER_FRAME * sizeof(int16_t) * OPUS_CHANNEL_NUM)

/**
 * Maximum encoded bytes per frame
 * Opus max bitrate ~120kbps, for 20ms frame = 300 bytes max
 * We use 400 bytes to be safe
 */
#define OPUS_MAX_ENCODED_SIZE    400

/**
 * Opus encoder handle
 */
typedef struct opus_encoder *opus_encoder_handle_t;

/**
 * Opus decoder handle
 */
typedef struct opus_decoder *opus_decoder_handle_t;

/**
 * Encoder configuration
 */
typedef struct {
    int32_t sample_rate;      /**< Sample rate in Hz (default: 16000) */
    uint8_t channels;         /**< Number of channels (default: 1) */
    int32_t bitrate_bps;      /**< Target bitrate in bits/sec (default: 16000) */
    uint8_t complexity;       /**< Encoder complexity 0-10 (default: 5) */
    uint8_t frame_duration_ms;/**< Frame duration in ms (default: 20) */
} opus_encoder_config_t;

/**
 * Decoder configuration
 */
typedef struct {
    int32_t sample_rate;      /**< Sample rate in Hz (default: 16000) */
    uint8_t channels;         /**< Number of channels (default: 1) */
} opus_decoder_config_t;

/**
 * Default encoder configuration
 */
#define OPUS_ENCODER_CONFIG_DEFAULT() { \
    .sample_rate = OPUS_SAMPLE_RATE, \
    .channels = OPUS_CHANNEL_NUM, \
    .bitrate_bps = 16000, \
    .complexity = 5, \
    .frame_duration_ms = OPUS_FRAME_DURATION_MS, \
}

/**
 * Default decoder configuration
 */
#define OPUS_DECODER_CONFIG_DEFAULT() { \
    .sample_rate = OPUS_SAMPLE_RATE, \
    .channels = OPUS_CHANNEL_NUM, \
}

/**
 * @brief Create an Opus encoder
 * 
 * @param[in] config Encoder configuration (NULL for defaults)
 * @return Encoder handle on success, NULL on failure
 */
opus_encoder_handle_t opus_enc_create(const opus_encoder_config_t *config);

/**
 * @brief Encode a PCM frame to Opus
 * 
 * @param[in] enc Encoder handle
 * @param[in] pcm PCM buffer (16-bit signed, native endian)
 * @param[in] pcm_size PCM buffer size in bytes (must be OPUS_PCM_FRAME_SIZE)
 * @param[out] opus Output buffer for encoded data
 * @param[in] opus_size Output buffer size
 * @param[out] encoded_size Actual encoded size in bytes
 * @return ESP_OK on success
 */
esp_err_t opus_encoder_encode(opus_encoder_handle_t enc, 
                               const int16_t *pcm, size_t pcm_size,
                               uint8_t *opus, size_t opus_size,
                               size_t *encoded_size);

/**
 * @brief Destroy an Opus encoder
 * 
 * @param enc Encoder handle
 */
void opus_enc_destroy(opus_encoder_handle_t enc);

/**
 * @brief Create an Opus decoder
 * 
 * @param[in] config Decoder configuration (NULL for defaults)
 * @return Decoder handle on success, NULL on failure
 */
opus_decoder_handle_t opus_dec_create(const opus_decoder_config_t *config);

/**
 * @brief Decode an Opus frame to PCM
 * 
 * @param[in] dec Decoder handle
 * @param[in] opus Encoded Opus data
 * @param[in] opus_size Encoded data size in bytes
 * @param[out] pcm Output PCM buffer (16-bit signed, native endian)
 * @param[in] pcm_size PCM buffer size in bytes
 * @param[out] decoded_size Actual decoded PCM size in bytes
 * @return ESP_OK on success
 */
esp_err_t opus_decoder_decode(opus_decoder_handle_t dec,
                               const uint8_t *opus, size_t opus_size,
                               int16_t *pcm, size_t pcm_size,
                               size_t *decoded_size);

/**
 * @brief Destroy an Opus decoder
 * 
 * @param dec Decoder handle
 */
void opus_dec_destroy(opus_decoder_handle_t dec);

#ifdef __cplusplus
}
#endif
