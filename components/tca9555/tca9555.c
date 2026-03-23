/**
 * TCA9555 - 16-bit I2C GPIO Expander Driver
 */

#include "tca9555.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "TCA9555";

/* Private struct */
struct tca9555_dev {
    i2c_master_dev_handle_t dev;
};


esp_err_t tca9555_init(i2c_master_bus_handle_t bus, tca9555_handle_t *out_handle)
{
    struct tca9555_dev *dev = calloc(1, sizeof(struct tca9555_dev));
    if (!dev) return ESP_ERR_NO_MEM;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9555_I2C_ADDR,
        .scl_speed_hz = 100000,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &dev->dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add TCA9555: %s", esp_err_to_name(ret));
        free(dev);
        return ret;
    }

    ESP_LOGI(TAG, "TCA9555 initialized on I2C addr 0x%02X", TCA9555_I2C_ADDR);

    /* Configure all pins as outputs (1=input, 0=output) */
    uint8_t cfg_cmd[3] = { TCA9555_REG_CONFIG, 0x00, 0x00 };
    ret = i2c_master_transmit(dev->dev, cfg_cmd, 3, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure TCA9555: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(dev->dev);
        free(dev);
        return ret;
    }

    /* Set all outputs low */
    uint8_t out_cmd[3] = { TCA9555_REG_OUTPUT, 0x00, 0x00 };
    ret = i2c_master_transmit(dev->dev, out_cmd, 3, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set TCA9555 output: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "TCA9555: all pins output, all low");
    *out_handle = (tca9555_handle_t)dev;
    return ESP_OK;
}


esp_err_t tca9555_set_direction(tca9555_handle_t h, uint16_t mask, uint16_t dir)
{
    struct tca9555_dev *dev = (struct tca9555_dev *)h;
    if (!dev) return ESP_ERR_INVALID_STATE;

    uint8_t cfg_cmd[3] = {
        TCA9555_REG_CONFIG,
        (uint8_t)(dir & 0xFF),
        (uint8_t)((dir >> 8) & 0xFF)
    };
    return i2c_master_transmit(dev->dev, cfg_cmd, 3, pdMS_TO_TICKS(100));
}


esp_err_t tca9555_set_level(tca9555_handle_t h, uint16_t mask, uint16_t level)
{
    struct tca9555_dev *dev = (struct tca9555_dev *)h;
    if (!dev) return ESP_ERR_INVALID_STATE;

    uint8_t out_cmd[3] = {
        TCA9555_REG_OUTPUT,
        (uint8_t)(level & 0xFF),
        (uint8_t)((level >> 8) & 0xFF)
    };
    return i2c_master_transmit(dev->dev, out_cmd, 3, pdMS_TO_TICKS(100));
}


esp_err_t tca9555_get_level(tca9555_handle_t h, uint16_t *out_level)
{
    struct tca9555_dev *dev = (struct tca9555_dev *)h;
    if (!dev) return ESP_ERR_INVALID_STATE;

    uint8_t reg = TCA9555_REG_INPUT;
    uint8_t data[2];
    esp_err_t ret = i2c_master_transmit_receive(dev->dev, &reg, 1, data, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) return ret;
    *out_level = (data[1] << 8) | data[0];
    return ESP_OK;
}


esp_err_t tca9555_output_enable(tca9555_handle_t h, uint8_t pin)
{
    struct tca9555_dev *dev = (struct tca9555_dev *)h;
    if (!dev) return ESP_ERR_INVALID_STATE;

    /* Set pin as output (0 in config) and high (1 in output) */
    uint8_t port = pin < 8 ? 0 : 1;
    uint8_t bit = pin % 8;

    /* Read current config */
    uint8_t cfg_reg[2] = { TCA9555_REG_CONFIG + port };
    uint8_t cfg_val;
    esp_err_t ret = i2c_master_transmit_receive(dev->dev, cfg_reg, 1, &cfg_val, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) return ret;

    /* Set as output (clear bit) */
    cfg_val &= ~(1 << bit);
    uint8_t cfg_cmd[2] = { TCA9555_REG_CONFIG + port, cfg_val };
    ret = i2c_master_transmit(dev->dev, cfg_cmd, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) return ret;

    /* Read current output */
    uint8_t out_reg[2] = { TCA9555_REG_OUTPUT + port };
    uint8_t out_val;
    ret = i2c_master_transmit_receive(dev->dev, out_reg, 1, &out_val, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) return ret;

    /* Set high */
    out_val |= (1 << bit);
    uint8_t out_cmd[2] = { TCA9555_REG_OUTPUT + port, out_val };
    return i2c_master_transmit(dev->dev, out_cmd, 2, pdMS_TO_TICKS(100));
}


esp_err_t tca9555_output_disable(tca9555_handle_t h, uint8_t pin)
{
    struct tca9555_dev *dev = (struct tca9555_dev *)h;
    if (!dev) return ESP_ERR_INVALID_STATE;

    uint8_t port = pin < 8 ? 0 : 1;
    uint8_t bit = pin % 8;

    /* Read current output register */
    uint8_t reg = TCA9555_REG_OUTPUT;
    uint8_t out_val;
    esp_err_t ret = i2c_master_transmit_receive(dev->dev, &reg, 1, &out_val, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) return ret;

    /* Set low */
    out_val &= ~(1 << bit);
    uint8_t cmd[2] = { TCA9555_REG_OUTPUT, out_val };
    return i2c_master_transmit(dev->dev, cmd, 2, pdMS_TO_TICKS(100));
}
