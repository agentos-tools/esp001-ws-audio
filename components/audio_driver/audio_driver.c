/**
 * ESP001 Audio Driver Implementation
 * 
 * I2S audio driver for ES7210 (ADC) and ES8311 (DAC)
 */

#include <string.h>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "audio_driver.h"
#include "es7210.h"
#include "es8311.h"

static const char *TAG = "AUDIO";

static audio_driver_t s_audio_ctx = {0};

/* I2C master bus handle */
i2c_master_bus_handle_t i2c_bus_handle = NULL;

/**
 * Initialize I2C master for ES7210/ES8311 communication
 */
static esp_err_t i2c_master_init(void)
{
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .flags.enable_internal_pullup = true,
    };
    
    esp_err_t ret = i2c_new_master_bus(&i2c_bus_config, &i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C master bus: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "I2C master initialized (SDA=%d, SCL=%d)", 
             I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
    return ESP_OK;
}

/**
 * Initialize I2S for audio capture and playback
 */
static esp_err_t i2s_init(void)
{
    esp_err_t ret = ESP_OK;
    
    /* I2S standard mode configuration */
    i2s_std_config_t i2s_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            AUDIO_BITS_PER_SAMPLE, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_IO,
            .bclk = I2S_BCLK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    /* Create I2S TX channel (playback) */
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = I2S_DMA_BUF_COUNT;
    tx_chan_cfg.dma_frame_num = I2S_DMA_BUF_LEN;
    
    ret = i2s_new_channel(&tx_chan_cfg, &s_audio_ctx.tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S TX channel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = i2s_channel_init_std_mode(s_audio_ctx.tx_handle, &i2s_std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S TX channel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Create I2S RX channel (capture) */
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    rx_chan_cfg.dma_desc_num = I2S_DMA_BUF_COUNT;
    rx_chan_cfg.dma_frame_num = I2S_DMA_BUF_LEN;
    
    ret = i2s_new_channel(&rx_chan_cfg, NULL, &s_audio_ctx.rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S RX channel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = i2s_channel_init_std_mode(s_audio_ctx.rx_handle, &i2s_std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S RX channel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "I2S initialized (MCLK=%d, BCLK=%d, WS=%d, DO=%d, DI=%d)",
             I2S_MCLK_IO, I2S_BCLK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO);
    
    return ESP_OK;
}

/**
 * Initialize audio driver
 */
esp_err_t audio_driver_init(void)
{
    esp_err_t ret = ESP_OK;
    
    if (s_audio_ctx.initialized) {
        ESP_LOGW(TAG, "Audio driver already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing audio driver...");
    
    /* Initialize I2C */
    ret = i2c_master_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C");
        return ret;
    }
    
    /* Initialize ES7210 ADC */
    ret = es7210_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ES7210");
        return ret;
    }
    
    /* Initialize ES8311 DAC */
    ret = es8311_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ES8311");
        return ret;
    }
    
    /* Set default volume to 80% */
    audio_set_volume(80);
    
    /* Initialize I2S */
    ret = i2s_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S");
        return ret;
    }
    
    s_audio_ctx.initialized = true;
    ESP_LOGI(TAG, "Audio driver initialized successfully");
    
    return ESP_OK;
}

/**
 * Deinitialize audio driver
 */
esp_err_t audio_driver_deinit(void)
{
    if (!s_audio_ctx.initialized) {
        return ESP_OK;
    }
    
    /* Stop audio */
    audio_stop();
    
    /* Delete I2S channels */
    if (s_audio_ctx.tx_handle) {
        i2s_del_channel(s_audio_ctx.tx_handle);
        s_audio_ctx.tx_handle = NULL;
    }
    
    if (s_audio_ctx.rx_handle) {
        i2s_del_channel(s_audio_ctx.rx_handle);
        s_audio_ctx.rx_handle = NULL;
    }
    
    /* Deinitialize audio chips */
    es7210_deinit();
    es8311_deinit();
    
    /* Delete I2C master bus */
    if (i2c_bus_handle) {
        i2c_del_master_bus(i2c_bus_handle);
        i2c_bus_handle = NULL;
    }
    
    s_audio_ctx.initialized = false;
    ESP_LOGI(TAG, "Audio driver deinitialized");
    
    return ESP_OK;
}

/**
 * Start audio capture and playback
 */
esp_err_t audio_start(void)
{
    if (!s_audio_ctx.initialized) {
        ESP_LOGE(TAG, "Audio driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret;
    
    /* Enable I2S channels */
    if (s_audio_ctx.tx_handle) {
        ret = i2s_channel_enable(s_audio_ctx.tx_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable TX channel");
            return ret;
        }
    }
    
    if (s_audio_ctx.rx_handle) {
        ret = i2s_channel_enable(s_audio_ctx.rx_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable RX channel");
            return ret;
        }
    }
    
    /* Enable DAC output */
    es8311_set_output_enable(true);
    
    ESP_LOGI(TAG, "Audio started");
    return ESP_OK;
}

/**
 * Stop audio capture and playback
 */
esp_err_t audio_stop(void)
{
    if (!s_audio_ctx.initialized) {
        return ESP_OK;
    }
    
    /* Disable DAC output */
    es8311_set_output_enable(false);
    
    /* Disable I2S channels */
    if (s_audio_ctx.tx_handle) {
        i2s_channel_disable(s_audio_ctx.tx_handle);
    }
    
    if (s_audio_ctx.rx_handle) {
        i2s_channel_disable(s_audio_ctx.rx_handle);
    }
    
    ESP_LOGI(TAG, "Audio stopped");
    return ESP_OK;
}

/**
 * Read audio data from microphone (ES7210)
 */
esp_err_t audio_read(void *data, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    if (!s_audio_ctx.initialized || !s_audio_ctx.rx_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return i2s_channel_read(s_audio_ctx.rx_handle, data, len, bytes_read, timeout_ms);
}

/**
 * Write audio data to speaker (ES8311)
 */
esp_err_t audio_write(const void *data, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    if (!s_audio_ctx.initialized || !s_audio_ctx.tx_handle) {
        ESP_LOGW(TAG, "audio_write: not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = i2s_channel_write(s_audio_ctx.tx_handle, data, len, bytes_written, timeout_ms);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "audio_write: i2s_channel_write failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * Set output volume
 */
esp_err_t audio_set_volume(uint8_t volume)
{
    return es8311_set_volume(volume);
}

/**
 * Get output volume
 */
esp_err_t audio_get_volume(uint8_t *volume)
{
    return es8311_get_volume(volume);
}