/**
 * ESP001 WebSocket Client Implementation
 */

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "ws_client.h"

static const char *TAG = "WS_CLIENT";

/* Default configuration */
#define DEFAULT_PING_INTERVAL_SEC      15
#define DEFAULT_PONG_TIMEOUT_SEC        5
#define DEFAULT_RECONNECT_INTERVAL_SEC  1
#define DEFAULT_MAX_RECONNECT_SEC      30

/* Event loop */
static esp_event_loop_handle_t s_event_loop = NULL;

/* Context */
typedef struct {
    ws_state_t state;
    ws_config_t config;
    ws_event_callback_t callback;
    void *user_data;
    bool wifi_connected;
    bool ws_connected;
    int reconnect_count;
    int64_t last_ping_time;
} ws_ctx_t;

static ws_ctx_t s_ctx = {0};

/* Timer handles */
static esp_timer_handle_t s_ping_timer = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;

/**
 * Log current state
 */
static void log_state(const char *action)
{
    const char *state_names[] = {"IDLE", "CONNECTING", "CONNECTED", "DISCONNECTING", "ERROR"};
    ESP_LOGI(TAG, "%s - State: %s, WiFi: %s, WS: %s", 
              action, 
              state_names[s_ctx.state],
              s_ctx.wifi_connected ? "connected" : "disconnected",
              s_ctx.ws_connected ? "connected" : "disconnected");
}

/**
 * Change state
 */
static void change_state(ws_state_t new_state)
{
    if (s_ctx.state != new_state) {
        ESP_LOGI(TAG, "State changed: %d -> %d", s_ctx.state, new_state);
        s_ctx.state = new_state;
    }
}

/* Forward declarations */
static void ping_timer_callback(void *arg);
static void reconnect_timer_callback(void *arg);

/**
 * Start ping timer
 */
static void start_ping_timer(void)
{
    if (s_ping_timer) {
        esp_timer_start_periodic(s_ping_timer, s_ctx.config.ping_interval_sec * 1000000);
    }
}

/**
 * Stop ping timer
 */
static void stop_ping_timer(void)
{
    if (s_ping_timer) {
        esp_timer_stop(s_ping_timer);
    }
}

/**
 * Start reconnect timer with exponential backoff
 */
static void start_reconnect_timer(void)
{
    if (s_reconnect_timer == NULL) {
        return;
    }
    
    /* Calculate delay with exponential backoff */
    int delay_sec = s_ctx.config.reconnect_interval_sec * (1 << s_ctx.reconnect_count);
    if (delay_sec > s_ctx.config.max_reconnect_interval_sec) {
        delay_sec = s_ctx.config.max_reconnect_interval_sec;
    }
    
    ESP_LOGI(TAG, "Reconnecting in %d seconds (attempt %d)", delay_sec, s_ctx.reconnect_count + 1);
    
    /* Convert to microseconds */
    esp_timer_start_once(s_reconnect_timer, delay_sec * 1000000);
}

/**
 * Stop reconnect timer
 */
static void stop_reconnect_timer(void)
{
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
}

/**
 * WiFi event handler
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi started, connecting to %s", s_ctx.config.wifi_ssid);
                esp_wifi_connect();
                break;
                
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi connected");
                s_ctx.wifi_connected = true;
                break;
                
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI(TAG, "WiFi disconnected");
                s_ctx.wifi_connected = false;
                s_ctx.ws_connected = false;
                stop_ping_timer();
                
                if (s_ctx.state == WS_STATE_CONNECTED || 
                    s_ctx.state == WS_STATE_CONNECTING) {
                    /* Auto reconnect */
                    s_ctx.reconnect_count = 0;
                    start_reconnect_timer();
                }
                break;
                
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ESP_LOGI(TAG, "WiFi got IP");
            /* WiFi is ready, try to connect WebSocket */
            if (s_ctx.state == WS_STATE_CONNECTING) {
                /* Will connect via timer or direct call */
            }
        }
    }
}

/**
 * Initialize WiFi
 */
static esp_err_t init_wifi(void)
{
    /* Initialize TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_init());
    
    /* Create default event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    /* Register WiFi event handler */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                              &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, 
                                              &wifi_event_handler, NULL));
    
    /* Create WiFi station */
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, s_ctx.config.wifi_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, s_ctx.config.wifi_password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    
    /* Initialize WiFi */
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    return ESP_OK;
}

/**
 * Ping timer callback
 */
static void ping_timer_callback(void *arg)
{
    (void)arg;
    if (s_ctx.ws_connected) {
        s_ctx.last_ping_time = esp_timer_get_time();
        ESP_LOGI(TAG, "Sending ping...");
        /* TODO: Send WebSocket ping */
    }
}

/**
 * Reconnect timer callback
 */
static void reconnect_timer_callback(void *arg)
{
    (void)arg;
    if (s_ctx.wifi_connected && s_ctx.state != WS_STATE_CONNECTED) {
        ESP_LOGI(TAG, "Reconnect timer triggered");
        s_ctx.reconnect_count++;
        ws_client_connect();
    }
}

/**
 * Initialize timers
 */
static esp_err_t init_timers(void)
{
    /* Create ping timer */
    const esp_timer_create_args_t ping_args = {
        .callback = ping_timer_callback,
        .arg = NULL,
        .name = "ping_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&ping_args, &s_ping_timer));
    
    /* Create reconnect timer */
    const esp_timer_create_args_t reconnect_args = {
        .callback = reconnect_timer_callback,
        .arg = NULL,
        .name = "reconnect_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_args, &s_reconnect_timer));
    
    return ESP_OK;
}

/**
 * Initialize WebSocket client
 */
esp_err_t ws_client_init(void)
{
    if (s_ctx.state != WS_STATE_IDLE) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing WebSocket client...");
    
    /* Set defaults */
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.config.ping_interval_sec = DEFAULT_PING_INTERVAL_SEC;
    s_ctx.config.pong_timeout_sec = DEFAULT_PONG_TIMEOUT_SEC;
    s_ctx.config.reconnect_interval_sec = DEFAULT_RECONNECT_INTERVAL_SEC;
    s_ctx.config.max_reconnect_interval_sec = DEFAULT_MAX_RECONNECT_SEC;
    
    /* Initialize WiFi */
    init_wifi();
    
    /* Initialize timers */
    init_timers();
    
    change_state(WS_STATE_IDLE);
    
    ESP_LOGI(TAG, "WebSocket client initialized");
    return ESP_OK;
}

/**
 * Deinitialize WebSocket client
 */
esp_err_t ws_client_deinit(void)
{
    if (s_ctx.state == WS_STATE_IDLE) {
        return ESP_OK;
    }
    
    /* Stop timers */
    stop_ping_timer();
    stop_reconnect_timer();
    
    if (s_ping_timer) {
        esp_timer_delete(s_ping_timer);
        s_ping_timer = NULL;
    }
    
    if (s_reconnect_timer) {
        esp_timer_delete(s_reconnect_timer);
        s_reconnect_timer = NULL;
    }
    
    /* Disconnect WiFi */
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    
    /* Reset state */
    memset(&s_ctx, 0, sizeof(s_ctx));
    change_state(WS_STATE_IDLE);
    
    ESP_LOGI(TAG, "WebSocket client deinitialized");
    return ESP_OK;
}

/**
 * Set WiFi credentials
 */
esp_err_t ws_client_set_wifi(const char *ssid, const char *password)
{
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }
    
    strncpy(s_ctx.config.wifi_ssid, ssid, sizeof(s_ctx.config.wifi_ssid) - 1);
    strncpy(s_ctx.config.wifi_password, password, sizeof(s_ctx.config.wifi_password) - 1);
    
    ESP_LOGI(TAG, "WiFi credentials set: %s", ssid);
    return ESP_OK;
}

/**
 * Set WebSocket server URL
 */
esp_err_t ws_client_set_url(const char *url)
{
    if (!url) {
        return ESP_ERR_INVALID_ARG;
    }
    
    strncpy(s_ctx.config.url, url, sizeof(s_ctx.config.url) - 1);
    
    ESP_LOGI(TAG, "WebSocket URL set: %s", url);
    return ESP_OK;
}

/**
 * Connect to WebSocket server
 */
esp_err_t ws_client_connect(void)
{
    if (!s_ctx.wifi_connected) {
        ESP_LOGW(TAG, "WiFi not connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_ctx.state == WS_STATE_CONNECTED || 
        s_ctx.state == WS_STATE_CONNECTING) {
        ESP_LOGW(TAG, "Already connecting or connected");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Connecting to WebSocket server: %s", s_ctx.config.url);
    change_state(WS_STATE_CONNECTING);
    
    /* TODO: Implement actual WebSocket connection
     * For now, simulate connection
     */
    
    /* Simulate successful connection after delay */
    change_state(WS_STATE_CONNECTED);
    s_ctx.ws_connected = true;
    s_ctx.reconnect_count = 0;
    
    /* Start ping timer */
    start_ping_timer();
    
    /* Notify callback */
    if (s_ctx.callback) {
        s_ctx.callback(WS_EVENT_CONNECTED, NULL, 0, s_ctx.user_data);
    }
    
    log_state("Connected");
    return ESP_OK;
}

/**
 * Disconnect from WebSocket server
 */
esp_err_t ws_client_disconnect(void)
{
    if (s_ctx.state == WS_STATE_IDLE) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Disconnecting...");
    change_state(WS_STATE_DISCONNECTING);
    
    stop_ping_timer();
    stop_reconnect_timer();
    
    s_ctx.ws_connected = false;
    
    /* TODO: Actually close WebSocket connection */
    
    change_state(WS_STATE_IDLE);
    
    /* Notify callback */
    if (s_ctx.callback) {
        s_ctx.callback(WS_EVENT_DISCONNECTED, NULL, 0, s_ctx.user_data);
    }
    
    ESP_LOGI(TAG, "Disconnected");
    return ESP_OK;
}

/**
 * Send data
 */
esp_err_t ws_client_send(const uint8_t *data, size_t len)
{
    if (!s_ctx.ws_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    
    /* TODO: Send via WebSocket */
    ESP_LOGI(TAG, "Sending %d bytes", len);
    
    return ESP_OK;
}

/**
 * Get current state
 */
ws_state_t ws_client_get_state(void)
{
    return s_ctx.state;
}

/**
 * Register event callback
 */
esp_err_t ws_client_register_callback(ws_event_callback_t callback, void *user_data)
{
    s_ctx.callback = callback;
    s_ctx.user_data = user_data;
    return ESP_OK;
}

/**
 * Check if connected
 */
bool ws_client_is_connected(void)
{
    return s_ctx.ws_connected;
}