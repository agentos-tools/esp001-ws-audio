/**
 * ESP001 WebSocket Client Implementation
 * 
 * Uses ESP-IDF's esp_http_client for WebSocket connections
 * WebSocket over HTTP uses the HTTP Upgrade mechanism
 */

#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
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

/* WiFi EventGroup bits */
#define WIFI_CONNECTED_BIT  (1 << 0)
#define WIFI_FAIL_BIT       (1 << 1)

/* WiFi NVS namespace */
#define WIFI_NVS_NAMESPACE  "wifi_cfg"

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
    int socket_fd;  /* Raw socket fd for WebSocket connection */
    bool handshake_done;
    uint8_t rx_buf[512];
    int rx_buf_len;
} ws_ctx_t;

static ws_ctx_t s_ctx = {0};

/* Timer handles */
static esp_timer_handle_t s_ping_timer = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;

/* Receive task handle */
static TaskHandle_t s_recv_task_handle = NULL;

/* WiFi initialization flag */
static bool s_wifi_initialized = false;

/* WiFi EventGroup for synchronization */
static EventGroupHandle_t s_wifi_event_group;

/* NVS WiFi credentials storage */
static bool s_wifi_credentials_loaded = false;

/* Connection guard flag (prevents re-entrant ws_client_connect calls) */
static volatile bool s_connecting = false;

/* Forward declarations */
static void send_websocket_frame(int sock, uint8_t opcode, 
                                 const uint8_t *data, size_t len);
static void recv_task(void *arg);

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
    if (s_ctx.ws_connected && s_ctx.socket_fd >= 0) {
        s_ctx.last_ping_time = esp_timer_get_time();
        ESP_LOGI(TAG, "Sending WebSocket ping...");
        send_websocket_frame(s_ctx.socket_fd, WS_OP_PING, NULL, 0);
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
 * Save WiFi credentials to NVS
 */
static esp_err_t wifi_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK && password) {
        err = nvs_set_str(handle, "password", password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/**
 * Load WiFi credentials from NVS
 */
static esp_err_t wifi_load_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t len = ssid_len;
    err = nvs_get_str(handle, "ssid", ssid, &len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    len = pass_len;
    err = nvs_get_str(handle, "password", password, &len);
    nvs_close(handle);
    return err;
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
                ESP_LOGI(TAG, "WiFi STA started");
                esp_wifi_connect();
                break;
                
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi connected to AP");
                break;
                
            case WIFI_EVENT_STA_DISCONNECTED:
                s_ctx.wifi_connected = false;
                s_ctx.ws_connected = false;
                stop_ping_timer();
                xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
                ESP_LOGW(TAG, "WiFi disconnected, retrying...");
                esp_wifi_connect();
                break;
                
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_ctx.wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        /* Now trigger WebSocket connection */
        if (s_ctx.state == WS_STATE_IDLE) {
            ws_client_connect();
        }
    }
}

/**
 * Set WiFi credentials and connect (non-blocking)
 * This stops existing WiFi, sets new credentials, saves to NVS, and starts connection
 */
static esp_err_t wifi_manager_set_and_connect(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID cannot be empty");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "WiFi set_and_connect: %s", ssid);

    /* Stop existing WiFi before reconfiguring */
    esp_wifi_stop();

    /* Configure WiFi */
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Save credentials to NVS */
    if (wifi_save_credentials(ssid, password) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials saved to NVS");
    }

    /* Update context */
    strncpy(s_ctx.config.wifi_ssid, ssid, sizeof(s_ctx.config.wifi_ssid) - 1);
    strncpy(s_ctx.config.wifi_password, password, sizeof(s_ctx.config.wifi_password) - 1);

    ESP_LOGI(TAG, "WiFi connecting to SSID: %s", ssid);
    return ESP_OK;
}

/**
 * Wait for WiFi connection with timeout
 * Returns: true if connected, false if timeout
 */
static bool wifi_manager_wait_connected(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
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
static void send_websocket_frame(int sock, uint8_t opcode, 
                                 const uint8_t *data, size_t len)
{
    if (sock < 0) return;
    
    /* Generate mask key (client MUST mask frames) */
    uint32_t mask_key = esp_random();
    
    /* Build frame header */
    uint8_t header[14];
    size_t header_len = 0;
    
    header[0] = 0x80 | opcode;  /* FIN + opcode */
    
    if (len < 126) {
        header[1] = 0x80 | len;  /* Mask bit + payload length */
        header[2] = (mask_key >> 24) & 0xFF;
        header[3] = (mask_key >> 16) & 0xFF;
        header[4] = (mask_key >> 8) & 0xFF;
        header[5] = mask_key & 0xFF;
        header_len = 6;
    } else if (len < 65536) {
        header[1] = 0x80 | 126;  /* Mask bit + 2-byte extended length */
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        header[4] = (mask_key >> 24) & 0xFF;
        header[5] = (mask_key >> 16) & 0xFF;
        header[6] = (mask_key >> 8) & 0xFF;
        header[7] = mask_key & 0xFF;
        header_len = 8;
    } else {
        header[1] = 0x80 | 127;  /* Mask bit + 8-byte extended length */
        /* 64-bit length in big endian */
        header[2] = 0;
        header[3] = 0;
        header[4] = 0;
        header[5] = 0;
        header[6] = (len >> 24) & 0xFF;
        header[7] = (len >> 16) & 0xFF;
        header[8] = (len >> 8) & 0xFF;
        header[9] = len & 0xFF;
        header[10] = (mask_key >> 24) & 0xFF;
        header[11] = (mask_key >> 16) & 0xFF;
        header[12] = (mask_key >> 8) & 0xFF;
        header[13] = mask_key & 0xFF;
        header_len = 14;
    }
    
    /* Write frame */
    uint8_t *frame = malloc(header_len + len);
    if (!frame) return;
    
    memcpy(frame, header, header_len);
    if (data && len > 0) {
        /* Mask the data */
        for (size_t i = 0; i < len; i++) {
            frame[header_len + i] = data[i] ^ ((uint8_t *)&mask_key)[i % 4];
        }
    }
    
    /* Send via socket */
    ssize_t sent = send(sock, frame, header_len + len, 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send WebSocket frame: errno=%d (%s)", errno, strerror(errno));
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
    (void)key_len;
    char nonce[16];
    
    /* Generate random 16-byte nonce */
    for (int i = 0; i < 16; i++) {
        nonce[i] = esp_random() & 0xFF;
    }
    
    /* Base64 encode: 16 bytes → 24 characters (with == padding) */
    static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int j = 0;
    
    /* Process 15 bytes in groups of 3 (5 groups × 4 chars = 20 chars) */
    for (int i = 0; i < 15; i += 3) {
        key[j++] = b64_chars[(nonce[i] >> 2) & 0x3F];
        key[j++] = b64_chars[((nonce[i] & 0x03) << 4) | ((nonce[i+1] >> 4) & 0x0F)];
        key[j++] = b64_chars[((nonce[i+1] & 0x0F) << 2) | ((nonce[i+2] >> 6) & 0x03)];
        key[j++] = b64_chars[nonce[i+2] & 0x3F];
    }
    
    /* Process remaining 1 byte with padding (4 chars: 2 data + 2 padding) */
    key[j++] = b64_chars[(nonce[15] >> 2) & 0x3F];
    key[j++] = b64_chars[((nonce[15] & 0x03) << 4) & 0x3F];
    key[j++] = '=';
    key[j++] = '=';
    key[j] = '\0';
}

/**
 * Parse URL to get host and path
 */
static void parse_ws_url(const char *url, char *host, size_t host_len, 
                         char *path, size_t path_len, int *port)
{
    /* Format: ws://host:port/path, wss://host:port/path, http://host:port/path, https://host:port/path */
    const char *p = url;
    
    /* Skip protocol */
    if (strncmp(p, "ws://", 5) == 0) {
        p += 5;
        *port = 80;
    } else if (strncmp(p, "wss://", 6) == 0) {
        p += 6;
        *port = 443;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        *port = 80;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
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
    
    ESP_LOGI(TAG, "init_wifi: starting...");

    /* Create EventGroup for WiFi sync */
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
    }
    
    /* Initialize NVS */
    ESP_LOGI(TAG, "init_wifi: initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_TYPE_MISMATCH) {
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
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* Try to load saved credentials from NVS */
    char saved_ssid[32] = {0};
    char saved_password[64] = {0};
    if (wifi_load_credentials(saved_ssid, sizeof(saved_ssid), saved_password, sizeof(saved_password)) == ESP_OK 
        && strlen(saved_ssid) > 0) {
        ESP_LOGI(TAG, "init_wifi: loading saved SSID: %s", saved_ssid);
        strncpy(s_ctx.config.wifi_ssid, saved_ssid, sizeof(s_ctx.config.wifi_ssid) - 1);
        strncpy(s_ctx.config.wifi_password, saved_password, sizeof(s_ctx.config.wifi_password) - 1);
        s_wifi_credentials_loaded = true;
        
        /* Use saved credentials to connect */
        wifi_config_t wifi_config = {
            .sta = {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
                .pmf_cfg = {
                    .capable = true,
                    .required = false,
                },
            },
        };
        strncpy((char *)wifi_config.sta.ssid, saved_ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, saved_password, sizeof(wifi_config.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "WiFi initialized with saved SSID: %s", saved_ssid);
    } else if (strlen(s_ctx.config.wifi_ssid) > 0) {
        /* Use configured credentials */
        wifi_config_t wifi_config = {
            .sta = {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
                .pmf_cfg = {
                    .capable = true,
                    .required = false,
                },
            },
        };
        strncpy((char *)wifi_config.sta.ssid, s_ctx.config.wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, s_ctx.config.wifi_password, sizeof(wifi_config.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "WiFi initialized with configured SSID: %s", s_ctx.config.wifi_ssid);
    } else {
        ESP_LOGI(TAG, "WiFi initialized (no SSID configured, use ws_client_set_wifi to set credentials)");
    }

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
 * Initialize WiFi (exposed for scan before connect)
 */
esp_err_t ws_client_init_wifi(void)
{
    return init_wifi();
}

/**
 * Deinitialize WebSocket client
 */
esp_err_t ws_client_deinit(void)
{
    stop_ping_timer();
    stop_reconnect_timer();
    
    /* Stop receive task */
    if (s_recv_task_handle != NULL) {
        vTaskDelete(s_recv_task_handle);
        s_recv_task_handle = NULL;
    }
    
    if (s_ping_timer) {
        esp_timer_delete(s_ping_timer);
        s_ping_timer = NULL;
    }
    
    if (s_reconnect_timer) {
        esp_timer_delete(s_reconnect_timer);
        s_reconnect_timer = NULL;
    }
    
    if (s_ctx.socket_fd >= 0) {
        close(s_ctx.socket_fd);
        s_ctx.socket_fd = -1;
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
 * WebSocket receive task - continuously reads and parses WebSocket frames
 */
static void recv_task(void *arg)
{
    (void)arg;
    uint8_t buf[4096];  /* Increased buffer size */
    
    ESP_LOGI(TAG, ">>> recv_task started, socket_fd=%d, ws_connected=%d", 
             s_ctx.socket_fd, s_ctx.ws_connected);
    
    // Heartbeat: print alive message every 5 seconds
    int loop_count = 0;
    
    while (s_ctx.ws_connected && s_ctx.socket_fd >= 0) {
        loop_count++;
        if (loop_count % 25 == 0) {  // Every ~25 iterations at ~100ms each = ~2.5s
            ESP_LOGI(TAG, "    recv_task alive: loop=%d, socket_fd=%d, ws_connected=%d", 
                     loop_count, s_ctx.socket_fd, s_ctx.ws_connected);
        }
        
        // Read WebSocket frame header (2 bytes minimum)
        int header_len = 2;
        int n = recv(s_ctx.socket_fd, buf, header_len, 0);
        
        if (n <= 0) {
            if (n < 0) {
                int err = errno;
                if (err == EAGAIN || err == EWOULDBLOCK) {
                    /* Socket timeout - no data available, continue waiting */
                    continue;
                }
                ESP_LOGW(TAG, "recv error: %d, errno=%d (%s)", n, err, strerror(err));
            } else {
                ESP_LOGI(TAG, "Connection closed by server (recv returned 0)");
            }
            break;
        }
        
        if (n < 2) {
            ESP_LOGW(TAG, "Incomplete header read: %d bytes", n);
            continue;
        }
        
        // Parse WebSocket frame header
        uint8_t opcode = buf[0] & 0x0F;
        uint8_t mask_bit = (buf[1] & 0x80) >> 7;
        uint64_t payload_len = buf[1] & 0x7F;
        
        ESP_LOGI(TAG, "    Frame: opcode=0x%02x, mask=%d, len=%llu", opcode, mask_bit, payload_len);
        
        int header_bytes = 2;
        
        // Extended payload length (if payload_len is 126 or 127)
        if (payload_len == 126) {
            int n = recv(s_ctx.socket_fd, buf + 2, 2, 0);
            if (n < 2) {
                ESP_LOGW(TAG, "Failed to read extended length");
                continue;
            }
            payload_len = (buf[2] << 8) | buf[3];
            header_bytes += 2;
        } else if (payload_len == 127) {
            int n = recv(s_ctx.socket_fd, buf + 2, 8, 0);
            if (n < 8) {
                ESP_LOGW(TAG, "Failed to read extended length");
                continue;
            }
            payload_len = 0;
            for (int i = 0; i < 8; i++) {
                payload_len = (payload_len << 8) | buf[2 + i];
            }
            header_bytes += 8;
        }
        
        // Read mask key (if present)
        uint8_t mask_key[4] = {0};
        if (mask_bit) {
            int n = recv(s_ctx.socket_fd, mask_key, 4, 0);
            if (n < 4) {
                ESP_LOGW(TAG, "Failed to read mask key");
                continue;
            }
            header_bytes += 4;
        }
        
        // Limit payload length to prevent buffer overflow
        if (payload_len > sizeof(buf) - header_bytes) {
            ESP_LOGW(TAG, "Payload too large: %llu", payload_len);
            // Consume the payload anyway to keep connection valid
            uint64_t consumed = 0;
            while (consumed < payload_len) {
                int to_read = (payload_len - consumed) > sizeof(buf) ? sizeof(buf) : (payload_len - consumed);
                int n = recv(s_ctx.socket_fd, buf, to_read, 0);
                if (n <= 0) break;
                consumed += n;
            }
            continue;
        }
        
        // Read payload
        uint8_t *payload = buf + header_bytes;
        uint64_t remaining = payload_len;
        while (remaining > 0) {
            int n = recv(s_ctx.socket_fd, payload + (payload_len - remaining), remaining, 0);
            if (n <= 0) {
                ESP_LOGW(TAG, "recv failed during payload read");
                break;
            }
            remaining -= n;
        }
        
        if (remaining > 0) {
            ESP_LOGW(TAG, "Incomplete payload read");
            continue;
        }
        
        // Unmask if necessary
        if (mask_bit) {
            for (uint64_t i = 0; i < payload_len; i++) {
                payload[i] ^= mask_key[i % 4];
            }
        }
        
        // Handle frame
        switch (opcode) {
            case 0x01: // Text frame
            case 0x02: // Binary frame
                ESP_LOGI(TAG, "Received %s frame: %llu bytes", 
                         opcode == 0x01 ? "text" : "binary", payload_len);
                if (s_ctx.callback) {
                    s_ctx.callback(WS_EVENT_DATA, payload, payload_len, s_ctx.user_data);
                }
                break;
                
            case 0x08: // Close frame
                ESP_LOGI(TAG, "Received close frame");
                if (s_ctx.callback) {
                    s_ctx.callback(WS_EVENT_DISCONNECTED, NULL, 0, s_ctx.user_data);
                }
                goto exit_loop;
                
            case 0x09: // Ping frame
                ESP_LOGI(TAG, ">>> PING received, sending PONG");
                send_websocket_frame(s_ctx.socket_fd, 0x0A, NULL, 0);  // Pong with no payload
                ESP_LOGI(TAG, ">>> PONG sent");
                
            case 0x0A: // Pong frame
                ESP_LOGI(TAG, "Received pong");
                break;
                
            default:
                ESP_LOGW(TAG, "Unknown opcode: 0x%02x", opcode);
                break;
        }
    }
    
exit_loop:
    ESP_LOGI(TAG, "Receive task exiting");
    s_recv_task_handle = NULL;
    vTaskDelete(NULL);
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
    
    /* If WiFi is already initialized, trigger reconnection with new credentials */
    if (s_wifi_initialized) {
        ESP_LOGI(TAG, "WiFi already initialized, reconnecting with new credentials...");
        wifi_manager_set_and_connect(ssid, password);
    }
    
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
    /* Guard against re-entrant calls */
    if (s_connecting) {
        ESP_LOGW(TAG, "ws_client_connect already in progress, skipping");
        return ESP_ERR_INVALID_STATE;
    }
    s_connecting = true;
    
    /* Initialize WiFi if not started */
    if (s_ctx.state == WS_STATE_IDLE) {
        ESP_LOGI(TAG, "ws_client_connect: initializing WiFi...");
        init_wifi();
    }
    
    /* Wait for WiFi connection with timeout (use saved credentials if no new ones set) */
    if (!s_wifi_credentials_loaded && strlen(s_ctx.config.wifi_ssid) == 0) {
        ESP_LOGW(TAG, "No WiFi credentials configured");
        s_connecting = false;
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Wait for WiFi to connect (max 30 seconds) */
    if (!s_ctx.wifi_connected) {
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        bool connected = wifi_manager_wait_connected(30000);
        if (!connected) {
            ESP_LOGW(TAG, "WiFi connection timed out");
            s_connecting = false;
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGI(TAG, "WiFi connected!");
    }
    
    /* Guard against re-entrant calls (e.g., from GOT_IP event while already connecting) */
    if (s_ctx.state == WS_STATE_CONNECTED || 
        s_ctx.state == WS_STATE_CONNECTING) {
        ESP_LOGW(TAG, "Already connecting or connected, skipping");
        s_connecting = false;
        return ESP_OK;
    }
    
    if (strlen(s_ctx.config.url) == 0) {
        ESP_LOGE(TAG, "WebSocket URL not set");
        s_connecting = false;
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Connecting to WebSocket server: %s", s_ctx.config.url);
    change_state(WS_STATE_CONNECTING);
    
    /* Use raw socket for WebSocket handshake (more reliable than esp_http_client) */
    char host[128];
    char path[256];
    int port;
    parse_ws_url(s_ctx.config.url, host, sizeof(host), path, sizeof(path), &port);
    
    ESP_LOGI(TAG, "Socket connecting to %s:%d%s", host, port, path);
    
    /* Verify WiFi is still connected before attempting socket connection */
    if (!s_ctx.wifi_connected) {
        ESP_LOGW(TAG, "WiFi not connected, skipping WebSocket connect");
        s_connecting = false;
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Close existing socket if any */
    if (s_ctx.socket_fd >= 0) {
        close(s_ctx.socket_fd);
        s_ctx.socket_fd = -1;
    }
    
    /* Create socket */
    int sock = socket(PF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        change_state(WS_STATE_ERROR);
        s_connecting = false;
        return ESP_FAIL;
    }
    
    /* Set socket timeout */
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    /* Resolve host */
    struct hostent *he = gethostbyname(host);
    if (!he) {
        ESP_LOGE(TAG, "Failed to resolve host: %s", host);
        close(sock);
        change_state(WS_STATE_ERROR);
        s_connecting = false;
        return ESP_FAIL;
    }
    
    /* Connect to server */
    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    memcpy(&dest_addr.sin_addr, he->h_addr_list[0], he->h_length);
    
    int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        int sock_err = 0;
        socklen_t len = sizeof(sock_err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &sock_err, &len);
        ESP_LOGE(TAG, "Socket connect failed: ret=%d errno=%d (%s)", err, sock_err, strerror(sock_err));
        close(sock);
        change_state(WS_STATE_ERROR);
        s_connecting = false;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Socket connected, sending WebSocket upgrade...");
    
    /* Generate WebSocket key */
    char ws_key[32];
    generate_ws_key(ws_key, sizeof(ws_key));
    
    /* Build and send upgrade request */
    char request[512];
    build_ws_upgrade_request(request, sizeof(request), host, path, ws_key);
    
    err = send(sock, request, strlen(request), 0);
    if (err < 0) {
        ESP_LOGE(TAG, "Socket send failed: %d", err);
        close(sock);
        change_state(WS_STATE_ERROR);
        s_connecting = false;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "WebSocket upgrade request sent");
    
    /* Read response */
    char response[512];
    int len = recv(sock, response, sizeof(response) - 1, 0);
    if (len <= 0) {
        ESP_LOGE(TAG, "Socket recv failed or timeout");
        close(sock);
        change_state(WS_STATE_ERROR);
        s_connecting = false;
        return ESP_FAIL;
    }
    response[len] = '\0';
    
    ESP_LOGI(TAG, "Socket response: %s", response);
    
    /* Check for 101 Switching Protocols */
    if (strstr(response, "HTTP/1.1 101") || strstr(response, "HTTP/1.0 101")) {
        ESP_LOGI(TAG, "WebSocket handshake successful!");
        s_ctx.handshake_done = true;
        s_ctx.ws_connected = true;
        s_ctx.reconnect_count = 0;
        s_ctx.socket_fd = sock;
        change_state(WS_STATE_CONNECTED);
        start_ping_timer();
        
        /* Start receive task */
        if (s_recv_task_handle == NULL) {
            BaseType_t ret = xTaskCreate(recv_task, "ws_recv", 8192, NULL, 5, &s_recv_task_handle);
            ESP_LOGI(TAG, "recv_task created: ret=%d, handle=%p", ret, s_recv_task_handle);
        } else {
            ESP_LOGW(TAG, "recv_task already exists, skipping creation");
        }
        
        if (s_ctx.callback) {
            s_ctx.callback(WS_EVENT_CONNECTED, NULL, 0, s_ctx.user_data);
        }
        
        ESP_LOGI(TAG, "WebSocket connected!");
    } else {
        ESP_LOGE(TAG, "WebSocket handshake failed. Response: %s", response);
        close(sock);
        change_state(WS_STATE_ERROR);
        s_connecting = false;
        return ESP_FAIL;
    }
    
    log_state("Connected");
    s_connecting = false;
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
    
    /* Stop receive task */
    if (s_recv_task_handle != NULL) {
        vTaskDelete(s_recv_task_handle);
        s_recv_task_handle = NULL;
    }
    
    if (s_ctx.ws_connected && s_ctx.socket_fd >= 0) {
        /* Send close frame */
        send_websocket_frame(s_ctx.socket_fd, WS_OP_CLOSE, NULL, 0);
    }
    
    s_ctx.ws_connected = false;
    s_ctx.handshake_done = false;
    
    if (s_ctx.socket_fd >= 0) {
        close(s_ctx.socket_fd);
        s_ctx.socket_fd = -1;
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
    if (!s_ctx.ws_connected || s_ctx.socket_fd < 0) {
        ESP_LOGW(TAG, "ws_client_send: not connected (ws_connected=%d, socket_fd=%d)", 
                  s_ctx.ws_connected, s_ctx.socket_fd);
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Check if socket is still valid by attempting a non-blocking recv */
    int saved_errno;
    int n = recv(s_ctx.socket_fd, NULL, 0, MSG_PEEK | MSG_DONTWAIT);
    saved_errno = errno;
    
    if (n < 0 && saved_errno != EAGAIN && saved_errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "ws_client_send: socket closed (n=%d, errno=%d)", n, saved_errno);
        /* Socket is dead - trigger disconnect */
        s_ctx.ws_connected = false;
        close(s_ctx.socket_fd);
        s_ctx.socket_fd = -1;
        change_state(WS_STATE_ERROR);
        return ESP_ERR_INVALID_STATE;
    }
    
    send_websocket_frame(s_ctx.socket_fd, WS_OP_BINARY, data, len);
    ESP_LOGI(TAG, "Sent %d bytes (binary)", len);
    
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
    bool result = s_ctx.state == WS_STATE_CONNECTED && s_ctx.socket_fd >= 0 && s_ctx.ws_connected;
    ESP_LOGI(TAG, "is_connected check: state=%d(need=%d) socket_fd=%d(need>=0) ws_connected=%d(need=1) => result=%d",
             s_ctx.state, WS_STATE_CONNECTED, s_ctx.socket_fd, s_ctx.ws_connected, result);
    return result;
}

/**
 * Check if WiFi is connected
 */
bool ws_client_is_wifi_connected(void)
{
    return s_ctx.wifi_connected;
}