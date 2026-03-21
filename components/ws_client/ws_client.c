/**
 * ESP001 WebSocket Client Implementation
 * 
 * Uses ESP-IDF's esp_http_client for WebSocket connections
 * WebSocket over HTTP uses the HTTP Upgrade mechanism
 */

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "ws_client.h"

static const char *TAG = "WS_CLIENT";

/* Default configuration */
#define DEFAULT_PING_INTERVAL_SEC      15
#define DEFAULT_PONG_TIMEOUT_SEC       5
#define DEFAULT_RECONNECT_INTERVAL_SEC  1
#define DEFAULT_MAX_RECONNECT_SEC      30

/* WebSocket opcodes */
#define WS_OP_CONTINUE   0x00
#define WS_OP_TEXT       0x01
#define WS_OP_BINARY     0x02
#define WS_OP_CLOSE      0x08
#define WS_OP_PING       0x09
#define WS_OP_PONG       0x0A

/* WebSocket frame header size */
#define WS_HEADER_SIZE   2
#define WS_MASK_BIT      0x80

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
    esp_http_client_handle_t client;
    bool handshake_done;
    uint8_t rx_buf[512];
    int rx_buf_len;
} ws_ctx_t;

static ws_ctx_t s_ctx = {0};

/* Timer handles */
static esp_timer_handle_t s_ping_timer = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;

/* WiFi initialization flag */
static bool s_wifi_initialized = false;

/* Forward declarations */
static void send_websocket_frame(esp_http_client_handle_t client, uint8_t opcode, 
                                 const uint8_t *data, size_t len);

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
    
    int delay_sec = s_ctx.config.reconnect_interval_sec * (1 << s_ctx.reconnect_count);
    if (delay_sec > s_ctx.config.max_reconnect_interval_sec) {
        delay_sec = s_ctx.config.max_reconnect_interval_sec;
    }
    
    ESP_LOGI(TAG, "Reconnecting in %d seconds (attempt %d)", delay_sec, s_ctx.reconnect_count + 1);
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
 * Ping timer callback
 */
static void ping_timer_callback(void *arg)
{
    (void)arg;
    if (s_ctx.ws_connected && s_ctx.client != NULL) {
        s_ctx.last_ping_time = esp_timer_get_time();
        ESP_LOGI(TAG, "Sending WebSocket ping...");
        send_websocket_frame(s_ctx.client, WS_OP_PING, NULL, 0);
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
 * WiFi event handler
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi started, connecting to %s", s_ctx.config.wifi_ssid);
                esp_wifi_connect();
                break;
                
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi connected to AP");
                /* Don't connect WebSocket yet - wait for IP address */
                break;
                
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI(TAG, "WiFi disconnected");
                s_ctx.wifi_connected = false;
                s_ctx.ws_connected = false;
                stop_ping_timer();
                
                if (s_ctx.state == WS_STATE_CONNECTED || 
                    s_ctx.state == WS_STATE_CONNECTING) {
                    esp_wifi_connect();
                    s_ctx.reconnect_count = 0;
                    start_reconnect_timer();
                }
                break;
                
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
            ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            s_ctx.wifi_connected = true;
            /* Now trigger WebSocket connection */
            if (s_ctx.state == WS_STATE_IDLE) {
                ws_client_connect();
            }
        }
    }
}

/**
 * Parse WebSocket frame
 * Returns: payload length, or -1 on error
 */
static int parse_websocket_frame(uint8_t *buf, size_t len, uint8_t *opcode, 
                                 uint8_t **payload, size_t *payload_len)
{
    if (len < 2) {
        return -1;
    }
    
    uint8_t first_byte = buf[0];
    uint8_t second_byte = buf[1];
    
    *opcode = first_byte & 0x0F;
    
    uint64_t msg_len = second_byte & 0x7F;
    size_t header_len = 2;
    
    if (msg_len == 126) {
        if (len < 4) return -1;
        msg_len = (buf[2] << 8) | buf[3];
        header_len = 4;
    } else if (msg_len == 127) {
        if (len < 10) return -1;
        msg_len = 0;
        for (int i = 0; i < 8; i++) {
            msg_len = (msg_len << 8) | buf[2 + i];
        }
        header_len = 10;
    }
    
    if (len < header_len + msg_len) {
        return -1;  /* Need more data */
    }
    
    *payload = &buf[header_len];
    *payload_len = msg_len;
    
    return msg_len;
}

/**
 * Send WebSocket frame
 */
static void send_websocket_frame(esp_http_client_handle_t client, uint8_t opcode, 
                                 const uint8_t *data, size_t len)
{
    if (client == NULL) return;
    
    /* Build frame header */
    uint8_t header[14];
    size_t header_len = 0;
    
    header[0] = 0x80 | opcode;  /* FIN + opcode */
    
    if (len < 126) {
        header[1] = len;
        header_len = 2;
    } else if (len < 65536) {
        header[1] = 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        header_len = 4;
    } else {
        header[1] = 127;
        /* 8-byte length */
        memset(&header[2], 0, 8);
        header[10] = (len >> 8) & 0xFF;
        header[11] = len & 0xFF;
        header_len = 12;
    }
    
    /* Write frame */
    uint8_t *frame = malloc(header_len + len);
    if (!frame) return;
    
    memcpy(frame, header, header_len);
    if (data && len > 0) {
        memcpy(frame + header_len, data, len);
    }
    
    /* Send via HTTP client (write any data triggers WebSocket mode) */
    esp_err_t err = esp_http_client_write(client, (const char *)frame, header_len + len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send WebSocket frame: %d", err);
    }
    
    free(frame);
}

/**
 * Handle WebSocket message
 */
static void handle_websocket_message(uint8_t opcode, const uint8_t *data, size_t len)
{
    switch (opcode) {
        case WS_OP_TEXT:
            ESP_LOGI(TAG, "WebSocket text message: %.*s", len, data);
            if (s_ctx.callback) {
                s_ctx.callback(WS_EVENT_DATA, data, len, s_ctx.user_data);
            }
            break;
            
        case WS_OP_BINARY:
            ESP_LOGI(TAG, "WebSocket binary message: %d bytes", len);
            if (s_ctx.callback) {
                s_ctx.callback(WS_EVENT_DATA, data, len, s_ctx.user_data);
            }
            break;
            
        case WS_OP_PING:
            ESP_LOGI(TAG, "WebSocket ping received");
            break;
            
        case WS_OP_PONG:
            ESP_LOGI(TAG, "WebSocket pong received");
            break;
            
        case WS_OP_CLOSE:
            ESP_LOGI(TAG, "WebSocket close received");
            s_ctx.ws_connected = false;
            if (s_ctx.callback) {
                s_ctx.callback(WS_EVENT_DISCONNECTED, NULL, 0, s_ctx.user_data);
            }
            break;
            
        default:
            ESP_LOGW(TAG, "Unknown WebSocket opcode: 0x%02X", opcode);
            break;
    }
}

/**
 * HTTP event handler (for WebSocket handshake and data)
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGI(TAG, "HTTP error");
            break;
            
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP connected, performing WebSocket handshake...");
            break;
            
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP headers sent");
            break;
            
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP header: %s: %s", evt->header_key, evt->header_value);
            break;
            
        case HTTP_EVENT_ON_DATA:
            if (s_ctx.handshake_done) {
                /* WebSocket data received */
                uint8_t opcode;
                uint8_t *payload;
                size_t payload_len;
                
                /* Copy data to buffer */
                if (s_ctx.rx_buf_len + evt->data_len < sizeof(s_ctx.rx_buf)) {
                    memcpy(s_ctx.rx_buf + s_ctx.rx_buf_len, evt->data, evt->data_len);
                    s_ctx.rx_buf_len += evt->data_len;
                    
                    /* Parse all complete frames */
                    while (s_ctx.rx_buf_len > 0) {
                        int msg_len = parse_websocket_frame(s_ctx.rx_buf, s_ctx.rx_buf_len,
                                                           &opcode, &payload, &payload_len);
                        if (msg_len < 0) break;
                        
                        handle_websocket_message(opcode, payload, payload_len);
                        
                        /* Remove processed frame from buffer */
                        size_t frame_len = (payload - s_ctx.rx_buf) + msg_len;
                        memmove(s_ctx.rx_buf, s_ctx.rx_buf + frame_len, 
                                s_ctx.rx_buf_len - frame_len);
                        s_ctx.rx_buf_len -= frame_len;
                    }
                }
            }
            break;
            
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP finished");
            if (s_ctx.handshake_done) {
                s_ctx.ws_connected = false;
                if (s_ctx.callback) {
                    s_ctx.callback(WS_EVENT_DISCONNECTED, NULL, 0, s_ctx.user_data);
                }
            }
            break;
            
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP disconnected");
            s_ctx.ws_connected = false;
            s_ctx.handshake_done = false;
            stop_ping_timer();
            break;
            
        default:
            break;
    }
    return ESP_OK;
}

/**
 * Build WebSocket upgrade request
 */
static void build_ws_upgrade_request(char *buf, size_t buf_len, const char *host, 
                                     const char *path, const char *ws_key)
{
    snprintf(buf, buf_len,
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "\r\n",
             path, host, ws_key);
}

/**
 * Generate WebSocket key
 */
static void generate_ws_key(char *key, size_t key_len)
{
    static const char *ws_magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char hash[20];
    char nonce[16];
    
    /* Generate random nonce */
    for (int i = 0; i < 16; i++) {
        nonce[i] = esp_random() & 0xFF;
    }
    
    /* Simple base64 encode of nonce */
    static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int j = 0;
    for (int i = 0; i < 16; i += 3) {
        key[j++] = b64_chars[(nonce[i] >> 2) & 0x3F];
        key[j++] = b64_chars[((nonce[i] & 0x03) << 4) | ((nonce[i+1] >> 4) & 0x0F)];
        key[j++] = b64_chars[((nonce[i+1] & 0x0F) << 2) | ((nonce[i+2] >> 6) & 0x03)];
        key[j++] = b64_chars[nonce[i+2] & 0x3F];
    }
    key[22] = '\0';
}

/**
 * Parse URL to get host and path
 */
static void parse_ws_url(const char *url, char *host, size_t host_len, 
                         char *path, size_t path_len, int *port)
{
    /* Format: ws://host:port/path or wss://host:port/path */
    const char *p = url;
    
    /* Skip protocol */
    if (strncmp(p, "ws://", 5) == 0) {
        p += 5;
        *port = 80;
    } else if (strncmp(p, "wss://", 6) == 0) {
        p += 6;
        *port = 443;
    } else {
        *port = 80;
    }
    
    /* Find path */
    const char *path_start = strchr(p, '/');
    if (path_start) {
        strncpy(path, path_start, path_len - 1);
        path[path_len - 1] = '\0';
        
        /* Extract host */
        strncpy(host, p, path_start - p);
        host[path_start - p] = '\0';
    } else {
        strcpy(path, "/");
        strncpy(host, p, host_len - 1);
        host[host_len - 1] = '\0';
    }
    
    /* Check for port in host */
    char *colon = strchr(host, ':');
    if (colon) {
        *port = atoi(colon + 1);
        *colon = '\0';
    }
}

/**
 * Initialize WiFi
 */
static esp_err_t init_wifi(void)
{
    /* Check if already initialized */
    if (s_wifi_initialized) {
        ESP_LOGI(TAG, "WiFi already initialized");
        return ESP_OK;
    }
    
    /* Check if WiFi is already initialized (from previous boot) */
    wifi_mode_t mode;
    esp_err_t ret = esp_wifi_get_mode(&mode);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi already initialized (mode=%d), skipping init", mode);
        s_wifi_initialized = true;
        return ESP_OK;
    }
    
    /* Initialize NVS - required for WiFi */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_TYPE_MISMATCH) {
        /* NVS partition was truncated, erase and reinitialize */
        ESP_LOGW(TAG, "NVS flash error, erasing...");
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS: %d", ret);
            return ret;
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash init failed: %d", ret);
        return ret;
    }
    
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Netif init failed: %d", ret);
        return ret;
    }
    
    /* Create default WiFi STA network interface (enables DHCP) */
    /* Check if already exists to avoid ESP_ERR_INVALID_STATE */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        esp_netif_create_default_wifi_sta();
    } else {
        ESP_LOGI(TAG, "WiFi STA netif already exists");
    }
    
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop create failed: %d", ret);
        return ret;
    }
    
    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                      &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi event handler register failed: %d", ret);
        return ret;
    }
    
    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, 
                                      &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IP event handler register failed: %d", ret);
        return ret;
    }
    
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, s_ctx.config.wifi_ssid, 
            sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, s_ctx.config.wifi_password, 
            sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %d", ret);
        return ret;
    }
    
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set mode failed: %d", ret);
        return ret;
    }
    
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set config failed: %d", ret);
        return ret;
    }
    
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %d", ret);
        return ret;
    }
    
    ESP_LOGI(TAG, "WiFi initialization complete");
    s_wifi_initialized = true;
    return ESP_OK;
}

/**
 * Initialize timers
 */
static esp_err_t init_timers(void)
{
    /* Only create timers if they don't exist */
    if (s_ping_timer == NULL) {
        const esp_timer_create_args_t ping_args = {
            .callback = ping_timer_callback,
            .arg = NULL,
            .name = "ping_timer"
        };
        esp_err_t ret = esp_timer_create(&ping_args, &s_ping_timer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create ping timer: %d", ret);
            return ret;
        }
    }
    
    if (s_reconnect_timer == NULL) {
        const esp_timer_create_args_t reconnect_args = {
            .callback = reconnect_timer_callback,
            .arg = NULL,
            .name = "reconnect_timer"
        };
        esp_err_t ret = esp_timer_create(&reconnect_args, &s_reconnect_timer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create reconnect timer: %d", ret);
            return ret;
        }
    }
    
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
    
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.config.ping_interval_sec = DEFAULT_PING_INTERVAL_SEC;
    s_ctx.config.pong_timeout_sec = DEFAULT_PONG_TIMEOUT_SEC;
    s_ctx.config.reconnect_interval_sec = DEFAULT_RECONNECT_INTERVAL_SEC;
    s_ctx.config.max_reconnect_interval_sec = DEFAULT_MAX_RECONNECT_SEC;
    
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
    
    if (s_ctx.client) {
        esp_http_client_close(s_ctx.client);
        esp_http_client_cleanup(s_ctx.client);
        s_ctx.client = NULL;
    }
    
    if (s_ctx.state != WS_STATE_IDLE) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_deinit();
    }
    
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
        
        /* Initialize WiFi if not started */
        if (s_ctx.state == WS_STATE_IDLE) {
            init_wifi();
        }
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_ctx.state == WS_STATE_CONNECTED || 
        s_ctx.state == WS_STATE_CONNECTING) {
        ESP_LOGW(TAG, "Already connecting or connected");
        return ESP_OK;
    }
    
    if (strlen(s_ctx.config.url) == 0) {
        ESP_LOGE(TAG, "WebSocket URL not set");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Connecting to WebSocket server: %s", s_ctx.config.url);
    change_state(WS_STATE_CONNECTING);
    
    /* Parse URL */
    char host[128];
    char path[256];
    int port;
    parse_ws_url(s_ctx.config.url, host, sizeof(host), path, sizeof(path), &port);
    
    ESP_LOGI(TAG, "Host: %s, Port: %d, Path: %s", host, port, path);
    
    /* Close existing client */
    if (s_ctx.client) {
        esp_http_client_close(s_ctx.client);
        esp_http_client_cleanup(s_ctx.client);
        s_ctx.client = NULL;
    }
    
    /* Generate WebSocket key */
    char ws_key[32];
    generate_ws_key(ws_key, sizeof(ws_key));
    
    /* Build upgrade request */
    char request[512];
    build_ws_upgrade_request(request, sizeof(request), host, path, ws_key);
    
    /* Configure HTTP client */
    esp_http_client_config_t cfg = {
        .url = s_ctx.config.url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data = NULL,
        .buffer_size = 1024,
        .timeout_ms = 5000,
    };
    
    s_ctx.client = esp_http_client_init(&cfg);
    if (!s_ctx.client) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        change_state(WS_STATE_ERROR);
        return ESP_FAIL;
    }
    
    /* Set headers for WebSocket upgrade */
    esp_http_client_set_header(s_ctx.client, "Upgrade", "websocket");
    esp_http_client_set_header(s_ctx.client, "Connection", "Upgrade");
    esp_http_client_set_header(s_ctx.client, "Sec-WebSocket-Key", ws_key);
    esp_http_client_set_header(s_ctx.client, "Sec-WebSocket-Version", "13");
    esp_http_client_set_header(s_ctx.client, "Sec-WebSocket-Protocol", "chat");
    
    /* Perform HTTP request (this triggers the WebSocket handshake) */
    esp_err_t err = esp_http_client_open(s_ctx.client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %d", err);
        esp_http_client_close(s_ctx.client);
        esp_http_client_cleanup(s_ctx.client);
        s_ctx.client = NULL;
        change_state(WS_STATE_ERROR);
        return err;
    }
    
    /* Read response (handshake) */
    int status = esp_http_client_get_status_code(s_ctx.client);
    ESP_LOGI(TAG, "HTTP status: %d", status);
    
    if (status == 101) {
        /* Upgrade successful */
        s_ctx.handshake_done = true;
        s_ctx.ws_connected = true;
        s_ctx.reconnect_count = 0;
        change_state(WS_STATE_CONNECTED);
        start_ping_timer();
        
        if (s_ctx.callback) {
            s_ctx.callback(WS_EVENT_CONNECTED, NULL, 0, s_ctx.user_data);
        }
        
        ESP_LOGI(TAG, "WebSocket connected!");
    } else {
        ESP_LOGE(TAG, "WebSocket handshake failed, status: %d", status);
        esp_http_client_close(s_ctx.client);
        esp_http_client_cleanup(s_ctx.client);
        s_ctx.client = NULL;
        change_state(WS_STATE_ERROR);
        return ESP_FAIL;
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
    
    if (s_ctx.ws_connected && s_ctx.client) {
        /* Send close frame */
        send_websocket_frame(s_ctx.client, WS_OP_CLOSE, NULL, 0);
    }
    
    s_ctx.ws_connected = false;
    s_ctx.handshake_done = false;
    
    if (s_ctx.client) {
        esp_http_client_close(s_ctx.client);
        esp_http_client_cleanup(s_ctx.client);
        s_ctx.client = NULL;
    }
    
    change_state(WS_STATE_IDLE);
    
    if (s_ctx.callback) {
        s_ctx.callback(WS_EVENT_DISCONNECTED, NULL, 0, s_ctx.user_data);
    }
    
    ESP_LOGI(TAG, "Disconnected");
    return ESP_OK;
}

/**
 * Send data to WebSocket server
 */
esp_err_t ws_client_send(const uint8_t *data, size_t len)
{
    if (!s_ctx.ws_connected || !s_ctx.client) {
        return ESP_ERR_INVALID_STATE;
    }
    
    send_websocket_frame(s_ctx.client, WS_OP_TEXT, data, len);
    ESP_LOGI(TAG, "Sent %d bytes", len);
    
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