/**
 * ES8311 - Low Power Audio DAC Driver Implementation
 */

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "es8311.h"
#include "audio_driver.h"

static const char *TAG = "ES8311";

/* I2C master bus handle - declared in audio_driver.c */
extern i2c_master_bus_handle_t i2c_bus_handle;

/* Volume mapping: 0-100 -> register value */
/* ES8311 volume: -95.5dB to +32dB in 0.5dB steps */
/* Register 0x06: bits[7:0] = volume, 0x00 = -95.5dB, 0xFF = +32dB */

static uint8_t current_volume = 100;  /* Default volume 100% for testing */

/**
 * Read ES8311 register
 */
esp_err_t es8311_read_reg(uint8_t reg, uint8_t *value)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    
    i2c_master_dev_handle_t dev_handle;
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) return ret;
    
    ret = i2c_master_transmit_receive(dev_handle, &reg, 1, value, 1, pdMS_TO_TICKS(100));
    
    i2c_master_bus_rm_device(dev_handle);
    return ret;
}

/**
 * Write ES8311 register
 */
esp_err_t es8311_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t write_buf[2] = {reg, value};
    
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    
    i2c_master_dev_handle_t dev_handle;
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) return ret;
    
    ret = i2c_master_transmit(dev_handle, write_buf, 2, pdMS_TO_TICKS(100));
    
    i2c_master_bus_rm_device(dev_handle);
    return ret;
}

/**
 * Initialize ES8311 DAC
 */
esp_err_t es8311_init(void)
{
    esp_err_t ret = ESP_OK;
    uint8_t chip_id = 0;
    
    ESP_LOGI(TAG, "Initializing ES8311 DAC...");
    
    /* Read chip ID */
    ret = es8311_read_reg(ES8311_CHIP_ID_REG, &chip_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read chip ID: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Chip ID: 0x%02X", chip_id);
    if ((chip_id >> 4) != ES8311_CHIP_ID) {
        ESP_LOGW(TAG, "Unexpected chip ID, expected 0x83");
    }
    
    /* Reset the chip */
    ret = es8311_write_reg(ES8311_RESET_REG, 0x01);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset chip");
        return ret;
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));  /* Wait for reset */
    
    /* Release reset */
    ret = es8311_write_reg(ES8311_RESET_REG, 0x00);
    if (ret != ESP_OK) return ret;
    
    /* Configure clock management - slave mode, MCLK from ESP32 */
    ret = es8311_write_reg(ES8311_CLOCK_MGMT_REG, 0x00);
    if (ret != ESP_OK) return ret;
    
    /* Configure DAC */
    ret = es8311_write_reg(ES8311_DAC_CFG_REG, 0x10);  /* Enable DAC */
    if (ret != ESP_OK) return ret;
    
    /* Configure DAC output */
    ret = es8311_write_reg(ES8311_DAC_OUT_CFG_REG, 0x01);  /* Enable output */
    if (ret != ESP_OK) return ret;
    
    /* Set sample rate */
    ret = es8311_set_sample_rate(AUDIO_SAMPLE_RATE);
    if (ret != ESP_OK) return ret;
    
    /* Set default volume */
    ret = es8311_set_volume(current_volume);
    if (ret != ESP_OK) return ret;
    
    ESP_LOGI(TAG, "ES8311 initialized successfully");
    return ESP_OK;
}

/**
 * Deinitialize ES8311 DAC
 */
esp_err_t es8311_deinit(void)
{
    /* Disable output */
    es8311_set_output_enable(false);
    
    /* Reset the chip */
    return es8311_write_reg(ES8311_RESET_REG, 0x01);
}

/**
 * Configure ES8311 for specific sample rate
 */
esp_err_t es8311_set_sample_rate(uint32_t sample_rate)
{
    esp_err_t ret = ESP_OK;
    
    /* Configure DAC sample rate based on MCLK/fs ratio
     * For 16kHz with MCLK = 256fs = 4.096MHz
     */
    switch (sample_rate) {
        case 16000:
            /* Configure for 16kHz - set PLL parameters */
            ret = es8311_write_reg(ES8311_PLL_CFG1_REG, 0x04);
            if (ret != ESP_OK) return ret;
            ret = es8311_write_reg(ES8311_PLL_CFG2_REG, 0x00);
            break;
        case 44100:
            ret = es8311_write_reg(ES8311_PLL_CFG1_REG, 0x02);
            if (ret != ESP_OK) return ret;
            ret = es8311_write_reg(ES8311_PLL_CFG2_REG, 0x00);
            break;
        case 48000:
            ret = es8311_write_reg(ES8311_PLL_CFG1_REG, 0x02);
            if (ret != ESP_OK) return ret;
            ret = es8311_write_reg(ES8311_PLL_CFG2_REG, 0x00);
            break;
        default:
            ESP_LOGW(TAG, "Unsupported sample rate: %lu", sample_rate);
            return ESP_ERR_NOT_SUPPORTED;
    }
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Sample rate set to %lu Hz", sample_rate);
    }
    
    return ret;
}

/**
 * Set output volume (0-100)
 */
esp_err_t es8311_set_volume(uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }
    
    /* Map 0-100 to ES8311 volume register
     * 0 = -95.5dB (mute)
     * 50 = approximately 0dB
     * 100 = +32dB
     */
    uint8_t vol_reg;
    if (volume == 0) {
        vol_reg = 0x00;  /* Mute */
    } else if (volume == 100) {
        vol_reg = 0xFF;  /* Max volume */
    } else {
        /* Linear mapping: 1-99 -> 0x80-0xFF (roughly -48dB to +32dB) */
        vol_reg = 0x80 + (volume * 0x7F / 100);
    }
    
    esp_err_t ret = es8311_write_reg(ES8311_VOLUME_REG, vol_reg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set volume");
        return ret;
    }
    
    current_volume = volume;
    ESP_LOGI(TAG, "Volume set to %d%% (reg=0x%02X)", volume, vol_reg);
    return ESP_OK;
}

/**
 * Get output volume
 */
esp_err_t es8311_get_volume(uint8_t *volume)
{
    if (volume == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *volume = current_volume;
    return ESP_OK;
}

/**
 * Enable/disable DAC output
 */
esp_err_t es8311_set_output_enable(bool enable)
{
    uint8_t value = enable ? 0x01 : 0x00;
    return es8311_write_reg(ES8311_DAC_OUT_CFG_REG, value);
}