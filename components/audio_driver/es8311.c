/**
 * ES8311 - Low Power Audio DAC Driver Implementation
 * 
 * Based on Everest Semi ES8311 datasheet
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

/* Persistent I2C device handle for ES8311 */
static i2c_master_dev_handle_t s_es8311_dev = NULL;
static bool s_es8311_initialized = false;

static uint8_t s_volume = 80;


/* ES8311 register definitions (from datasheet) */
#define ES8311_REG_RESET          0x00
#define ES8311_REG_CLK_MANAGER    0x01
#define ES8311_REG_PLL1           0x02
#define ES8311_REG_PLL2           0x03
#define ES8311_REG_CLK_TREE       0x04
#define ES8311_REG_ADC_DAC        0x05
#define ES8311_REG_AUTOMATIC      0x06
#define ES8311_REG_DAC_OFFSET     0x07
#define ES8311_REG_ADC_OFFSET     0x08
#define ES8311_REG_DAC_SETUP1     0x10
#define ES8311_REG_DAC_SETUP2     0x11
#define ES8311_REG_DAC_TDM        0x12
#define ES8311_REG_DAC_INIT       0x13
#define ES8311_REG_DAC_VP         0x14
#define ES8311_REG_DAC_VREFBIAS   0x15
#define ES8311_REG_DAC_SETUP3     0x16
#define ES8311_REG_DAC_SETUP4     0x17
#define ES8311_REG_DAC_CHEPA      0x18
#define ES8311_REG_DAC_CHEPB      0x19
#define ES8311_REG_DAC_INIT1      0x1A
#define ES8311_REG_DAC_VOL_L      0x1B
#define ES8311_REG_DAC_VOL_R      0x1C
#define ES8311_REG_DAC_MIXER      0x1D
#define ES8311_REG_DAC_INIT2      0x1E
#define ES8311_REG_DAC_HP         0x1F
#define ES8311_REG_DAC_HP2        0x20


/**
 * Create persistent I2C device for ES8311
 */
static esp_err_t es8311_create_dev(void)
{
    if (s_es8311_dev != NULL) return ESP_OK;
    
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &s_es8311_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ES8311 I2C device: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "ES8311 I2C device created (freq=%dHz)", I2C_MASTER_FREQ_HZ);
    return ESP_OK;
}


/**
 * Read ES8311 register
 */
esp_err_t es8311_read_reg(uint8_t reg, uint8_t *value)
{
    if (s_es8311_dev == NULL) return ESP_ERR_INVALID_STATE;
    
    esp_err_t ret = i2c_master_transmit_receive(s_es8311_dev, &reg, 1, value, 1, pdMS_TO_TICKS(200));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C read reg 0x%02X failed: %s", reg, esp_err_to_name(ret));
    }
    return ret;
}

/**
 * Write ES8311 register
 */
esp_err_t es8311_write_reg(uint8_t reg, uint8_t value)
{
    if (s_es8311_dev == NULL) return ESP_ERR_INVALID_STATE;
    
    uint8_t write_buf[2] = {reg, value};
    esp_err_t ret = i2c_master_transmit(s_es8311_dev, write_buf, 2, pdMS_TO_TICKS(200));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C write reg 0x%02X=0x%02X failed: %s", reg, value, esp_err_to_name(ret));
    }
    return ret;
}


/**
 * Initialize ES8311 DAC
 * 
 * Complete initialization based on ES8311 typical application circuit
 */
esp_err_t es8311_init(void)
{
    esp_err_t ret = ESP_OK;
    uint8_t chip_id = 0;
    
    ESP_LOGI(TAG, "Initializing ES8311 DAC...");
    
    /* Create persistent I2C device */
    ret = es8311_create_dev();
    if (ret != ESP_OK) return ret;
    
    /* Read chip ID to verify I2C communication */
    ret = es8311_read_reg(ES8311_CHIP_ID_REG, &chip_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C communication failed: %s. Check wiring!", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "ES8311 Chip ID: 0x%02X (expected 0x83)", chip_id);
    if (chip_id != ES8311_CHIP_ID) {
        ESP_LOGW(TAG, "Unexpected chip ID!");
    }
    
    /* Reset entire chip */
    ret = es8311_write_reg(ES8311_REG_RESET, 0x1F);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(30));
    
    /* Release reset, normal operation */
    ret = es8311_write_reg(ES8311_REG_RESET, 0x00);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(20));
    
    /* Set clock manager: CLKODIV2=1 (div2), MCLK from external, slave mode */
    ret = es8311_write_reg(ES8311_REG_CLK_MANAGER, 0x30);
    if (ret != ESP_OK) return ret;
    
    /* Configure PLL for 16kHz */
    ret = es8311_write_reg(ES8311_REG_PLL1, 0x40);  /* N=4 */
    if (ret != ESP_OK) return ret;
    ret = es8311_write_reg(ES8311_REG_PLL2, 0x00);
    if (ret != ESP_OK) return ret;
    
    /* Clock tree: CLKO divisor and enable */
    ret = es8311_write_reg(ES8311_REG_CLK_TREE, 0x00);
    if (ret != ESP_OK) return ret;
    
    /* Power up analog blocks: DAC enabled, bias enabled */
    ret = es8311_write_reg(ES8311_REG_DAC_INIT, 0x00);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));
    ret = es8311_write_reg(ES8311_REG_DAC_INIT, 0x3C);  /* BIASEN=1, HPEN=0, DAC2EN=1 */
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));
    
    /* DAC setup: enable, unmute, stereo */
    /* reg 0x10: DACEN, LMUTE=0, RMUTE=0, LRMP=0, DACFS=0, DACORS=0, DACMONO=0 */
    ret = es8311_write_reg(ES8311_REG_DAC_SETUP1, 0x39);
    if (ret != ESP_OK) return ret;
    
    /* I2S interface: MSB first, Philips I2S (1-bit delay), 16-bit, slave */
    /* reg 0x11: FMT=0(MSB), JUST=1(I2S), DLY=1(1clk), TDM=0, ACLKINV=0, BCKINV=0, WL=00(16bit) */
    /* 0x02 = JUST=1 (I2S mode), DLY=1 (1-bit delay for I2S) */
    ret = es8311_write_reg(ES8311_REG_DAC_SETUP2, 0x02);
    if (ret != ESP_OK) return ret;
    
    /* TDM slot configuration */
    ret = es8311_write_reg(ES8311_REG_DAC_TDM, 0x00);
    if (ret != ESP_OK) return ret;
    
    /* Set DAC volume to 0dB (0xC0) initially for both channels */
    ret = es8311_write_reg(ES8311_REG_DAC_VOL_L, 0xC0);
    if (ret != ESP_OK) return ret;
    ret = es8311_write_reg(ES8311_REG_DAC_VOL_R, 0xC0);
    if (ret != ESP_OK) return ret;
    
    /* DAC mixer: enable, soft transition, scatter */
    ret = es8311_write_reg(ES8311_REG_DAC_MIXER, 0x89);
    if (ret != ESP_OK) return ret;
    
    /* Set the actual volume */
    ret = es8311_set_volume(s_volume);
    if (ret != ESP_OK) return ret;
    
    /* Verify key registers */
    uint8_t val = 0;
    ESP_LOGI(TAG, "=== ES8311 Register Dump ===");
    for (uint8_t r = 0x00; r <= 0x20; r++) {
        if (es8311_read_reg(r, &val) == ESP_OK) {
            ESP_LOGI(TAG, "  [0x%02X] = 0x%02X", r, val);
        } else {
            ESP_LOGI(TAG, "  [0x%02X] = ERR", r);
        }
    }
    ESP_LOGI(TAG, "============================");
    
    ESP_LOGI(TAG, "ES8311 initialized successfully");
    ESP_LOGI(TAG, "  Format: I2S, 16-bit, stereo");
    ESP_LOGI(TAG, "  Clock: MCLK slave (256fs)");
    ESP_LOGI(TAG, "  Volume: %d%%", s_volume);
    
    return ESP_OK;
}


/**
 * Deinitialize ES8311 DAC
 */
esp_err_t es8311_deinit(void)
{
    es8311_set_output_enable(false);
    return es8311_write_reg(ES8311_REG_RESET, 0x1F);
}


/**
 * Configure ES8311 sample rate
 */
esp_err_t es8311_set_sample_rate(uint32_t sample_rate)
{
    ESP_LOGI(TAG, "Sample rate: %lu Hz", sample_rate);
    return ESP_OK;
}


/**
 * Set output volume (0-100)
 * 
 * ES8311 volume control:
 * - 0x00 = mute
 * - 0xC0 = 0dB (unity gain)
 * - 0xFF = +32dB (max)
 * Range: ~-95dB to +32dB in 0.5dB steps
 */
esp_err_t es8311_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    
    uint8_t vol_reg;
    if (volume == 0) {
        vol_reg = 0x00;  /* Mute */
    } else if (volume <= 20) {
        /* Very quiet: 0x01 to 0x3F */
        vol_reg = 0x01 + (volume * 0x3E / 20);
    } else if (volume <= 70) {
        /* Normal: 0x40 to 0xBF (roughly -30dB to 0dB) */
        vol_reg = 0x40 + ((volume - 20) * 0x7F / 50);
    } else {
        /* Loud: 0xC0 to 0xFF (0dB to +32dB) */
        vol_reg = 0xC0 + ((volume - 70) * 0x3F / 30);
    }
    
    esp_err_t ret = es8311_write_reg(ES8311_REG_DAC_VOL_L, vol_reg);
    if (ret != ESP_OK) return ret;
    ret = es8311_write_reg(ES8311_REG_DAC_VOL_R, vol_reg);
    if (ret != ESP_OK) return ret;
    
    s_volume = volume;
    ESP_LOGI(TAG, "Volume: %d%% (reg=0x%02X)", volume, vol_reg);
    return ESP_OK;
}


/**
 * Get output volume
 */
esp_err_t es8311_get_volume(uint8_t *volume)
{
    if (volume == NULL) return ESP_ERR_INVALID_ARG;
    *volume = s_volume;
    return ESP_OK;
}


/**
 * Enable/disable DAC output
 */
esp_err_t es8311_set_output_enable(bool enable)
{
    uint8_t val = enable ? 0x39 : 0x00;
    return es8311_write_reg(ES8311_REG_DAC_SETUP1, val);
}


/**
 * Public: Read ES8311 register (for debugging via UART)
 */
esp_err_t es8311_read_reg_public(uint8_t reg, uint8_t *value)
{
    return es8311_read_reg(reg, value);
}
