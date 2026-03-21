/**
 * ES8311 - Low Power Audio DAC Driver
 * 
 * Datasheet: https://www.everest-semi.com/pdf/ES8311%20PB.pdf
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

/* ES8311 register addresses */
#define ES8311_RESET_REG        0x00
#define ES8311_CLOCK_MGMT_REG   0x01
#define ES8311_PLL_CFG1_REG     0x02
#define ES8311_PLL_CFG2_REG     0x03
#define ES8311_DAC_CFG_REG      0x04
#define ES8311_DAC_OUT_CFG_REG  0x05
#define ES8311_VOLUME_REG       0x06
#define ES8311_CHIP_ID_REG      0xFD
#define ES8311_CHIP_VERSION_REG 0xFE

/* ES8311 chip ID */
#define ES8311_CHIP_ID          0x83

/**
 * Initialize ES8311 DAC
 * 
 * @return ESP_OK on success
 */
esp_err_t es8311_init(void);

/**
 * Deinitialize ES8311 DAC
 * 
 * @return ESP_OK on success
 */
esp_err_t es8311_deinit(void);

/**
 * Read ES8311 register
 * 
 * @param reg Register address
 * @param value Pointer to store register value
 * @return ESP_OK on success
 */
esp_err_t es8311_read_reg(uint8_t reg, uint8_t *value);

/**
 * Write ES8311 register
 * 
 * @param reg Register address
 * @param value Value to write
 * @return ESP_OK on success
 */
esp_err_t es8311_write_reg(uint8_t reg, uint8_t value);

/**
 * Configure ES8311 for specific sample rate
 * 
 * @param sample_rate Sample rate in Hz (e.g., 16000)
 * @return ESP_OK on success
 */
esp_err_t es8311_set_sample_rate(uint32_t sample_rate);

/**
 * Set output volume
 * 
 * @param volume Volume level (0-100)
 * @return ESP_OK on success
 */
esp_err_t es8311_set_volume(uint8_t volume);

/**
 * Get output volume
 * 
 * @param volume Pointer to store volume level (0-100)
 * @return ESP_OK on success
 */
esp_err_t es8311_get_volume(uint8_t *volume);

/**
 * Enable/disable DAC output
 * 
 * @param enable true to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t es8311_set_output_enable(bool enable);