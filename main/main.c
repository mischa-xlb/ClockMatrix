#include <time.h>
#include <sys/time.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
//#include "driver/i2c_master.h"
#include "config.h"
#include "max7219.h"
#include "wifi_manager.h"
#include "time_sync.h"
#include "inputs.h"
#include "ds3231_rtc.h"

static const char *TAG = "main";


// ---------------------------------------------------------------------------
// RTC state (file-scope so handle_button can also write to it)
// ---------------------------------------------------------------------------
#ifndef RTC_DISABLED
static i2c_master_bus_handle_t s_i2c_bus   = NULL;
static SemaphoreHandle_t       s_i2c_mutex = NULL;
static ds3231_handle_t         rtc_handle  = {0};
static bool                    s_rtc_ok    = false;
#endif

// ---------------------------------------------------------------------------
// Scenes — order must match SCENE_NAMES[] in wifi_manager.c.
// ---------------------------------------------------------------------------
typedef enum {
    SCENE_SCROLL = 0,   // old digit slides down, new enters from above
    SCENE_EXPLODE,      // digit shrinks to dot, explodes outward
    SCENE_DECAY,        // columns disappear in scattered order, new digit appears
    SCENE_MELT,         // pixels fall to a heap, new digit reforms from below
    SCENE_WIPER,        // vertical bar sweeps left-to-right revealing new digit
    SCENE_BLINK,        // old digit flickers out, new digit flickers in
    SCENE_BLEND,        // cross-dissolve through the centre column
    SCENE_COUNT
} scene_t;

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
typedef enum {
    APPST_NORMAL = 0,
    APPST_SET_HOUR,
    APPST_SET_MINUTE,
} appst_t;

static scene_t  g_scene      = SCENE_SCROLL;
static uint8_t  g_scene_mask = 0xFF;   // loaded from NVS in app_main
static appst_t  g_state      = APPST_NORMAL;
static int      g_set_h      = 12;
static int      g_set_m      = 0;

// ---------------------------------------------------------------------------
// Animation state (used by SCENE_SCROLL and SCENE_EXPLODE)
// ---------------------------------------------------------------------------
typedef struct {
    bool    active;
    uint8_t old_digit;   // 0-9 or MAX7219_BLANK
    uint8_t new_digit;   // 0-9 or MAX7219_BLANK
    int     frame;       // 1-based frame index
    int     tick;        // render-tick counter within current frame (0..ANIM_TICKS_PER_FRAME-1)
} mod_anim_t;

static mod_anim_t s_anim[MAX7219_NUM_MODULES];
static uint8_t    s_displayed[MAX7219_NUM_MODULES]; // digit currently tracked on each module
static bool       s_anim_initialised = false;       // false forces a silent init on first render

// ---------------------------------------------------------------------------
// Scene helpers
// ---------------------------------------------------------------------------

// 0-indexed position of `sc` among currently enabled scenes.
static int scene_position(scene_t sc)
{
    int pos = 0;
    for (int i = 0; i < (int)sc; i++) {
        if (g_scene_mask & (1u << i)) pos++;
    }
    return pos;
}

static int count_enabled_scenes(void)
{
    int n = 0;
    for (int i = 0; i < SCENE_COUNT; i++) {
        if (g_scene_mask & (1u << i)) n++;
    }
    return n;
}

// Advance to the next enabled scene, wrapping around.
static scene_t next_enabled_scene(scene_t current)
{
    for (int i = 1; i <= SCENE_COUNT; i++) {
        scene_t candidate = (scene_t)((current + i) % SCENE_COUNT);
        if (g_scene_mask & (1u << candidate)) return candidate;
    }
    return current;  // all others disabled — stay on current
}

// First enabled scene in enum order.
static scene_t first_enabled_scene(void)
{
    for (int i = 0; i < SCENE_COUNT; i++) {
        if (g_scene_mask & (1u << i)) return (scene_t)i;
    }
    return SCENE_SCROLL;  // fallback
}

// Frame count for each scene type.
static int max_frames_for_scene(scene_t sc)
{
    switch (sc) {
        case SCENE_SCROLL:  return ANIM_FRAMES_SCROLL;
        case SCENE_EXPLODE: return ANIM_FRAMES_EXPLODE;
        case SCENE_DECAY:   return ANIM_FRAMES_DECAY;
        case SCENE_MELT:    return ANIM_FRAMES_MELT;
        case SCENE_WIPER:   return ANIM_FRAMES_WIPER;
        case SCENE_BLINK:   return ANIM_FRAMES_BLINK;
        case SCENE_BLEND:   return ANIM_FRAMES_BLEND;
        default:            return ANIM_FRAMES_SCROLL;
    }
}

// Render one animation frame for module `m` using the current scene.
static void render_module_anim(int m)
{
    switch (g_scene) {
        case SCENE_SCROLL:
            max7219_anim_scroll (m, s_anim[m].old_digit, s_anim[m].new_digit, s_anim[m].frame);
            break;
        case SCENE_EXPLODE:
            max7219_anim_explode(m, s_anim[m].old_digit, s_anim[m].new_digit, s_anim[m].frame);
            break;
        case SCENE_DECAY:
            max7219_anim_decay  (m, s_anim[m].old_digit, s_anim[m].new_digit, s_anim[m].frame);
            break;
        case SCENE_MELT:
            max7219_anim_melt   (m, s_anim[m].old_digit, s_anim[m].new_digit, s_anim[m].frame);
            break;
        case SCENE_WIPER:
            max7219_anim_wiper  (m, s_anim[m].old_digit, s_anim[m].new_digit, s_anim[m].frame);
            break;
        case SCENE_BLINK:
            max7219_anim_blink  (m, s_anim[m].old_digit, s_anim[m].new_digit, s_anim[m].frame);
            break;
        case SCENE_BLEND:
            max7219_anim_blend  (m, s_anim[m].old_digit, s_anim[m].new_digit, s_anim[m].frame);
            break;
        default:
            max7219_anim_scroll (m, s_anim[m].old_digit, s_anim[m].new_digit, s_anim[m].frame);
            break;
    }
}

// ---------------------------------------------------------------------------
// RTC helpers (compiled out when RTC_DISABLED)
// ---------------------------------------------------------------------------
#ifndef RTC_DISABLED

// Initialise the I2C bus and DS3231.
//
// Returns true  — RTC detected, oscillator running, and the stored date is
//                 after 2026-02-20, so the time can be trusted immediately.
// Returns false — RTC absent, oscillator was stopped (power loss), or the
//                 date is implausible.  s_rtc_ok is still set if the RTC
//                 hardware is present so NTP can write to it later.
static bool rtc_setup(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = I2C_MASTER_NUM,
        .scl_io_num                   = I2C_MASTER_SCL_IO,
        .sda_io_num                   = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "RTC: I2C bus init failed — continuing without RTC");
        return false;
    }

    s_i2c_mutex = xSemaphoreCreateMutex();

    if (ds3231_init(&rtc_handle, s_i2c_bus, &s_i2c_mutex) != ESP_OK ||
        !ds3231_is_available(&rtc_handle)) {
        ESP_LOGW(TAG, "RTC: DS3231 not detected — continuing without RTC");
        return false;
    }

    // Hardware is present — mark OK so NTP can write to it even if the
    // stored time is currently untrustworthy.
    s_rtc_ok = true;
    ESP_LOGI(TAG, "RTC: DS3231 detected");

    // If the oscillator was stopped (battery removed / first use), the time
    // registers are invalid.  Clear the flag and wait for NTP.
    bool osc_stopped = false;
    if (ds3231_check_oscillator_stopped(&rtc_handle, &osc_stopped) == ESP_OK
            && osc_stopped) {
        ESP_LOGW(TAG, "RTC: oscillator was stopped — time invalid, will wait for NTP");
        ds3231_clear_oscillator_flag(&rtc_handle);
        return false;
    }

    // Read the stored time and check it is plausible (after 2026-02-20).
    ds3231_datetime_t dt;
    if (ds3231_get_time(&rtc_handle, &dt) != ESP_OK) {
        ESP_LOGW(TAG, "RTC: could not read time registers");
        return false;
    }

    ESP_LOGI(TAG, "RTC time: 20%02d-%02d-%02d %02d:%02d:%02d",
             dt.year, dt.month, dt.date, dt.hour, dt.minute, dt.second);

    bool plausible = (dt.year > 26) ||
                     (dt.year == 26 && dt.month > 2) ||
                     (dt.year == 26 && dt.month == 2 && dt.date > 20);
    if (!plausible) {
        ESP_LOGW(TAG, "RTC date is before 2026-02-20 — will wait for NTP");
        return false;
    }

    return true;   // time is valid; caller should sync system clock now
}

// Write the current system time back to the RTC (called after NTP sync or
// after the user manually sets the time).
static void rtc_write_system_time(void)
{
    if (!s_rtc_ok) return;
    time_t now; struct tm ti;
    time(&now);
    localtime_r(&now, &ti);
    ds3231_datetime_t dt;
    tm_to_ds3231(&ti, &dt);
    if (ds3231_set_time(&rtc_handle, &dt) == ESP_OK) {
        ESP_LOGI(TAG, "RTC updated: 20%02d-%02d-%02d %02d:%02d:%02d",
                 dt.year, dt.month, dt.date, dt.hour, dt.minute, dt.second);
    } else {
        ESP_LOGW(TAG, "RTC write failed");
    }
}

#endif // RTC_DISABLED

// ---------------------------------------------------------------------------
// Render helpers
// ---------------------------------------------------------------------------

// Push one complete time frame to the display.
// blank_hour / blank_min suppress those panels (used for blink effect).
static void render_time(int hour24, int minute, bool colon_on,
                        bool blank_hour, bool blank_min)
{
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;

    // Hour tens: blank when 0 (single-digit hours: 1–9)
    if (blank_hour) {
        max7219_put_blank(0);
        max7219_put_blank(1);
    } else {
        if (hour12 < 10) max7219_put_blank(0);
        else             max7219_put_digit(0, hour12 / 10);
        max7219_put_digit(1, hour12 % 10);
    }

    if (blank_min) {
        max7219_put_blank(2);
        max7219_put_blank(3);
    } else {
        max7219_put_digit(2, minute / 10);
        max7219_put_digit(3, minute % 10);
    }

    max7219_set_colon(colon_on);
    max7219_refresh_digits();
}

// Render the current animated clock scene.
// Called every render tick (50 ms).  Each animation frame is held for
// ANIM_TICKS_PER_FRAME ticks before advancing.  The colon blinks on the
// real second boundary and the seconds bar runs normally.
// A dot indicator in the top-left shows which scene comes next in the cycle.
static void render_animated(void)
{
    time_t    now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    int hour12 = ti.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;

    uint8_t want[MAX7219_NUM_MODULES];
    want[0] = (hour12 >= 10) ? (uint8_t)(hour12 / 10) : MAX7219_BLANK;
    want[1] = (uint8_t)(hour12 % 10);
    want[2] = (uint8_t)(ti.tm_min / 10);
    want[3] = (uint8_t)(ti.tm_min % 10);

    bool    colon_on = (ti.tm_sec % 2 == 0);
    uint8_t sec      = (uint8_t)ti.tm_sec;

    // Compute scene indicator: dots = 1-indexed position of the NEXT scene
    // in the enabled-scene cycle (so pressing MODE shows that many dots next).
    int total     = count_enabled_scenes();
    int cur_pos   = scene_position(g_scene);
    int next_pos  = (total > 1) ? (cur_pos + 1) % total : cur_pos;
    uint8_t dots  = (uint8_t)(next_pos + 1);

    // First call after a scene switch: show current time immediately, no animation.
    if (!s_anim_initialised) {
        memcpy(s_displayed, want, sizeof(want));
        memset(s_anim, 0, sizeof(s_anim));
        s_anim_initialised = true;
        for (int m = 0; m < MAX7219_NUM_MODULES; m++) {
            if (s_displayed[m] == MAX7219_BLANK) max7219_put_blank(m);
            else                                 max7219_put_digit(m, s_displayed[m]);
        }
        max7219_set_colon(colon_on);
        max7219_refresh_digits();
        max7219_set_seconds_bar(sec);
        max7219_set_indicator(dots);
        return;
    }

    // Start a new animation for any module whose digit has changed.
    for (int m = 0; m < MAX7219_NUM_MODULES; m++) {
        if (!s_anim[m].active && want[m] != s_displayed[m]) {
            s_anim[m].active    = true;
            s_anim[m].old_digit = s_displayed[m];
            s_anim[m].new_digit = want[m];
            s_anim[m].frame     = 1;
            s_anim[m].tick      = 0;
            s_displayed[m]      = want[m];
        }
    }

    int max_frames = max_frames_for_scene(g_scene);

    for (int m = 0; m < MAX7219_NUM_MODULES; m++) {
        if (s_anim[m].active) {
            render_module_anim(m);
            if (++s_anim[m].tick >= ANIM_TICKS_PER_FRAME) {
                s_anim[m].tick = 0;
                s_anim[m].frame++;
                if (s_anim[m].frame > max_frames) {
                    s_anim[m].active = false;
                }
            }
        } else {
            if (s_displayed[m] == MAX7219_BLANK) max7219_put_blank(m);
            else                                  max7219_put_digit(m, s_displayed[m]);
        }
    }

    max7219_set_colon(colon_on);
    max7219_refresh_digits();
    max7219_set_seconds_bar(sec);
    max7219_set_indicator(dots);
}

static void render(bool blink_on)
{
    if (g_state != APPST_NORMAL) {
        // Time-set mode: static blinking display regardless of the active scene.
        switch (g_state) {
            case APPST_SET_HOUR:
                render_time(g_set_h, g_set_m,
                            true,               // colon steady while editing
                            !blink_on, false);  // hour digits blink
                max7219_set_seconds_bar(0);     // blank bar while editing
                break;
            case APPST_SET_MINUTE:
                render_time(g_set_h, g_set_m,
                            true,
                            false, !blink_on);  // minute digits blink
                max7219_set_seconds_bar(0);
                break;
            default:
                break;
        }
        return;
    }

    render_animated();
}

// ---------------------------------------------------------------------------
// Button event handler
// ---------------------------------------------------------------------------
static void handle_button(btn_event_t evt)
{
    if (evt == BTN_EVT_NONE) return;

    // WiFi long-press always resets credentials, regardless of state
    if (evt == BTN_EVT_WIFI_LONG) {
        wifi_manager_reset_credentials();   // erases NVS and reboots
        return;
    }

    switch (g_state) {
        // ---- Normal clock running ----
        case APPST_NORMAL:
            if (evt == BTN_EVT_WIFI_SHORT) {
                // Enter time-set: seed with the current displayed time
                time_t now; struct tm ti;
                time(&now); localtime_r(&now, &ti);
                g_set_h = ti.tm_hour;
                g_set_m = ti.tm_min;
                g_state = APPST_SET_HOUR;
                ESP_LOGI(TAG, "Entering time-set mode");
            }
            if (evt == BTN_EVT_MODE_SHORT) {
                g_scene = next_enabled_scene(g_scene);
                s_anim_initialised = false;   // reset animation state for new scene
                max7219_clear_all();
                ESP_LOGI(TAG, "Scene -> %d (mask 0x%02X)", (int)g_scene, g_scene_mask);
            }
            if (evt == BTN_EVT_MODE_LONG) {
                // No action in normal mode
            }
            break;

        // ---- Setting hours ----
        case APPST_SET_HOUR:
            if (evt == BTN_EVT_WIFI_SHORT) {
                g_set_h = (g_set_h + 1) % 24;
            }
            if (evt == BTN_EVT_MODE_SHORT) {
                g_state = APPST_SET_MINUTE;
            }
            if (evt == BTN_EVT_MODE_LONG) {
                g_state = APPST_NORMAL;         // cancel without saving
                ESP_LOGI(TAG, "Time-set cancelled");
            }
            break;

        // ---- Setting minutes ----
        case APPST_SET_MINUTE:
            if (evt == BTN_EVT_WIFI_SHORT) {
                g_set_m = (g_set_m + 1) % 60;
            }
            if (evt == BTN_EVT_MODE_SHORT) {
                // Confirm: apply the edited time while keeping the current date
                time_t now; struct tm ti;
                time(&now); localtime_r(&now, &ti);
                ti.tm_hour = g_set_h;
                ti.tm_min  = g_set_m;
                ti.tm_sec  = 0;
                struct timeval tv = { .tv_sec = mktime(&ti), .tv_usec = 0 };
                settimeofday(&tv, NULL);
                g_state = APPST_NORMAL;
                ESP_LOGI(TAG, "Time set to %02d:%02d", g_set_h, g_set_m);
#ifndef RTC_DISABLED
                // Persist the manually-set time to the RTC
                rtc_write_system_time();
#endif
            }
            if (evt == BTN_EVT_MODE_LONG) {
                g_state = APPST_NORMAL;         // cancel without saving
                ESP_LOGI(TAG, "Time-set cancelled");
            }
            break;
    }
}

// ---------------------------------------------------------------------------
// Render task — runs forever, independent of WiFi/NTP state.
// Starts as soon as the display and inputs are ready so the clock is live
// from the moment the RTC provides a valid time (or from NTP sync if not).
// ---------------------------------------------------------------------------
static void render_task(void *arg)
{
    uint64_t blink_at    = 0;
    uint64_t scene_at    = 0;   // timestamp when the current scene started
    bool     blink_on    = true;
    uint8_t  prev_bright = 255;
    scene_t  prev_scene  = g_scene;

    while (1) {
        uint64_t now_us = esp_timer_get_time();

        // Toggle blink flag every 500 ms
        if (now_us - blink_at >= 500000ULL) {
            blink_at = now_us;
            blink_on = !blink_on;
        }

        // Handle any pending button events
        handle_button(inputs_get_event());

        // If the user pressed MODE, reset the auto-advance timer so the new
        // scene gets a full interval before being replaced automatically.
        if (g_scene != prev_scene) {
            scene_at   = now_us;
            prev_scene = g_scene;
        }

#if SCENE_AUTO_ADVANCE_MS > 0
        // Auto-advance scene every SCENE_AUTO_ADVANCE_MS (normal mode only).
        if (g_state == APPST_NORMAL &&
            now_us - scene_at >= (uint64_t)SCENE_AUTO_ADVANCE_MS * 1000ULL) {
            scene_at           = now_us;
            g_scene            = next_enabled_scene(g_scene);
            prev_scene         = g_scene;
            s_anim_initialised = false;
            max7219_clear_all();
            ESP_LOGI(TAG, "Auto scene -> %d", (int)g_scene);
        }
#endif

        // Update display brightness from LDR only when it changes
        uint8_t bright = inputs_get_brightness();
        if (bright != prev_bright) {
            max7219_set_intensity(bright);
            prev_bright = bright;
        }

        render(blink_on);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ---------------------------------------------------------------------------
// app_main — init only; render_task takes over the display loop.
// ---------------------------------------------------------------------------
void app_main(void)
{
    ESP_LOGI(TAG, ">>> max7219_init");
    max7219_init();
    ESP_LOGI(TAG, ">>> max7219_clear_all");
    max7219_clear_all();
    ESP_LOGI(TAG, ">>> wifi_manager_init");
    wifi_manager_init();  // inits NVS — must come before get_scene_mask
    ESP_LOGI(TAG, ">>> inputs_init");
    inputs_init();

    // Load display rotation and scene settings from NVS.
    max7219_set_rotate(wifi_manager_get_rotate_180());
    g_scene_mask = wifi_manager_get_scene_mask();
    g_scene      = first_enabled_scene();
    ESP_LOGI(TAG, "Scene mask: 0x%02X  initial scene: %d", g_scene_mask, (int)g_scene);

    // Set timezone early so that mktime() inside ds3231_sync_system_time()
    // correctly converts the RTC's local time to a UTC epoch value.
    setenv("TZ", NTP_TIMEZONE, 1);
    tzset();

#ifndef RTC_DISABLED
    // Initialise RTC.  If it holds a valid, plausible time, load it into
    // the system clock now so the display shows the correct time immediately.
    ESP_LOGI(TAG, ">>> rtc_setup");
    if (rtc_setup()) {
        if (ds3231_sync_system_time(&rtc_handle) == ESP_OK) {
            ESP_LOGI(TAG, "System time loaded from RTC");
        }
    } else {
        ESP_LOGI(TAG, "RTC not providing time yet — display will show 00:00 until NTP");
    }
#endif

    // Start rendering now — display is live while WiFi connects and NTP syncs.
    xTaskCreate(render_task, "render", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, ">>> wifi_manager_connect_sta");
    if (!wifi_manager_connect_sta()) {
        ESP_LOGI(TAG, "No WiFi credentials — starting setup portal");
        wifi_manager_start_portal();   // blocks until credentials saved, then reboots
    }

    // NTP sync.  time_sync_init() also calls setenv/tzset — harmless.
    ESP_LOGI(TAG, ">>> time_sync_init");
    time_sync_init();
    ESP_LOGI(TAG, ">>> time_sync_wait (timeout=%d ms)", NTP_SYNC_TIMEOUT_MS);
    bool ntp_synced = time_sync_wait(NTP_SYNC_TIMEOUT_MS);
    if (!ntp_synced) {
        ESP_LOGW(TAG, "NTP timed out — clock will sync in the background");
    }

#ifndef RTC_DISABLED
    // Write the NTP-accurate time back to the RTC so the next boot can start
    // displaying immediately without waiting for a network connection.
    if (ntp_synced) {
        rtc_write_system_time();
    }
#endif

    ESP_LOGI(TAG, "Init complete — render_task running");
    vTaskDelete(NULL);   // app_main task no longer needed; render_task runs forever
}
