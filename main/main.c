/**
 * ESP001 - ESP32-S3 WebSocket Audio
 * 
 * Main entry point with FreeRTOS task framework
 */

#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "audio_driver.h"

static const char *TAG = "ESP001";

/* Task handles */
static TaskHandle_t audio_task_handle = NULL;
static TaskHandle_t status_task_handle = NULL;

/* Audio buffer */
#define AUDIO_BUFFER_SIZE   1024
static int16_t audio_buffer[AUDIO_BUFFER_SIZE];

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
 * Audio processing task
 * Handles I2S audio capture and playback
 */
static void audio_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Audio task started");
    
    /* Initialize audio driver */
    esp_err_t ret = audio_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize audio driver");
        vTaskDelete(NULL);
        return;
    }
    
    /* Start audio */
    ret = audio_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Audio driver initialized and started");
    
    /* Generate test tone (1kHz) */
    generate_sine_wave(audio_buffer, AUDIO_BUFFER_SIZE, 1000, AUDIO_SAMPLE_RATE);
    
    size_t bytes_written = 0;
    size_t bytes_read = 0;
    int16_t rx_buffer[AUDIO_BUFFER_SIZE];
    uint32_t loop_count = 0;
    
    while (1) {
        /* Write test tone to speaker (optional - comment out for mic-only test) */
        // audio_write(audio_buffer, sizeof(audio_buffer), &bytes_written, 100);
        
        /* Read from microphone */
        ret = audio_read(rx_buffer, sizeof(rx_buffer), &bytes_read, 100);
        if (ret == ESP_OK && bytes_read > 0) {
            /* Calculate RMS value to verify audio capture */
            int64_t sum = 0;
            size_t samples = bytes_read / sizeof(int16_t);
            for (size_t i = 0; i < samples; i++) {
                sum += (int64_t)rx_buffer[i] * rx_buffer[i];
            }
            double rms = sqrt((double)sum / samples);
            
            /* Log every 100 iterations */
            if (loop_count % 100 == 0) {
                ESP_LOGI(TAG, "Audio capture: %d bytes, RMS: %.1f", bytes_read, rms);
            }
        }
        
        loop_count++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * Status LED task
 * Handles LED status indication
 */
static void status_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Status task started");
    
    while (1) {
        // TODO: LED status indication
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * Main application entry point
 */
void app_main(void)
{
    ESP_LOGI(TAG, "ESP001 Audio System Starting...");
    ESP_LOGI(TAG, "ESP-IDF version: %s", esp_get_idf_version());
    
    /* Print chip information */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Chip: %s, %d CPU cores", CONFIG_IDF_TARGET, chip_info.cores);
    
    /* Create FreeRTOS tasks */
    BaseType_t ret;
    
    // Audio task - Core 0, Priority 5 (highest)
    ret = xTaskCreatePinnedToCore(audio_task, "audio_task", 
                                   8192, NULL, 5, 
                                   &audio_task_handle, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio task");
    }
    
    // Status task - Core 1, Priority 1 (lowest)
    ret = xTaskCreatePinnedToCore(status_task, "status_task",
                                   2048, NULL, 1,
                                   &status_task_handle, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create status task");
    }
    
    ESP_LOGI(TAG, "All tasks created successfully");
}