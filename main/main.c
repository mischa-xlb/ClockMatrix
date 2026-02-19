#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "max7219.h"
#include "wifi_manager.h"
#include "time_sync.h"

static const char *TAG = "main";

// ---------------------------------------------------------------------------
// Push a 12-hour HH:MM time to the four display panels.
// Panel 0 = tens of hour  (0 or 1)
// Panel 1 = units of hour (0-9)
// Panel 2 = tens of minute (0-5)
// Panel 3 = units of minute (0-9)
// ---------------------------------------------------------------------------
static void show_time(int hour24, int minute)
{
    // Convert to 12-hour format: 0→12, 13→1 … 23→11
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;

    // Update all four digit buffers first, then flush once to avoid flicker.
    max7219_put_digit(0, hour12 / 10);
    max7219_put_digit(1, hour12 % 10);
    max7219_put_digit(2, minute  / 10);
    max7219_put_digit(3, minute  % 10);
    max7219_refresh_digits();
}

void app_main(void)
{
    max7219_init();
    max7219_clear_all();

    // -----------------------------------------------------------------------
    // WiFi — try stored credentials; fall back to captive portal if needed.
    // -----------------------------------------------------------------------
    wifi_manager_init();

    if (!wifi_manager_connect_sta()) {
        ESP_LOGI(TAG, "No WiFi — starting setup portal");
        // portal blocks until credentials are saved, then reboots
        wifi_manager_start_portal();
    }

    // -----------------------------------------------------------------------
    // NTP — sync clock; carry on with potentially wrong time if it times out.
    // -----------------------------------------------------------------------
    time_sync_init();
    if (!time_sync_wait(NTP_SYNC_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "NTP sync failed — clock may be inaccurate");
    }

    // -----------------------------------------------------------------------
    // Main clock loop — update display every 200 ms.
    // -----------------------------------------------------------------------
    ESP_LOGI(TAG, "Clock running");

    while (1) {
        time_t    now;
        struct tm ti;
        time(&now);
        localtime_r(&now, &ti);

        show_time(ti.tm_hour, ti.tm_min);
        max7219_set_seconds_bar((uint8_t)ti.tm_sec);

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
