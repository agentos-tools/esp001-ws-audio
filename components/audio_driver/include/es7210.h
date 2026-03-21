/**
 * ES7210 - 4-channel Audio ADC Driver
 * 
 * Datasheet: https://www.everest-semi.com/pdf/ES7210%20PB.pdf
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

/* ES7210 register addresses */
#define ES7210_RESET_REG        0x00
#define ES7210_CLOCK_CFG_REG    0x01
#define ES7210_CLOCK_DIV_REG    0x02
#define ES7210_ADC1_CFG_REG     0x03
#define ES7210_ADC2_CFG_REG     0x04
#define ES7210_ADC3_CFG_REG     0x05
#define ES7210_ADC4_CFG_REG     0x06
#define ES7210_ANALOG_CFG_REG   0x07
#define ES7210_ANALOG_CFG2_REG  0x08
#define ES7210_CHIP_ID_REG      0x40
#define ES7210_CHIP_VERSION_REG 0x41

/* ES7210 chip ID */
#define ES7210_CHIP_ID          0x72

/**
 * Initialize ES7210 ADC
 * 
 * @return ESP_OK on success
 */
esp_err_t es7210_init(void);

/**
 * Deinitialize ES7210 ADC
 * 
 * @return ESP_OK on success
 */
esp_err_t es7210_deinit(void);

/**
 * Read ES7210 register
 * 
 * @param reg Register address
 * @param value Pointer to store register value
 * @return ESP_OK on success
 */
esp_err_t es7210_read_reg(uint8_t reg, uint8_t *value);

/**
 * Write ES7210 register
 * 
 * @param reg Register address
 * @param value Value to write
 * @return ESP_OK on success
 */
esp_err_t es7210_write_reg(uint8_t reg, uint8_t value);

/**
 * Configure ES7210 for specific sample rate
 * 
 * @param sample_rate Sample rate in Hz (e.g., 16000)
 * @return ESP_OK on success
 */
esp_err_t es7210_set_sample_rate(uint32_t sample_rate);

/**
 * Set gain for all ADC channels
 * 
 * @param gain_db Gain in dB (0-42)
 * @return ESP_OK on success
 */
esp_err_t es7210_set_gain(uint8_t gain_db);