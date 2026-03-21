/**
 * ES7210 - 4-channel Audio ADC Driver Implementation
 */

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "es7210.h"
#include "audio_driver.h"

static const char *TAG = "ES7210";

/* I2C master bus handle - declared in audio_driver.c */
extern i2c_master_bus_handle_t i2c_bus_handle;

/**
 * Read ES7210 register
 */
esp_err_t es7210_read_reg(uint8_t reg, uint8_t *value)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES7210_I2C_ADDR,
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
 * Write ES7210 register
 */
esp_err_t es7210_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t write_buf[2] = {reg, value};
    
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES7210_I2C_ADDR,
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
 * Initialize ES7210 ADC
 */
esp_err_t es7210_init(void)
{
    esp_err_t ret = ESP_OK;
    uint8_t chip_id = 0;
    
    ESP_LOGI(TAG, "Initializing ES7210 ADC...");
    
    /* Read chip ID */
    ret = es7210_read_reg(ES7210_CHIP_ID_REG, &chip_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read chip ID: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Chip ID: 0x%02X", chip_id);
    if ((chip_id >> 4) != ES7210_CHIP_ID) {
        ESP_LOGW(TAG, "Unexpected chip ID, expected 0x72");
    }
    
    /* Reset the chip */
    ret = es7210_write_reg(ES7210_RESET_REG, 0x01);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset chip");
        return ret;
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));  /* Wait for reset */
    
    /* Configure clock - Slave mode, MCLK from ESP32 */
    ret = es7210_write_reg(ES7210_CLOCK_CFG_REG, 0x00);  /* Slave mode */
    if (ret != ESP_OK) return ret;
    
    /* Configure ADC1 (channel 1) - enable, gain setting */
    ret = es7210_write_reg(ES7210_ADC1_CFG_REG, 0x20);  /* Enable ADC1 */
    if (ret != ESP_OK) return ret;
    
    /* Power up ADC */
    ret = es7210_write_reg(ES7210_ANALOG_CFG_REG, 0x00);
    if (ret != ESP_OK) return ret;
    
    /* Set gain (0dB) */
    ret = es7210_set_gain(0);
    if (ret != ESP_OK) return ret;
    
    /* Configure for 16kHz sample rate */
    ret = es7210_set_sample_rate(AUDIO_SAMPLE_RATE);
    if (ret != ESP_OK) return ret;
    
    ESP_LOGI(TAG, "ES7210 initialized successfully");
    return ESP_OK;
}

/**
 * Deinitialize ES7210 ADC
 */
esp_err_t es7210_deinit(void)
{
    /* Reset the chip */
    return es7210_write_reg(ES7210_RESET_REG, 0x01);
}

/**
 * Configure ES7210 for specific sample rate
 */
esp_err_t es7210_set_sample_rate(uint32_t sample_rate)
{
    esp_err_t ret = ESP_OK;
    
    /* Configure ADC sample rate based on MCLK/fs ratio
     * For 16kHz with MCLK = 256fs = 4.096MHz
     */
    switch (sample_rate) {
        case 16000:
            /* Configure for 16kHz */
            ret = es7210_write_reg(ES7210_CLOCK_DIV_REG, 0x06);
            break;
        case 44100:
            ret = es7210_write_reg(ES7210_CLOCK_DIV_REG, 0x04);
            break;
        case 48000:
            ret = es7210_write_reg(ES7210_CLOCK_DIV_REG, 0x04);
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
 * Set gain for all ADC channels
 */
esp_err_t es7210_set_gain(uint8_t gain_db)
{
    /* Clamp gain to valid range (0-42 dB) */
    if (gain_db > 42) {
        gain_db = 42;
    }
    
    /* ES7210 gain register: 0.5dB steps, 0x00 = 0dB, 0x54 = 42dB */
    uint8_t gain_reg = gain_db * 2;
    
    esp_err_t ret = es7210_write_reg(ES7210_ADC1_CFG_REG, 0x20 | (gain_reg & 0x1F));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set gain");
        return ret;
    }
    
    ESP_LOGI(TAG, "Gain set to %d dB", gain_db);
    return ESP_OK;
}