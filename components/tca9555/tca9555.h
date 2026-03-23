#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TCA9555 I2C GPIO Expander
 * Address: 0x20 (A0=A1=A2=0)
 * 
 * Registers:
 *   0x00: Input port register (read)
 *   0x01: Output port register (write)
 *   0x02: Polarity inversion register (write)
 *   0x03: Configuration register (write, 1=input, 0=output)
 */

#define TCA9555_I2C_ADDR         0x20
#define TCA9555_REG_INPUT        0x00
#define TCA9555_REG_OUTPUT       0x01
#define TCA9555_REG_POLINV       0x02
#define TCA9555_REG_CONFIG       0x03

#define TCA9555_PIN_0   (1 << 0)
#define TCA9555_PIN_1   (1 << 1)
#define TCA9555_PIN_2   (1 << 2)
#define TCA9555_PIN_3   (1 << 3)
#define TCA9555_PIN_4   (1 << 4)
#define TCA9555_PIN_5   (1 << 5)
#define TCA9555_PIN_6   (1 << 6)
#define TCA9555_PIN_7   (1 << 7)
#define TCA9555_PIN_8   (1 << 0)  /* Port 2 starts at bit 0 */
#define TCA9555_PIN_9   (1 << 1)
#define TCA9555_PIN_10  (1 << 2)
#define TCA9555_PIN_11  (1 << 3)

/* Opaque handle type - must be a pointer */
typedef struct tca9555_dev *tca9555_handle_t;


/**
 * @brief Initialize TCA9555 GPIO expander
 */
esp_err_t tca9555_init(i2c_master_bus_handle_t bus, tca9555_handle_t *out_handle);


/**
 * @brief Set GPIO direction (1=input, 0=output)
 */
esp_err_t tca9555_set_direction(tca9555_handle_t h, uint16_t mask, uint16_t dir);


/**
 * @brief Set output level (1=high, 0=low, only for outputs)
 */
esp_err_t tca9555_set_level(tca9555_handle_t h, uint16_t mask, uint16_t level);


/**
 * @brief Read input level
 */
esp_err_t tca9555_get_level(tca9555_handle_t h, uint16_t *out_level);


/**
 * @brief Set pin as output and high (enable)
 */
esp_err_t tca9555_output_enable(tca9555_handle_t h, uint8_t pin);


/**
 * @brief Set pin as output and low (disable)
 */
esp_err_t tca9555_output_disable(tca9555_handle_t h, uint8_t pin);

#ifdef __cplusplus
}
#endif
