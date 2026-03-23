/**
 * @file opus_codec.c
 * @brief Opus audio codec implementation for ESP32-S3
 * 
 * This component wraps libopus to provide encoder/decoder functionality
 * optimized for voice communication at 16kHz mono.
 * 
 * Libopus source is downloaded and built automatically via CMake FetchContent.
 */

#include "opus_codec.h"
#include "esp_log.h"
#include "esp_check.h"
#include <opus.h>
#include <opus_types.h>
#include <string.h>

static const char *TAG = "OPUS_CODEC";

/* Maximum packet size (60ms frame at 60kbps = 450 bytes, use 512 for safety) */
#define OPUS_MAX_PACKET_SIZE    512

struct opus_encoder {
    OpusEncoder *enc;
    int32_t sample_rate;
    uint8_t channels;
};

struct opus_decoder {
    OpusDecoder *dec;
    int32_t sample_rate;
    uint8_t channels;
};

opus_encoder_handle_t opus_enc_create(const opus_encoder_config_t *config)
{
    opus_encoder_config_t cfg;
    if (config != NULL) {
        cfg = *config;
    } else {
        cfg = (opus_encoder_config_t)OPUS_ENCODER_CONFIG_DEFAULT();
    }

    /* Validate parameters */
    if (cfg.channels == 0 || cfg.sample_rate == 0) {
        ESP_LOGE(TAG, "Invalid encoder config: channels=%d, sample_rate=%d",
                 cfg.channels, cfg.sample_rate);
        return NULL;
    }

    /* Check for valid sample rates */
    int opus_sr;
    switch (cfg.sample_rate) {
        case 8000:   opus_sr = 8000; break;
        case 12000:  opus_sr = 12000; break;
        case 16000:  opus_sr = 16000; break;
        case 24000:  opus_sr = 24000; break;
        case 48000:  opus_sr = 48000; break;
        default:
            ESP_LOGE(TAG, "Unsupported sample rate: %d", cfg.sample_rate);
            return NULL;
    }

    int error;
    OpusEncoder *enc = opus_encoder_create(opus_sr, cfg.channels, 
                                            OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK) {
        ESP_LOGE(TAG, "Failed to create encoder: error=%d", error);
        return NULL;
    }

    /* Configure encoder */
    if (cfg.bitrate_bps > 0) {
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(cfg.bitrate_bps));
    }
    
    /* Clamp complexity to valid range 0-10 */
    uint8_t complexity = cfg.complexity > 10 ? 10 : cfg.complexity;
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity));

    /* Force Opus in VOICE mode for low latency */
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    ESP_LOGI(TAG, "Encoder created: %dHz, %dch, %dbps, complexity=%d",
             cfg.sample_rate, cfg.channels, cfg.bitrate_bps, complexity);

    opus_encoder_handle_t handle = calloc(1, sizeof(struct opus_encoder));
    if (handle == NULL) {
        opus_encoder_destroy(enc);
        return NULL;
    }
    handle->enc = enc;
    handle->sample_rate = cfg.sample_rate;
    handle->channels = cfg.channels;

    return handle;
}

esp_err_t opus_encoder_encode(opus_encoder_handle_t enc_handle,
                               const int16_t *pcm, size_t pcm_size,
                               uint8_t *opus, size_t opus_size,
                               size_t *encoded_size)
{
    ESP_RETURN_ON_FALSE(enc_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL encoder handle");
    ESP_RETURN_ON_FALSE(pcm != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL PCM buffer");
    ESP_RETURN_ON_FALSE(opus != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL opus buffer");
    ESP_RETURN_ON_FALSE(encoded_size != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL output size");

    /* Validate input size */
    size_t expected_pcm = OPUS_SAMPLES_PER_FRAME * sizeof(int16_t) * enc_handle->channels;
    if (pcm_size != expected_pcm) {
        ESP_LOGW(TAG, "Unexpected PCM size: %d, expected %d", pcm_size, expected_pcm);
    }

    if (opus_size < OPUS_MAX_PACKET_SIZE) {
        ESP_LOGE(TAG, "Output buffer too small: %d < %d", opus_size, OPUS_MAX_PACKET_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    int nsamples = pcm_size / sizeof(int16_t) / enc_handle->channels;
    int out_size = opus_encode(enc_handle->enc, pcm, nsamples, 
                                opus, opus_size);

    if (out_size < 0) {
        ESP_LOGE(TAG, "Encoding failed: %d", out_size);
        return ESP_FAIL;
    }

    *encoded_size = out_size;
    return ESP_OK;
}

void opus_enc_destroy(opus_encoder_handle_t enc_handle)
{
    if (enc_handle == NULL) {
        return;
    }
    if (enc_handle->enc != NULL) {
        opus_encoder_destroy(enc_handle->enc);
    }
    free(enc_handle);
}

opus_decoder_handle_t opus_dec_create(const opus_decoder_config_t *config)
{
    opus_decoder_config_t cfg;
    if (config != NULL) {
        cfg = *config;
    } else {
        cfg = (opus_decoder_config_t)OPUS_DECODER_CONFIG_DEFAULT();
    }

    /* Validate parameters */
    if ( cfg.channels == 0 || cfg.sample_rate == 0) {
        ESP_LOGE(TAG, "Invalid decoder config: channels=%d, sample_rate=%d",
                 cfg.channels, cfg.sample_rate);
        return NULL;
    }

    /* Check for valid sample rates */
    int opus_sr;
    switch (cfg.sample_rate) {
        case 8000:   opus_sr = 8000; break;
        case 12000:  opus_sr = 12000; break;
        case 16000:  opus_sr = 16000; break;
        case 24000:  opus_sr = 24000; break;
        case 48000:  opus_sr = 48000; break;
        default:
            ESP_LOGE(TAG, "Unsupported sample rate: %d", cfg.sample_rate);
            return NULL;
    }

    int error;
    OpusDecoder *dec = opus_decoder_create(opus_sr, cfg.channels, &error);
    if (error != OPUS_OK) {
        ESP_LOGE(TAG, "Failed to create decoder: error=%d", error);
        return NULL;
    }

    ESP_LOGI(TAG, "Decoder created: %dHz, %dch", cfg.sample_rate, cfg.channels);

    opus_decoder_handle_t handle = calloc(1, sizeof(struct opus_decoder));
    if (handle == NULL) {
        opus_decoder_destroy(dec);
        return NULL;
    }
    handle->dec = dec;
    handle->sample_rate = cfg.sample_rate;
    handle->channels = cfg.channels;

    return handle;
}

esp_err_t opus_decoder_decode(opus_decoder_handle_t dec_handle,
                               const uint8_t *opus, size_t opus_size,
                               int16_t *pcm, size_t pcm_size,
                               size_t *decoded_size)
{
    ESP_RETURN_ON_FALSE(dec_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL decoder handle");
    ESP_RETURN_ON_FALSE(opus != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL opus buffer");
    ESP_RETURN_ON_FALSE(pcm != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL PCM buffer");
    ESP_RETURN_ON_FALSE(decoded_size != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL output size");

    if (opus_size == 0) {
        /* PLC - Packet Loss Concealment: repeat last frame */
        int nsamples = pcm_size / sizeof(int16_t) / dec_handle->channels;
        int out_size = opus_decode(dec_handle->dec, NULL, 0, pcm, nsamples, 0);
        if (out_size < 0) {
            ESP_LOGE(TAG, "PLC decode failed: %d", out_size);
            return ESP_FAIL;
        }
        *decoded_size = out_size * sizeof(int16_t) * dec_handle->channels;
        return ESP_OK;
    }

    int nsamples = pcm_size / sizeof(int16_t) / dec_handle->channels;
    int out_size = opus_decode(dec_handle->dec, (const unsigned char *)opus, opus_size, 
                                pcm, nsamples, 0);

    if (out_size < 0) {
        ESP_LOGE(TAG, "Decoding failed: %d", out_size);
        return ESP_FAIL;
    }

    *decoded_size = out_size * sizeof(int16_t) * dec_handle->channels;
    return ESP_OK;
}

void opus_dec_destroy(opus_decoder_handle_t dec_handle)
{
    if (dec_handle == NULL) {
        return;
    }
    if (dec_handle->dec != NULL) {
        opus_decoder_destroy(dec_handle->dec);
    }
    free(dec_handle);
}
