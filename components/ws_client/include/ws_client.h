/**
 * ESP001 WebSocket Client
 * 
 * WebSocket client for ESP32-S3 with WiFi and reconnection support
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

/* WebSocket client states */
typedef enum {
    WS_STATE_IDLE = 0,
    WS_STATE_CONNECTING,
    WS_STATE_CONNECTED,
    WS_STATE_DISCONNECTING,
    WS_STATE_ERROR
} ws_state_t;

/* WebSocket events */
typedef enum {
    WS_EVENT_CONNECTED,
    WS_EVENT_DISCONNECTED,
    WS_EVENT_DATA,
    WS_EVENT_ERROR
} ws_event_t;

/* WebSocket configuration */
typedef struct {
    char url[128];           /* WebSocket server URL */
    char wifi_ssid[32];       /* WiFi SSID */
    char wifi_password[64];    /* WiFi password */
    int ping_interval_sec;    /* Ping interval in seconds */
    int pong_timeout_sec;     /* Pong timeout in seconds */
    int reconnect_interval_sec; /* Reconnect interval base in seconds */
    int max_reconnect_interval_sec; /* Max reconnect interval */
} ws_config_t;

/* WebSocket event callback */
typedef void (*ws_event_callback_t)(ws_event_t event, const uint8_t *data, size_t len, void *user_data);

/**
 * Initialize WebSocket client
 * 
 * @return ESP_OK on success
 */
esp_err_t ws_client_init(void);

/**
 * Deinitialize WebSocket client
 * 
 * @return ESP_OK on success
 */
esp_err_t ws_client_deinit(void);

/**
 * Configure WiFi credentials
 * 
 * @param ssid WiFi SSID
 * @param password WiFi password
 * @return ESP_OK on success
 */
esp_err_t ws_client_set_wifi(const char *ssid, const char *password);

/**
 * Configure WebSocket server URL
 * 
 * @param url WebSocket server URL (e.g., "ws://192.168.1.100:8080")
 * @return ESP_OK on success
 */
esp_err_t ws_client_set_url(const char *url);

/**
 * Connect to WebSocket server
 * 
 * @return ESP_OK on success
 */
esp_err_t ws_client_connect(void);

/**
 * Disconnect from WebSocket server
 * 
 * @return ESP_OK on success
 */
esp_err_t ws_client_disconnect(void);

/**
 * Send data to WebSocket server
 * 
 * @param data Data to send
 * @param len Length of data
 * @return ESP_OK on success
 */
esp_err_t ws_client_send(const uint8_t *data, size_t len);

/**
 * Get current state
 * 
 * @return Current WebSocket state
 */
ws_state_t ws_client_get_state(void);

/**
 * Register event callback
 * 
 * @param callback Event callback function
 * @param user_data User data passed to callback
 * @return ESP_OK on success
 */
esp_err_t ws_client_register_callback(ws_event_callback_t callback, void *user_data);

/**
 * Check if connected
 * 
 * @return true if connected
 */
bool ws_client_is_connected(void);