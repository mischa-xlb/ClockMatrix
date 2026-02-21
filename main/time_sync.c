#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sntp.h"
#include "esp_log.h"

#include "config.h"
#include "time_sync.h"

static const char *TAG = "time_sync";

void time_sync_init(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER);
    esp_sntp_init();

    // Apply POSIX timezone string (handles DST automatically)
    setenv("TZ", NTP_TIMEZONE, 1);
    tzset();

    ESP_LOGI(TAG, "SNTP started — server: %s  TZ: %s", NTP_SERVER, NTP_TIMEZONE);
}

bool time_sync_wait(uint32_t timeout_ms)
{
    // Give the SNTP client a moment to send its first request before polling.
    vTaskDelay(pdMS_TO_TICKS(1000));

    uint32_t elapsed = 1000;
    while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        if (elapsed >= timeout_ms) {
            ESP_LOGW(TAG, "Timed out waiting for NTP sync — will retry in background");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        elapsed += 500;
    }

    // Log the synced time for confirmation
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);
    ESP_LOGI(TAG, "Time synced: %04d-%02d-%02d %02d:%02d:%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);

    return true;
}
