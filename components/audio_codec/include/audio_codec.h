/**
 * ESP001 Audio Codec
 * 
 * Audio encoding/decoding interface for raw PCM passthrough.
 * Includes lightweight noise reduction (high-pass filter + noise gate).
 * 
 * Audio format: 16kHz, 16-bit, mono
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* Audio codec configuration */
#define AUDIO_CODEC_SAMPLE_RATE     16000
#define AUDIO_CODEC_BITS_PER_SAMPLE 16
#define AUDIO_CODEC_CHANNELS        1
#define AUDIO_CODEC_FRAME_SIZE_MS   20
#define AUDIO_CODEC_FRAME_SIZE      (AUDIO_CODEC_SAMPLE_RATE * AUDIO_CODEC_BITS_PER_SAMPLE / 8 * AUDIO_CODEC_CHANNELS * AUDIO_CODEC_FRAME_SIZE_MS / 1000)  /* 320 bytes */

/* Codec types */
typedef enum {
    AUDIO_CODEC_TYPE_NONE = 0,  /* Raw PCM passthrough */
    AUDIO_CODEC_TYPE_OPUS,      /* Opus (future) */
} audio_codec_type_t;

/* Noise reduction configuration */
typedef struct {
    bool enable;              /* Enable/disable noise reduction */
    int  hp_cutoff_hz;        /* High-pass filter cutoff frequency (Hz). 0=disabled. Typical: 80-300 */
    float noise_gate_db;      /* Noise gate threshold in dB (e.g., -40). Below this RMS, audio is attenuated */
    float attack_ms;          /* Noise gate attack time in ms */
    float release_ms;         /* Noise gate release time in ms */
} audio_nr_config_t;

/* Codec context */
typedef struct {
    audio_codec_type_t type;
    bool initialized;
    void *encoder_state;
    void *decoder_state;
} audio_codec_t;

/**
 * Default noise reduction configuration
 * High-pass at 100Hz + noise gate at -40dB
 */
#define AUDIO_NR_CONFIG_DEFAULT() \
    { \
        .enable = true, \
        .hp_cutoff_hz = 100, \
        .noise_gate_db = -40.0f, \
        .attack_ms = 5.0f, \
        .release_ms = 50.0f, \
    }

/**
 * Initialize audio codec
 * 
 * @param type Codec type (AUDIO_CODEC_TYPE_NONE for raw PCM)
 * @return ESP_OK on success
 */
esp_err_t audio_codec_init(audio_codec_type_t type);

/**
 * Deinitialize audio codec
 * 
 * @return ESP_OK on success
 */
esp_err_t audio_codec_deinit(void);

/**
 * Encode audio data (PCM -> compressed)
 * For AUDIO_CODEC_TYPE_NONE, this is a passthrough.
 * 
 * @param pcm_in Input PCM data (16kHz, 16-bit, mono)
 * @param pcm_in_size Input size in bytes (must be AUDIO_CODEC_FRAME_SIZE for opus)
 * @param encoded_out Output buffer for encoded data
 * @param encoded_out_size Output size (filled by function)
 * @param encoded_out_max Maximum output buffer size
 * @return ESP_OK on success
 */
esp_err_t audio_codec_encode(const int16_t *pcm_in, size_t pcm_in_size,
                              uint8_t *encoded_out, size_t *encoded_out_size,
                              size_t encoded_out_max);

/**
 * Decode audio data (compressed -> PCM)
 * For AUDIO_CODEC_TYPE_NONE, this is a passthrough.
 * 
 * @param encoded_in Input encoded data
 * @param encoded_in_size Input size in bytes
 * @param pcm_out Output PCM buffer
 * @param pcm_out_size Output size (filled by function)
 * @param pcm_out_max Maximum output buffer size
 * @return ESP_OK on success
 */
esp_err_t audio_codec_decode(const uint8_t *encoded_in, size_t encoded_in_size,
                              int16_t *pcm_out, size_t *pcm_out_size,
                              size_t pcm_out_max);

/**
 * Get current codec type name
 * 
 * @return Codec type name string
 */
const char* audio_codec_get_type_name(void);

/* ==================== Noise Reduction API ==================== */

/**
 * Initialize noise reduction module
 * 
 * @param config Noise reduction configuration (NULL = use defaults)
 * @return ESP_OK on success
 */
esp_err_t audio_nr_init(const audio_nr_config_t *config);

/**
 * Deinitialize noise reduction module
 * 
 * @return ESP_OK on success
 */
esp_err_t audio_nr_deinit(void);

/**
 * Apply noise reduction to PCM samples
 * This is called automatically by audio_codec_encode() when NR is enabled,
 * but can also be called directly.
 * 
 * @param pcm_in Input PCM samples (16kHz, 16-bit, mono)
 * @param pcm_out Output PCM buffer (can be same as input for in-place)
 * @param sample_count Number of samples
 * @return ESP_OK on success
 */
esp_err_t audio_nr_process(const int16_t *pcm_in, int16_t *pcm_out, size_t sample_count);

/**
 * Update noise reduction configuration at runtime
 * 
 * @param config New configuration (NULL = no change)
 * @return ESP_OK on success
 */
esp_err_t audio_nr_update_config(const audio_nr_config_t *config);

/**
 * Check if noise reduction is enabled
 * 
 * @return true if enabled
 */
bool audio_nr_is_enabled(void);

/**
 * Get current RMS level of last processed frame (for debugging/monitoring)
 * 
 * @return RMS value in linear scale (0-32768)
 */
float audio_nr_get_rms_level(void);
