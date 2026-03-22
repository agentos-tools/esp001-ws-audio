/**
 * ESP001 - Debug Version
 * Just printf, no UART driver
 */

#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <esp_chip_info.h>

void app_main(void)
{
    int boot_count = 0;
    
    printf("\n");
    printf("========================================\n");
    printf("ESP001 Audio System Ready\n");
    printf("========================================\n");
    printf("Chip: %s\n", CONFIG_IDF_TARGET);
    printf("========================================\n");
    
    while (1) {
        boot_count++;
        printf("[%d] Heartbeat - Free heap: %lu bytes\n", 
               boot_count, (unsigned long)esp_get_free_heap_size());
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
