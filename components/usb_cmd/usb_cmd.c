/**
 * ESP001 USB Command Interface Implementation
 */

#include <string.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "usb_cmd.h"

static const char *TAG = "USB_CMD";

/* UART configuration */
#define USB_UART_NUM       UART_NUM_0
#define USB_UART_BUF_SIZE  512
#define USB_UART_BAUD      115200

/* Parser states */
typedef enum {
    PARSE_STATE_IDLE,
    PARSE_STATE_LEN_L,
    PARSE_STATE_LEN_H,
    PARSE_STATE_SEQ,
    PARSE_STATE_CMD,
    PARSE_STATE_DATA,
    PARSE_STATE_CRC_L,
    PARSE_STATE_CRC_H,
    PARSE_STATE_EOF,
} parse_state_t;

/* Context structure */
typedef struct {
    parse_state_t state;
    usb_cmd_frame_t rx_frame;
    uint16_t data_index;
    uint8_t tx_seq;
    usb_cmd_handler_t handlers[256];
    usb_state_t device_state;
    usb_config_t config;
    bool initialized;
} usb_cmd_ctx_t;

static usb_cmd_ctx_t s_ctx = {0};

/**
 * Calculate CRC16 (CCITT)
 */
uint16_t usb_cmd_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    
    return crc;
}

/**
 * Send response frame
 */
esp_err_t usb_cmd_send_response(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    if (len > USB_CMD_MAX_DATA) {
        return ESP_ERR_INVALID_SIZE;
    }
    
    uint8_t frame[USB_CMD_MAX_DATA + 10];
    uint16_t idx = 0;
    
    /* Build frame */
    frame[idx++] = USB_CMD_SOF;
    frame[idx++] = len & 0xFF;
    frame[idx++] = (len >> 8) & 0xFF;
    frame[idx++] = s_ctx.tx_seq++;
    frame[idx++] = cmd;
    
    if (len > 0 && data != NULL) {
        memcpy(&frame[idx], data, len);
        idx += len;
    }
    
    /* Calculate CRC (from LEN to DATA) */
    uint16_t crc = usb_cmd_crc16(&frame[1], 4 + len);
    frame[idx++] = crc & 0xFF;
    frame[idx++] = (crc >> 8) & 0xFF;
    frame[idx++] = USB_CMD_EOF;
    
    /* Send frame */
    return uart_write_bytes(USB_UART_NUM, frame, idx);
}

/**
 * Send ACK response
 */
static esp_err_t send_ack(uint8_t seq)
{
    uint8_t data[] = {seq};
    return usb_cmd_send_response(USB_CMD_ACK, data, 1);
}

/**
 * Send NACK response
 */
static esp_err_t send_nack(uint8_t seq, uint8_t error)
{
    uint8_t data[] = {seq, error};
    return usb_cmd_send_response(USB_CMD_NACK, data, 2);
}

/**
 * Process received frame
 */
static void process_frame(void)
{
    usb_cmd_frame_t *frame = &s_ctx.rx_frame;
    
    /* Verify EOF */
    if (frame->eof != USB_CMD_EOF) {
        ESP_LOGW(TAG, "Invalid EOF: 0x%02X", frame->eof);
        send_nack(frame->seq, USB_ERR_INVALID_FRAME);
        return;
    }
    
    /* Verify CRC */
    uint16_t calc_crc = usb_cmd_crc16((uint8_t*)frame + 1, 4 + frame->length);
    if (calc_crc != frame->crc) {
        ESP_LOGW(TAG, "CRC mismatch: calc=0x%04X, recv=0x%04X", calc_crc, frame->crc);
        send_nack(frame->seq, USB_ERR_CRC_FAILED);
        return;
    }
    
    ESP_LOGI(TAG, "Received command: 0x%02X, len=%d, seq=%d", 
             frame->cmd, frame->length, frame->seq);
    
    /* Check for registered handler */
    if (s_ctx.handlers[frame->cmd] != NULL) {
        esp_err_t ret = s_ctx.handlers[frame->cmd](frame->cmd, frame->data, frame->length);
        if (ret == ESP_OK) {
            send_ack(frame->seq);
        } else {
            send_nack(frame->seq, USB_ERR_INVALID_PARAM);
        }
    } else {
        /* Default command processing */
        switch (frame->cmd) {
            case USB_CMD_STATUS:
                handle_status_command(frame->seq);
                break;
                
            case USB_CMD_CONNECT_TO:
                handle_connect_to(frame->data, frame->length, frame->seq);
                break;
                
            case USB_CMD_CONNECT:
                handle_connect(frame->seq);
                break;
                
            case USB_CMD_DISCONNECT:
                handle_disconnect(frame->seq);
                break;
                
            case USB_CMD_SET_VOLUME:
                handle_set_volume(frame->data, frame->length, frame->seq);
                break;
                
            case USB_CMD_SHOW_CONFIG:
                handle_show_config(frame->seq);
                break;
                
            case USB_CMD_RESET:
                handle_reset(frame->seq);
                break;
                
            default:
                ESP_LOGW(TAG, "Unknown command: 0x%02X", frame->cmd);
                send_nack(frame->seq, USB_ERR_UNKNOWN_CMD);
                break;
        }
    }
}

/**
 * Parse received byte
 */
static void parse_byte(uint8_t byte)
{
    switch (s_ctx.state) {
        case PARSE_STATE_IDLE:
            if (byte == USB_CMD_SOF) {
                s_ctx.rx_frame.sof = byte;
                s_ctx.state = PARSE_STATE_LEN_L;
            }
            break;
            
        case PARSE_STATE_LEN_L:
            s_ctx.rx_frame.length = byte;
            s_ctx.state = PARSE_STATE_LEN_H;
            break;
            
        case PARSE_STATE_LEN_H:
            s_ctx.rx_frame.length |= (uint16_t)byte << 8;
            if (s_ctx.rx_frame.length > USB_CMD_MAX_DATA) {
                ESP_LOGW(TAG, "Frame too long: %d", s_ctx.rx_frame.length);
                s_ctx.state = PARSE_STATE_IDLE;
            } else {
                s_ctx.state = PARSE_STATE_SEQ;
            }
            break;
            
        case PARSE_STATE_SEQ:
            s_ctx.rx_frame.seq = byte;
            s_ctx.state = PARSE_STATE_CMD;
            break;
            
        case PARSE_STATE_CMD:
            s_ctx.rx_frame.cmd = byte;
            s_ctx.data_index = 0;
            if (s_ctx.rx_frame.length > 0) {
                s_ctx.state = PARSE_STATE_DATA;
            } else {
                s_ctx.state = PARSE_STATE_CRC_L;
            }
            break;
            
        case PARSE_STATE_DATA:
            s_ctx.rx_frame.data[s_ctx.data_index++] = byte;
            if (s_ctx.data_index >= s_ctx.rx_frame.length) {
                s_ctx.state = PARSE_STATE_CRC_L;
            }
            break;
            
        case PARSE_STATE_CRC_L:
            s_ctx.rx_frame.crc = byte;
            s_ctx.state = PARSE_STATE_CRC_H;
            break;
            
        case PARSE_STATE_CRC_H:
            s_ctx.rx_frame.crc |= (uint16_t)byte << 8;
            s_ctx.state = PARSE_STATE_EOF;
            break;
            
        case PARSE_STATE_EOF:
            s_ctx.rx_frame.eof = byte;
            process_frame();
            s_ctx.state = PARSE_STATE_IDLE;
            break;
    }
}

/* Default command handlers */

esp_err_t handle_status_command(uint8_t seq)
{
    uint8_t data[4];
    data[0] = seq;
    data[1] = (uint8_t)s_ctx.device_state;
    data[2] = s_ctx.config.configured ? 1 : 0;
    data[3] = s_ctx.config.volume;
    
    return usb_cmd_send_response(USB_CMD_STATUS_RESP, data, 4);
}

esp_err_t handle_connect_to(const uint8_t *data, uint16_t len, uint8_t seq)
{
    if (len == 0 || len >= sizeof(s_ctx.config.server_url)) {
        send_nack(seq, USB_ERR_INVALID_PARAM);
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(s_ctx.config.server_url, data, len);
    s_ctx.config.server_url[len] = '\0';
    s_ctx.config.configured = true;
    
    ESP_LOGI(TAG, "Server URL set: %s", s_ctx.config.server_url);
    
    send_ack(seq);
    return ESP_OK;
}

esp_err_t handle_connect(uint8_t seq)
{
    if (!s_ctx.config.configured) {
        ESP_LOGW(TAG, "Not configured");
        send_nack(seq, USB_ERR_INVALID_PARAM);
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Connect command received");
    s_ctx.device_state = USB_STATE_CONNECTING;
    
    /* TODO: Trigger WebSocket connection */
    
    send_ack(seq);
    return ESP_OK;
}

esp_err_t handle_disconnect(uint8_t seq)
{
    ESP_LOGI(TAG, "Disconnect command received");
    s_ctx.device_state = USB_STATE_DISCONNECTING;
    
    /* TODO: Trigger WebSocket disconnection */
    
    send_ack(seq);
    return ESP_OK;
}

esp_err_t handle_set_volume(const uint8_t *data, uint16_t len, uint8_t seq)
{
    if (len != 1 || data[0] > 100) {
        send_nack(seq, USB_ERR_INVALID_PARAM);
        return ESP_ERR_INVALID_ARG;
    }
    
    s_ctx.config.volume = data[0];
    ESP_LOGI(TAG, "Volume set to %d%%", s_ctx.config.volume);
    
    /* TODO: Apply volume to audio driver */
    
    send_ack(seq);
    return ESP_OK;
}

esp_err_t handle_show_config(uint8_t seq)
{
    ESP_LOGI(TAG, "=== Current Configuration ===");
    ESP_LOGI(TAG, "Server URL: %s", s_ctx.config.server_url);
    ESP_LOGI(TAG, "Volume: %d%%", s_ctx.config.volume);
    ESP_LOGI(TAG, "Configured: %s", s_ctx.config.configured ? "Yes" : "No");
    
    send_ack(seq);
    return ESP_OK;
}

esp_err_t handle_reset(uint8_t seq)
{
    ESP_LOGI(TAG, "Factory reset command received");
    
    memset(&s_ctx.config, 0, sizeof(s_ctx.config));
    s_ctx.device_state = USB_STATE_IDLE;
    
    /* TODO: Clear persistent storage */
    
    send_ack(seq);
    return ESP_OK;
}

/**
 * UART event task
 */
static void uart_event_task(void *pvParameters)
{
    uint8_t data[128];
    
    ESP_LOGI(TAG, "USB command task started");
    
    while (1) {
        int len = uart_read_bytes(USB_UART_NUM, data, sizeof(data), pdMS_TO_TICKS(10));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                parse_byte(data[i]);
            }
        }
    }
}

/**
 * Initialize USB command interface
 */
esp_err_t usb_cmd_init(void)
{
    if (s_ctx.initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing USB command interface...");
    
    /* Configure UART */
    uart_config_t uart_config = {
        .baud_rate = USB_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    ESP_ERROR_CHECK(uart_driver_install(USB_UART_NUM, USB_UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(USB_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(USB_UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, 
                                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    
    /* Initialize context */
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.state = PARSE_STATE_IDLE;
    s_ctx.device_state = USB_STATE_IDLE;
    s_ctx.config.volume = 50;
    s_ctx.tx_seq = 0;
    
    /* Create parser task */
    xTaskCreate(uart_event_task, "usb_cmd_task", 4096, NULL, 5, NULL);
    
    s_ctx.initialized = true;
    ESP_LOGI(TAG, "USB command interface initialized");
    
    return ESP_OK;
}

/**
 * Deinitialize USB command interface
 */
esp_err_t usb_cmd_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_OK;
    }
    
    uart_driver_delete(USB_UART_NUM);
    s_ctx.initialized = false;
    
    return ESP_OK;
}

/**
 * Register command handler
 */
esp_err_t usb_cmd_register_handler(uint8_t cmd, usb_cmd_handler_t handler)
{
    if (cmd >= 256) {
        return ESP_ERR_INVALID_ARG;
    }
    
    s_ctx.handlers[cmd] = handler;
    return ESP_OK;
}

/**
 * Get current state
 */
usb_state_t usb_cmd_get_state(void)
{
    return s_ctx.device_state;
}

/**
 * Set device state
 */
void usb_cmd_set_state(usb_state_t state)
{
    s_ctx.device_state = state;
}

/**
 * Get configuration
 */
const usb_config_t* usb_cmd_get_config(void)
{
    return &s_ctx.config;
}