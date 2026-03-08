#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include "config.h"
#include "inputs.h"

static const char *TAG = "inputs";

// Button polling interval and long-press threshold
#define POLL_MS      20
#define LONG_THRESH  (3000 / POLL_MS)   // 150 polls = 3 seconds

static QueueHandle_t               s_evt_queue;
static adc_oneshot_unit_handle_t   s_adc_handle;
static volatile uint8_t            s_brightness = (BRIGHTNESS_MIN + BRIGHTNESS_MAX) / 2;

static void push_event(btn_event_t evt)
{
    xQueueSend(s_evt_queue, &evt, 0);   // non-blocking; drop if full
}

// ---------------------------------------------------------------------------
// Background task: polls buttons every POLL_MS ms, reads LDR every 500 ms.
// ---------------------------------------------------------------------------
static void inputs_task(void *arg)
{
    static const gpio_num_t  PIN[2]       = { BTN_WIFI_PIN, BTN_MODE_PIN };
    static const btn_event_t SHORT_EVT[2] = { BTN_EVT_WIFI_SHORT, BTN_EVT_MODE_SHORT };
    static const btn_event_t LONG_EVT[2]  = { BTN_EVT_WIFI_LONG,  BTN_EVT_MODE_LONG  };

    uint16_t press_count[2]  = {0, 0};
    bool     long_fired[2]   = {false, false};
    uint32_t ldr_ms          = 0;
    int      ldr_avg         = 2048;   // start at mid-scale

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

#ifndef BUTTONS_DISABLED
        // -- Buttons --
        for (int i = 0; i < 2; i++) {
            if (gpio_get_level(PIN[i]) == 0) {      // active-low: pressed
                press_count[i]++;
                if (press_count[i] >= LONG_THRESH && !long_fired[i]) {
                    long_fired[i] = true;
                    push_event(LONG_EVT[i]);
                }
            } else {                                // released
                if (press_count[i] >= 2 && !long_fired[i]) {
                    push_event(SHORT_EVT[i]);       // fire short press on release
                }
                press_count[i] = 0;
                long_fired[i]  = false;
            }
        }
#endif

        // -- LDR brightness (every 500 ms) --
        ldr_ms += POLL_MS;
        if (ldr_ms >= 500) {
            ldr_ms = 0;
            int raw = 0;
            if (adc_oneshot_read(s_adc_handle, (adc_channel_t)LDR_ADC_CHANNEL, &raw) == ESP_OK) {
                // Exponential moving average: 15/16 old + 1/16 new
                ldr_avg = (ldr_avg * 15 + raw) / 16;
                int mapped = LDR_INVERT ? (4095 - ldr_avg) : ldr_avg;
                s_brightness = (uint8_t)(BRIGHTNESS_MIN +
                    (mapped * (BRIGHTNESS_MAX - BRIGHTNESS_MIN)) / 4095);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void inputs_init(void)
{
    s_evt_queue = xQueueCreate(16, sizeof(btn_event_t));

#ifndef BUTTONS_DISABLED
    // GPIO buttons — internal pull-up, active low
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BTN_WIFI_PIN) | (1ULL << BTN_MODE_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);
#endif

    // ADC1 for LDR
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten    = ADC_ATTEN_DB_12,    // full 0–3.3 V range
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(
        s_adc_handle, (adc_channel_t)LDR_ADC_CHANNEL, &chan_cfg));

    xTaskCreate(inputs_task, "inputs", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Buttons: WiFi=GPIO%d  Mode=GPIO%d  LDR=ADC1_CH%d",
             BTN_WIFI_PIN, BTN_MODE_PIN, LDR_ADC_CHANNEL);
}

btn_event_t inputs_get_event(void)
{
    btn_event_t evt = BTN_EVT_NONE;
    xQueueReceive(s_evt_queue, &evt, 0);    // non-blocking
    return evt;
}

uint8_t inputs_get_brightness(void)
{
    return s_brightness;
}
