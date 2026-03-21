/**
 * ESP001 USB Command Interface
 * 
 * Serial command protocol for ESP32-S3 configuration and control
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Protocol constants */
#define USB_CMD_SOF         0xAA    /* Start of frame */
#define USB_CMD_EOF         0x55    /* End of frame */
#define USB_CMD_MAX_DATA    256     /* Maximum data length */
#define USB_CMD_TIMEOUT_MS  1000    /* Command timeout */

/* Command codes */
typedef enum {
    USB_CMD_CONNECT_TO     = 0x01,  /* Set WebSocket server URL */
    USB_CMD_CONNECT        = 0x02,  /* Initiate WebSocket connection */
    USB_CMD_DISCONNECT     = 0x03,  /* Disconnect WebSocket */
    USB_CMD_SET_SOUND      = 0x04,  /* Set sound effect file */
    USB_CMD_SET_VOLUME     = 0x05,  /* Set volume (0-100) */
    USB_CMD_STATUS         = 0x06,  /* Query current status */
    USB_CMD_SHOW_CONFIG    = 0x07,  /* Show current configuration */
    USB_CMD_RESET          = 0x08,  /* Factory reset */
    
    /* Response codes */
    USB_CMD_ACK            = 0x81,  /* Acknowledge */
    USB_CMD_NACK           = 0x82,  /* Negative acknowledge */
    USB_CMD_STATUS_RESP    = 0x83,  /* Status response */
} usb_cmd_code_t;

/* Error codes for NACK */
typedef enum {
    USB_ERR_OK             = 0x00,
    USB_ERR_INVALID_FRAME  = 0x01,  /* Invalid frame format */
    USB_ERR_CRC_FAILED     = 0x02,  /* CRC check failed */
    USB_ERR_UNKNOWN_CMD    = 0x03,  /* Unknown command */
    USB_ERR_INVALID_PARAM  = 0x04,  /* Invalid parameter */
    USB_ERR_BUSY           = 0x05,  /* Device busy */
    USB_ERR_WS_NOT_CONNECTED = 0x06, /* WebSocket not connected */
    USB_ERR_WS_CONNECT_FAILED = 0x07, /* WebSocket connection failed */
    USB_ERR_HARDWARE       = 0x08,  /* Hardware error */
    USB_ERR_MEMORY         = 0x09,  /* Memory error */
} usb_err_code_t;

/* Device states */
typedef enum {
    USB_STATE_IDLE,         /* Idle, waiting for commands */
    USB_STATE_CONNECTING,   /* Connecting to server */
    USB_STATE_CONNECTED,    /* Connected, streaming audio */
    USB_STATE_DISCONNECTING,/* Disconnecting */
    USB_STATE_ERROR,        /* Error state */
} usb_state_t;

/* Command frame structure */
typedef struct {
    uint8_t sof;            /* Start of frame (0xAA) */
    uint16_t length;        /* Data length */
    uint8_t seq;            /* Sequence number */
    uint8_t cmd;            /* Command code */
    uint8_t data[USB_CMD_MAX_DATA];  /* Data payload */
    uint16_t crc;           /* CRC16 */
    uint8_t eof;            /* End of frame (0x55) */
} __attribute__((packed)) usb_cmd_frame_t;

/* Configuration structure */
typedef struct {
    char server_url[128];   /* WebSocket server URL */
    uint8_t volume;         /* Volume (0-100) */
    bool configured;        /* Configuration valid flag */
} usb_config_t;

/* Callback types */
typedef esp_err_t (*usb_cmd_handler_t)(uint8_t cmd, const uint8_t *data, uint16_t len);

/* Default command handlers (for internal use) */
esp_err_t handle_status_command(uint8_t seq);
esp_err_t handle_connect_to(const uint8_t *data, uint16_t len, uint8_t seq);
esp_err_t handle_connect(uint8_t seq);
esp_err_t handle_disconnect(uint8_t seq);
esp_err_t handle_set_volume(const uint8_t *data, uint16_t len, uint8_t seq);
esp_err_t handle_show_config(uint8_t seq);
esp_err_t handle_reset(uint8_t seq);

/**
 * Initialize USB command interface
 * 
 * @return ESP_OK on success
 */
esp_err_t usb_cmd_init(void);

/**
 * Deinitialize USB command interface
 * 
 * @return ESP_OK on success
 */
esp_err_t usb_cmd_deinit(void);

/**
 * Register command handler
 * 
 * @param cmd Command code
 * @param handler Handler function
 * @return ESP_OK on success
 */
esp_err_t usb_cmd_register_handler(uint8_t cmd, usb_cmd_handler_t handler);

/**
 * Send response
 * 
 * @param cmd Response command code
 * @param data Response data
 * @param len Data length
 * @return ESP_OK on success
 */
esp_err_t usb_cmd_send_response(uint8_t cmd, const uint8_t *data, uint16_t len);

/**
 * Get current state
 * 
 * @return Current device state
 */
usb_state_t usb_cmd_get_state(void);

/**
 * Set device state
 * 
 * @param state New state
 */
void usb_cmd_set_state(usb_state_t state);

/**
 * Get configuration
 * 
 * @return Pointer to configuration structure
 */
const usb_config_t* usb_cmd_get_config(void);

/**
 * Calculate CRC16
 * 
 * @param data Data buffer
 * @param len Data length
 * @return CRC16 value
 */
uint16_t usb_cmd_crc16(const uint8_t *data, uint16_t len);