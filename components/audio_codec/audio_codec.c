/**
 * ESP001 Audio Codec Implementation
 * 
 * Raw PCM passthrough + lightweight noise reduction:
 * - 1st-order IIR high-pass filter (removes low-frequency rumble)
 * - RMS-based noise gate with smooth attack/release
 * 
 * Audio format: 16kHz, 16-bit, mono
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "audio_codec.h"
#include "opus_codec.h"

static const char *TAG = "AUDIO_CODEC";

/* ==================== Codec State ==================== */

/* Opus encoder/decoder handles (opaque) */
static opus_encoder_handle_t s_opus_enc = NULL;
static opus_decoder_handle_t s_opus_dec = NULL;

static audio_codec_t s_codec = {
    .type = AUDIO_CODEC_TYPE_NONE,
    .initialized = false,
    .encoder_state = NULL,
    .decoder_state = NULL,
};

/* ==================== Noise Reduction State ==================== */

typedef struct {
    bool enable;
    
    /* High-pass filter state (1st-order IIR) */
    int hp_cutoff_hz;
    float hp_alpha;           /* Filter coefficient */
    float hp_z1;              /* Filter state */
    
    /* Noise gate state */
    float noise_gate_db;
    float attack_coeff;       /* Attack time constant */
    float release_coeff;      /* Release time constant */
    float gate_gain;          /* Current gate gain (0.0 - 1.0) */
    
    /* RMS monitoring */
    float last_rms;
    
    /* Sample rate (fixed) */
    int sample_rate;
} audio_nr_state_t;

static audio_nr_state_t s_nr = {
    .enable = false,
    .hp_cutoff_hz = 100,
    .hp_alpha = 0.0f,
    .hp_z1 = 0.0f,
    .noise_gate_db = -40.0f,
    .attack_coeff = 0.0f,
    .release_coeff = 0.0f,
    .gate_gain = 1.0f,
    .last_rms = 0.0f,
    .sample_rate = AUDIO_CODEC_SAMPLE_RATE,
};

static bool s_nr_initialized = false;

/* ==================== Utility Functions ==================== */

/**
 * Calculate high-pass filter coefficient
 * 1st-order IIR: y[n] = hp_alpha * (y[n-1] + x[n] - x[n-1])
 * Transfer function: H(z) = (1 - z^-1) / (1 - (1-hp_alpha)*z^-1)
 * 
 * @param cutoff_hz Cutoff frequency in Hz
 * @param sample_rate Sample rate in Hz
 * @return Filter coefficient alpha
 */
static float hp_filter_coef(float cutoff_hz, int sample_rate)
{
    if (cutoff_hz <= 0.0f) {
        return 0.0f;  /* Disabled */
    }
    float wc = 2.0f * M_PI * cutoff_hz;
    float alpha = wc / (wc + (float)sample_rate);
    return alpha;
}

/**
 * Convert dB to linear scale
 */
static float db_to_linear(float db)
{
    return powf(10.0f, db / 20.0f);
}

/**
 * Calculate RMS of PCM samples
 */
static float calc_rms(const int16_t *samples, size_t count)
{
    if (count == 0) return 0.0f;
    
    float sum = 0.0f;
    for (size_t i = 0; i < count; i++) {
        float s = (float)samples[i] / 32768.0f;
        sum += s * s;
    }
    return sqrtf(sum / count) * 32768.0f;
}

/* ==================== High-Pass Filter ==================== */

/**
 * Apply 1st-order high-pass filter to PCM samples
 * y[n] = alpha * (y[n-1] + x[n] - x[n-1])
 */
static void hp_filter_process(const int16_t *pcm_in, int16_t *pcm_out, size_t count)
{
    if (!s_nr.enable || s_nr.hp_alpha <= 0.0f) {
        /* Passthrough */
        if (pcm_out != pcm_in) {
            memcpy(pcm_out, pcm_in, count * sizeof(int16_t));
        }
        return;
    }
    
    float z1 = s_nr.hp_z1;
    float alpha = s_nr.hp_alpha;
    
    for (size_t i = 0; i < count; i++) {
        float x = (float)pcm_in[i];
        float y = alpha * (z1 + x - s_nr.hp_z1);  /* Using stored z1 for x[n-1] */
        /* Actually correct formula: y[n] = alpha * (y[n-1] + x[n] - x[n-1]) */
        /* With x[n-1] = prev input */
        float prev_out = z1;
        float prev_in = s_nr.hp_z1;  /* This is x[n-1] stored in hp_z1 */
        y = alpha * (prev_out + x - prev_in);
        
        pcm_out[i] = (int16_t)roundf(y);
        z1 = y;
        s_nr.hp_z1 = x;  /* Store current input as x[n-1] for next iteration */
    }
    s_nr.hp_z1 = z1;
}

/* ==================== Noise Gate ==================== */

/**
 * Apply RMS-based noise gate with smooth attack/release
 */
static void noise_gate_process(int16_t *pcm, size_t count)
{
    if (!s_nr.enable || s_nr.noise_gate_db <= -80.0f) {
        return;  /* Disabled or very low threshold */
    }
    
    float rms = calc_rms(pcm, count);
    float threshold_linear = db_to_linear(s_nr.noise_gate_db);
    
    /* Target gain based on RMS vs threshold */
    float target_gain;
    if (rms < threshold_linear) {
        target_gain = rms / threshold_linear;  /* 0.0 to 1.0 */
        if (target_gain > 1.0f) target_gain = 1.0f;
    } else {
        target_gain = 1.0f;
    }
    
    /* Smooth gain transition */
    float coeff;
    if (target_gain > s_nr.gate_gain) {
        /* Attack: gain increasing */
        coeff = s_nr.attack_coeff;
    } else {
        /* Release: gain decreasing */
        coeff = s_nr.release_coeff;
    }
    
    s_nr.gate_gain = coeff * s_nr.gate_gain + (1.0f - coeff) * target_gain;
    
    /* Apply gain to all samples */
    for (size_t i = 0; i < count; i++) {
        float sample = (float)pcm[i] * s_nr.gate_gain;
        /* Soft clipping to prevent harsh distortion */
        if (sample > 32767.0f) sample = 32767.0f;
        if (sample < -32768.0f) sample = -32768.0f;
        pcm[i] = (int16_t)roundf(sample);
    }
    
    s_nr.last_rms = rms;
}

/* ==================== Codec API ==================== */

esp_err_t audio_codec_init(audio_codec_type_t type)
{
    if (s_codec.initialized) {
        ESP_LOGW(TAG, "Codec already initialized, deinitializing first");
        audio_codec_deinit();
    }

    s_codec.type = type;

    switch (type) {
        case AUDIO_CODEC_TYPE_NONE:
            ESP_LOGI(TAG, "Initializing codec: RAW PCM (passthrough mode)");
            break;
        case AUDIO_CODEC_TYPE_OPUS: {
            ESP_LOGI(TAG, "Initializing Opus codec...");
            
            /* Create Opus encoder */
            s_opus_enc = opus_enc_create(NULL);
            if (s_opus_enc == NULL) {
                ESP_LOGE(TAG, "Failed to create Opus encoder");
                return ESP_FAIL;
            }
            
            /* Create Opus decoder */
            s_opus_dec = opus_dec_create(NULL);
            if (s_opus_dec == NULL) {
                ESP_LOGE(TAG, "Failed to create Opus decoder");
                opus_enc_destroy(s_opus_enc);
                s_opus_enc = NULL;
                return ESP_FAIL;
            }
            
            ESP_LOGI(TAG, "Opus codec initialized: 16kHz, mono, 16kbps, VOICE mode");
            break;
        }
        default:
            ESP_LOGE(TAG, "Unknown codec type: %d", type);
            return ESP_ERR_INVALID_ARG;
    }

    s_codec.initialized = true;
    ESP_LOGI(TAG, "Codec initialized successfully");
    return ESP_OK;
}

esp_err_t audio_codec_deinit(void)
{
    if (!s_codec.initialized) {
        return ESP_OK;
    }

    /* Destroy Opus encoder/decoder if allocated */
    if (s_opus_enc != NULL) {
        opus_enc_destroy(s_opus_enc);
        s_opus_enc = NULL;
    }
    if (s_opus_dec != NULL) {
        opus_dec_destroy(s_opus_dec);
        s_opus_dec = NULL;
    }
    
    s_codec.encoder_state = NULL;
    s_codec.decoder_state = NULL;
    s_codec.initialized = false;

    ESP_LOGI(TAG, "Codec deinitialized");
    return ESP_OK;
}

esp_err_t audio_codec_encode(const int16_t *pcm_in, size_t pcm_in_size,
                              uint8_t *encoded_out, size_t *encoded_out_size,
                              size_t encoded_out_max)
{
    if (!s_codec.initialized) {
        ESP_LOGE(TAG, "Codec not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (pcm_in == NULL || encoded_out == NULL || encoded_out_size == NULL) {
        ESP_LOGE(TAG, "Null pointer argument");
        return ESP_ERR_INVALID_ARG;
    }

    switch (s_codec.type) {
        case AUDIO_CODEC_TYPE_NONE: {
            /* Raw PCM passthrough - apply noise reduction if enabled */
            size_t sample_count = pcm_in_size / sizeof(int16_t);
            
            if (s_nr_initialized && s_nr.enable) {
                /* Process noise reduction in-place into output buffer */
                esp_err_t nr_ret = audio_nr_process(pcm_in, (int16_t *)encoded_out, sample_count);
                if (nr_ret != ESP_OK) {
                    /* NR failed, fall back to passthrough */
                    if (pcm_in_size > encoded_out_max) {
                        ESP_LOGE(TAG, "Output buffer too small: %d > %d", pcm_in_size, encoded_out_max);
                        return ESP_ERR_INVALID_SIZE;
                    }
                    memcpy(encoded_out, pcm_in, pcm_in_size);
                } else {
                    /* NR succeeded */
                    if (pcm_in_size > encoded_out_max) {
                        ESP_LOGE(TAG, "Output buffer too small: %d > %d", pcm_in_size, encoded_out_max);
                        return ESP_ERR_INVALID_SIZE;
                    }
                    /* Output already in encoded_out from audio_nr_process */
                    *encoded_out_size = pcm_in_size;
                    return ESP_OK;
                }
            } else {
                /* No NR, direct copy */
                if (pcm_in_size > encoded_out_max) {
                    ESP_LOGE(TAG, "Output buffer too small: %d > %d", pcm_in_size, encoded_out_max);
                    return ESP_ERR_INVALID_SIZE;
                }
                memcpy(encoded_out, pcm_in, pcm_in_size);
            }
            *encoded_out_size = pcm_in_size;
            break;
        }

        case AUDIO_CODEC_TYPE_OPUS: {
            /* Opus encoding */
            if (s_opus_enc == NULL) {
                ESP_LOGE(TAG, "Opus encoder not initialized");
                return ESP_ERR_INVALID_STATE;
            }
            
            if (encoded_out_max < OPUS_MAX_ENCODED_SIZE) {
                ESP_LOGE(TAG, "Output buffer too small: %d < %d", encoded_out_max, OPUS_MAX_ENCODED_SIZE);
                return ESP_ERR_INVALID_SIZE;
            }
            
            esp_err_t ret = opus_encoder_encode(s_opus_enc, pcm_in, pcm_in_size,
                                                encoded_out, encoded_out_max,
                                                encoded_out_size);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Opus encoding failed");
                return ret;
            }
            
            ESP_LOGD(TAG, "Encoded %d bytes PCM -> %d bytes Opus", pcm_in_size, *encoded_out_size);
            break;
        }
    }

    return ESP_OK;
}

esp_err_t audio_codec_decode(const uint8_t *encoded_in, size_t encoded_in_size,
                              int16_t *pcm_out, size_t *pcm_out_size,
                              size_t pcm_out_max)
{
    if (!s_codec.initialized) {
        ESP_LOGE(TAG, "Codec not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (encoded_in == NULL || pcm_out == NULL || pcm_out_size == NULL) {
        ESP_LOGE(TAG, "Null pointer argument");
        return ESP_ERR_INVALID_ARG;
    }

    switch (s_codec.type) {
        case AUDIO_CODEC_TYPE_NONE:
            /* Raw PCM passthrough */
            if (encoded_in_size > pcm_out_max) {
                ESP_LOGE(TAG, "Output buffer too small: %d > %d", encoded_in_size, pcm_out_max);
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(pcm_out, encoded_in, encoded_in_size);
            *pcm_out_size = encoded_in_size;
            break;

        case AUDIO_CODEC_TYPE_OPUS: {
            /* Opus decoding */
            if (s_opus_dec == NULL) {
                ESP_LOGE(TAG, "Opus decoder not initialized");
                return ESP_ERR_INVALID_STATE;
            }
            
            if (pcm_out_max < OPUS_PCM_FRAME_SIZE) {
                ESP_LOGE(TAG, "Output buffer too small: %d < %d", pcm_out_max, OPUS_PCM_FRAME_SIZE);
                return ESP_ERR_INVALID_SIZE;
            }
            
            esp_err_t ret = opus_decoder_decode(s_opus_dec, encoded_in, encoded_in_size,
                                                pcm_out, pcm_out_max, pcm_out_size);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Opus decoding failed");
                return ret;
            }
            
            ESP_LOGD(TAG, "Decoded %d bytes Opus -> %d bytes PCM", encoded_in_size, *pcm_out_size);
            break;
        }
    }

    return ESP_OK;
}

const char* audio_codec_get_type_name(void)
{
    switch (s_codec.type) {
        case AUDIO_CODEC_TYPE_NONE:
            return "RAW_PCM";
        case AUDIO_CODEC_TYPE_OPUS:
            return "OPUS";
        default:
            return "UNKNOWN";
    }
}

/* ==================== Noise Reduction API ==================== */

esp_err_t audio_nr_init(const audio_nr_config_t *config)
{
    if (s_nr_initialized) {
        ESP_LOGW(TAG, "NR already initialized, reinitializing");
        audio_nr_deinit();
    }
    
    audio_nr_config_t cfg;
    if (config != NULL) {
        cfg = *config;
    } else {
        cfg = (audio_nr_config_t)AUDIO_NR_CONFIG_DEFAULT();
    }
    
    s_nr.enable = cfg.enable;
    s_nr.hp_cutoff_hz = cfg.hp_cutoff_hz;
    s_nr.noise_gate_db = cfg.noise_gate_db;
    s_nr.sample_rate = AUDIO_CODEC_SAMPLE_RATE;
    
    /* Calculate high-pass filter coefficient */
    s_nr.hp_alpha = hp_filter_coef((float)cfg.hp_cutoff_hz, AUDIO_CODEC_SAMPLE_RATE);
    s_nr.hp_z1 = 0.0f;
    
    /* Calculate noise gate smoothing coefficients */
    /* attack_coeff = exp(-1.0 / (attack_ms * sample_rate / 1000)) */
    /* release_coeff = exp(-1.0 / (release_ms * sample_rate / 1000)) */
    if (cfg.attack_ms > 0.0f) {
        float attack_samples = (float)AUDIO_CODEC_SAMPLE_RATE * cfg.attack_ms / 1000.0f;
        s_nr.attack_coeff = expf(-1.0f / attack_samples);
    } else {
        s_nr.attack_coeff = 0.0f;
    }
    
    if (cfg.release_ms > 0.0f) {
        float release_samples = (float)AUDIO_CODEC_SAMPLE_RATE * cfg.release_ms / 1000.0f;
        s_nr.release_coeff = expf(-1.0f / release_samples);
    } else {
        s_nr.release_coeff = 0.9f;  /* Slow release by default */
    }
    
    s_nr.gate_gain = 1.0f;
    s_nr.last_rms = 0.0f;
    
    s_nr_initialized = true;
    
    ESP_LOGI(TAG, "Noise reduction initialized:");
    ESP_LOGI(TAG, "  High-pass filter: %d Hz (alpha=%.4f)", 
             s_nr.hp_cutoff_hz, s_nr.hp_alpha);
    ESP_LOGI(TAG, "  Noise gate: %.1f dB threshold", s_nr.noise_gate_db);
    ESP_LOGI(TAG, "  Attack/release: %.1f/%.1f ms", cfg.attack_ms, cfg.release_ms);
    
    return ESP_OK;
}

esp_err_t audio_nr_deinit(void)
{
    if (!s_nr_initialized) {
        return ESP_OK;
    }
    
    s_nr.enable = false;
    s_nr_initialized = false;
    
    ESP_LOGI(TAG, "Noise reduction deinitialized");
    return ESP_OK;
}

esp_err_t audio_nr_process(const int16_t *pcm_in, int16_t *pcm_out, size_t sample_count)
{
    if (pcm_in == NULL || pcm_out == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Step 1: Apply high-pass filter */
    hp_filter_process(pcm_in, pcm_out, sample_count);
    
    /* Step 2: Apply noise gate */
    noise_gate_process(pcm_out, sample_count);
    
    return ESP_OK;
}

esp_err_t audio_nr_update_config(const audio_nr_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Update enabled state */
    if (!s_nr.enable && config->enable) {
        /* Re-enabling - reinit HP filter state */
        s_nr.hp_z1 = 0.0f;
        s_nr.gate_gain = 1.0f;
    }
    
    s_nr.enable = config->enable;
    s_nr.hp_cutoff_hz = config->hp_cutoff_hz;
    s_nr.hp_alpha = hp_filter_coef((float)config->hp_cutoff_hz, AUDIO_CODEC_SAMPLE_RATE);
    s_nr.noise_gate_db = config->noise_gate_db;
    
    /* Recalculate smoothing coefficients */
    if (config->attack_ms > 0.0f) {
        float attack_samples = (float)AUDIO_CODEC_SAMPLE_RATE * config->attack_ms / 1000.0f;
        s_nr.attack_coeff = expf(-1.0f / attack_samples);
    } else {
        s_nr.attack_coeff = 0.0f;
    }
    
    if (config->release_ms > 0.0f) {
        float release_samples = (float)AUDIO_CODEC_SAMPLE_RATE * config->release_ms / 1000.0f;
        s_nr.release_coeff = expf(-1.0f / release_samples);
    } else {
        s_nr.release_coeff = 0.9f;
    }
    
    ESP_LOGI(TAG, "NR config updated: HP=%dHz, NG=%.1fdB, Attack=%.1fms, Release=%.1fms",
             s_nr.hp_cutoff_hz, s_nr.noise_gate_db, config->attack_ms, config->release_ms);
    
    return ESP_OK;
}

bool audio_nr_is_enabled(void)
{
    return s_nr_initialized && s_nr.enable;
}

float audio_nr_get_rms_level(void)
{
    return s_nr.last_rms;
}
