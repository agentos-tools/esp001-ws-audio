/**
 * ESP001 - Audio + WebSocket Test
 * 
 * Phase 2: Audio streaming over WebSocket
 * - Audio driver + sine wave loopback test
 * - WiFi connection (non-blocking, with timeout)
 * - WebSocket audio streaming
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "driver/uart.h"
#include "audio_driver.h"
#include "esp_timer.h"
#include "ws_client.h"

static const char *TAG = "ESP001";

/* Task handle */
static TaskHandle_t audio_task_handle = NULL;
static TaskHandle_t wifi_task_handle = NULL;

/* Audio buffer */
#define AUDIO_BUFFER_SIZE   1024
static int16_t audio_buffer[AUDIO_BUFFER_SIZE];

/* UART config */
#define USB_UART_NUM       UART_NUM_0
#define USB_UART_BUF_SIZE  512
#define USB_UART_BAUD      115200

/* Flag for audio streaming */
static volatile bool audio_streaming = true;

/* WebSocket configuration */
#define WIFI_SSID       "__abc2__"
#define WIFI_PASSWORD   "12345678"
#define WS_SERVER_URL   "ws://192.168.1.100:8080"

/* Connection state */
static volatile bool ws_connected = false;

/* WiFi connection timeout (10 seconds) */
#define WIFI_CONNECT_TIMEOUT_MS  10000

/**
 * Generate sine wave for testing
 */
static void generate_sine_wave(int16_t *buffer, size_t samples, int frequency, int sample_rate)
{
    for (size_t i = 0; i < samples; i++) {
        double t = (double)i / sample_rate;
        double value = sin(2.0 * M_PI * frequency * t);
        buffer[i] = (int16_t)(value * 32767 * 0.5);  /* 50% volume */
    }
}

/**
 * WebSocket event callback
 */
static void ws_event_callback(ws_event_t event, const uint8_t *data, size_t len, void *user_data)
{
    (void)user_data;
    
    switch (event) {
        case WS_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected!");
            ws_connected = true;
            break;
        case WS_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WebSocket disconnected");
            ws_connected = false;
            break;
        case WS_EVENT_DATA:
            ESP_LOGI(TAG, "WebSocket data: %d bytes", len);
            break;
        case WS_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            ws_connected = false;
            break;
    }
}

/**
 * UART event task (for future command interface)
 */
static void uart_event_task(void *pvParameters)
{
    uint8_t data[128];
    
    ESP_LOGI(TAG, "UART event task started");
    
    while (1) {
        int len = uart_read_bytes(USB_UART_NUM, data, sizeof(data), pdMS_TO_TICKS(10));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                putchar(data[i]);  // Echo
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * WiFi initialization task (non-blocking with timeout)
 */
static void wifi_task(void *pvParameters)
{
    (void)pvParameters;
    
    ESP_LOGI(TAG, "WiFi task started, connecting to %s...", WIFI_SSID);
    
    /* Initialize WebSocket client first */
    esp_err_t ret = ws_client_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ws_client_init failed: %s", esp_err_to_name(ret));
        ws_connected = false;
        vTaskDelete(NULL);
        return;
    }
    
    /* Configure WiFi and WebSocket */
    ws_client_set_wifi(WIFI_SSID, WIFI_PASSWORD);
    ws_client_set_url(WS_SERVER_URL);
    ws_client_register_callback(ws_event_callback, NULL);
    
    /* Record start time for timeout */
    int64_t start_time = esp_timer_get_time() / 1000;  /* ms */
    
    /* Try to connect - non-blocking, will connect in background */
    ESP_LOGI(TAG, "Initiating WiFi connection (timeout: %d ms)...", WIFI_CONNECT_TIMEOUT_MS);
    ESP_LOGI(TAG, "Calling ws_client_connect()...");
    ret = ws_client_connect();
    ESP_LOGI(TAG, "ws_client_connect() returned: %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ws_client_connect returned: %s", esp_err_to_name(ret));
        /* Not a fatal error - WiFi will reconnect in background */
    }
    
    /* Wait for WiFi connection or timeout */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        
        if (ws_client_is_wifi_connected()) {
            ESP_LOGI(TAG, "WiFi connected successfully!");
            break;
        }
        
        int64_t elapsed = (esp_timer_get_time() / 1000) - start_time;
        if (elapsed >= WIFI_CONNECT_TIMEOUT_MS) {
            ESP_LOGW(TAG, "WiFi connection timeout (%d ms elapsed)", (int)elapsed);
            ESP_LOGI(TAG, "Continuing without WiFi - audio mode");
            break;
        }
        
        /* Log progress every 2 seconds */
        if (((int)elapsed) % 2000 < 500) {
            ESP_LOGI(TAG, "WiFi connecting... %d ms / %d ms", (int)elapsed, WIFI_CONNECT_TIMEOUT_MS);
        }
    }
    
    ws_connected = ws_client_is_connected();
    ESP_LOGI(TAG, "WiFi task done. WiFi: %s, WS: %s",
             ws_client_is_wifi_connected() ? "connected" : "disconnected",
             ws_connected ? "connected" : "disconnected");
    
    vTaskDelete(NULL);
}

/**
 * Audio processing task
 * Handles I2S audio capture and streaming to WebSocket
 */
static void audio_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Audio task started");
    
    /* Initialize audio driver */
    esp_err_t ret = audio_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize audio driver: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Audio driver initialized");
    
    /* Generate test tone (1kHz sine wave) */
    generate_sine_wave(audio_buffer, AUDIO_BUFFER_SIZE, 1000, AUDIO_SAMPLE_RATE);
    ESP_LOGI(TAG, "Generated 1kHz sine wave test buffer");
    
    size_t bytes_read = 0;
    int16_t rx_buffer[AUDIO_BUFFER_SIZE];
    uint32_t loop_count = 0;
    bool audio_started = false;
    
    while (1) {
        if (audio_streaming) {
            if (!audio_started) {
                ret = audio_start();
                if (ret == ESP_OK) {
                    audio_started = true;
                    ESP_LOGI(TAG, "Audio started");
                } else {
                    ESP_LOGE(TAG, "audio_start failed: %s", esp_err_to_name(ret));
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
            }
            
            /* Read from microphone */
            ret = audio_read(rx_buffer, sizeof(rx_buffer), &bytes_read, 100);
            if (ret == ESP_OK && bytes_read > 0) {
                /* Calculate RMS value */
                int64_t sum = 0;
                size_t samples = bytes_read / sizeof(int16_t);
                for (size_t i = 0; i < samples; i++) {
                    sum += (int64_t)rx_buffer[i] * rx_buffer[i];
                }
                double rms = sqrt((double)sum / samples);
                
                /* Log every 50 iterations */
                if (loop_count % 50 == 0) {
                    ESP_LOGI(TAG, "Audio capture: %d bytes, RMS: %.1f, WS: %s", 
                             bytes_read, rms, ws_connected ? "connected" : "disconnected");
                }
                
                /* Send audio via WebSocket if connected */
                if (ws_connected) {
                    ws_client_send((const uint8_t *)rx_buffer, bytes_read);
                }
                
                /* Loopback: write to speaker */
                size_t bytes_written = 0;
                audio_write(rx_buffer, bytes_read, &bytes_written, 100);
            } else if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "audio_read: %s", esp_err_to_name(ret));
            }
            
            /* Periodically output test tone to verify speaker (every 100 loops ≈ 1 sec) */
            if (loop_count > 0 && loop_count % 100 == 0) {
                ESP_LOGI(TAG, "Speaker test: outputting sine wave");
                size_t tw = 0;
                audio_write(audio_buffer, sizeof(audio_buffer), &tw, 100);
            }
            
            loop_count++;
        } else {
            if (audio_started) {
                audio_stop();
                audio_started = false;
                ESP_LOGI(TAG, "Audio stopped");
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * Main application entry point
 */
void app_main(void)
{
    printf("\n");
    printf("========================================\n");
    printf("ESP001 Audio + WebSocket Test\n");
    printf("========================================\n");
    printf("Chip: %s\n", CONFIG_IDF_TARGET);
    printf("========================================\n");
    
    /* Initialize UART for command interface */
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
    
    printf("UART initialized at %d baud\n", USB_UART_BAUD);
    
    /* Create UART task */
    xTaskCreate(uart_event_task, "uart_task", 4096, NULL, 5, NULL);
    
    /* Create audio task on Core 0 (highest priority, runs first) */
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        audio_task, "audio_task", 
        8192, NULL, 6, 
        &audio_task_handle, 0);
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio task");
    } else {
        ESP_LOGI(TAG, "Audio task created");
    }
    
    /* Create WiFi task on Core 1 (lower priority, non-blocking) */
    task_ret = xTaskCreatePinnedToCore(
        wifi_task, "wifi_task", 
        8192, NULL, 3, 
        &wifi_task_handle, 1);
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WiFi task");
        ESP_LOGI(TAG, "Continuing without WiFi - audio only mode");
    } else {
        ESP_LOGI(TAG, "WiFi task created");
    }
    
    ESP_LOGI(TAG, "System ready");
    ESP_LOGI(TAG, "WiFi: %s, WS: %s", WIFI_SSID, WS_SERVER_URL);
}
