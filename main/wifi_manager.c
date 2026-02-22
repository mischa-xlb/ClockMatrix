#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "config.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_mgr";

#define NVS_NAMESPACE       "wifi_cfg"
#define NVS_KEY_SSID        "ssid"
#define NVS_KEY_PASS        "pass"
#define NVS_KEY_SCENE_MASK  "scene_mask"

// Scene names must match the scene_t enum order in main.c.
static const char *SCENE_NAMES[] = {
    "Scroll", "Explode", "Decay", "Melt", "Wiper", "Blink", "Blend"
};
#define NUM_SCENES  7

#define BIT_CONNECTED  BIT0
#define BIT_FAILED     BIT1

static EventGroupHandle_t   s_wifi_events;
static int                  s_retry_count;
static SemaphoreHandle_t    s_portal_done;

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------

static bool load_credentials(char *ssid, size_t ssid_len,
                              char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    bool ok = (nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_len) == ESP_OK) &&
              (nvs_get_str(h, NVS_KEY_PASS, pass, &pass_len) == ESP_OK) &&
              ssid[0] != '\0';
    nvs_close(h);
    return ok;
}

static void save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, NVS_KEY_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, NVS_KEY_PASS, pass));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "Credentials saved for SSID: %s", ssid);
}

// ---------------------------------------------------------------------------
// Scene mask NVS helpers
// ---------------------------------------------------------------------------

uint8_t wifi_manager_get_scene_mask(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0xFF;
    uint8_t mask = 0xFF;
    nvs_get_u8(h, NVS_KEY_SCENE_MASK, &mask);  // leaves mask unchanged if key absent
    nvs_close(h);
    return (mask == 0) ? 0xFF : mask;           // treat all-zero as "all enabled"
}

void wifi_manager_set_scene_mask(uint8_t mask)
{
    if (mask == 0) mask = 0xFF;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_SCENE_MASK, mask);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Scene mask saved: 0x%02X", mask);
}

// ---------------------------------------------------------------------------
// Simple URL decoder (handles + and %XX)
// ---------------------------------------------------------------------------

static void url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t i = 0, j = 0;
    while (src[i] && j < dst_len - 1) {
        if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], '\0' };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

// Extract a URL-encoded field value from a POST body (key=value&...).
static void extract_field(const char *body, const char *key,
                           char *out, size_t out_len)
{
    char search[34];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(search);

    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);

    char encoded[128] = {0};
    if (len >= sizeof(encoded)) len = sizeof(encoded) - 1;
    memcpy(encoded, p, len);

    url_decode(encoded, out, out_len);
}

// ---------------------------------------------------------------------------
// STA event handler
// ---------------------------------------------------------------------------

static void sta_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_STA_MAX_RETRIES) {
            s_retry_count++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, BIT_CONNECTED);
    }
}

// ---------------------------------------------------------------------------
// Portal HTTP handlers
// ---------------------------------------------------------------------------

// Static HTML parts — split so checkboxes can be injected between them.
static const char PORTAL_PART1[] =
    "<!DOCTYPE html><html>"
    "<head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ClockMatrix Setup</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 20px}"
    "h2,h3{color:#333}"
    "label{display:block;margin-top:12px;color:#555}"
    "input[type=text],input[type=password]{width:100%;padding:8px;margin-top:4px;"
    "box-sizing:border-box;border:1px solid #ccc;border-radius:4px;font-size:15px}"
    "button{margin-top:20px;width:100%;padding:12px;background:#0070f3;"
    "color:#fff;border:none;border-radius:4px;font-size:16px;cursor:pointer}"
    "</style></head>"
    "<body><h2>ClockMatrix WiFi Setup</h2>"
    "<form method='POST' action='/save'>"
    "<label>WiFi SSID"
    "<input name='ssid' type='text' placeholder='Network name' required></label>"
    "<label>Password"
    "<input name='pass' type='password' placeholder='Leave blank for open network'></label>"
    "<h3>Animation Scenes</h3>";

static const char PORTAL_PART2[] =
    "<button type='submit'>Save &amp; Reboot</button>"
    "</form></body></html>";

static esp_err_t root_handler(httpd_req_t *req)
{
    uint8_t mask = wifi_manager_get_scene_mask();

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, PORTAL_PART1, HTTPD_RESP_USE_STRLEN);

    char cb[128];
    for (int i = 0; i < NUM_SCENES; i++) {
        bool en = (mask >> i) & 1;
        snprintf(cb, sizeof(cb),
            "<label><input name='sc%d' type='checkbox' value='1'%s> %s</label>",
            i, en ? " checked" : "", SCENE_NAMES[i]);
        httpd_resp_send_chunk(req, cb, HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send_chunk(req, PORTAL_PART2, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);  // end chunked response
    return ESP_OK;
}

static esp_err_t save_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int  received  = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[64] = {0}, pass[64] = {0};
    extract_field(body, "ssid", ssid, sizeof(ssid));
    extract_field(body, "pass", pass, sizeof(pass));

    if (ssid[0] == '\0') {
        httpd_resp_sendstr(req, "<html><body><b>SSID cannot be empty.</b>"
                                " <a href='/'>Go back</a></body></html>");
        return ESP_OK;
    }

    save_credentials(ssid, pass);

    // Parse scene-enable checkboxes (only present in POST body when checked).
    uint8_t new_mask = 0;
    for (int i = 0; i < NUM_SCENES; i++) {
        char key[5];
        snprintf(key, sizeof(key), "sc%d", i);
        char val[4] = {0};
        extract_field(body, key, val, sizeof(val));
        if (val[0] == '1') new_mask |= (uint8_t)(1u << i);
    }
    wifi_manager_set_scene_mask(new_mask);  // 0 -> all-enabled handled inside

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<html><body><h2>Saved!</h2>"
        "<p>Rebooting in 2 seconds&hellip;</p></body></html>");

    xSemaphoreGive(s_portal_done);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void wifi_manager_init(void)
{
    // NVS is required for WiFi driver and credential storage.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
}

bool wifi_manager_connect_sta(void)
{
    char ssid[64] = {0}, pass[64] = {0};
    if (!load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "No stored WiFi credentials found");
        return false;
    }

    ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);

    s_wifi_events  = xEventGroupCreate();
    s_retry_count  = 0;

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, sta_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, sta_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid,     ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, BIT_CONNECTED | BIT_FAILED,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & BIT_CONNECTED) {
        ESP_LOGI(TAG, "Connected to %s", ssid);
        // Do NOT delete s_wifi_events here — the event handlers remain
        // registered and will call xEventGroupSetBits() on it if WiFi
        // drops and retries.  Deleting it now would leave a dangling pointer
        // and crash the device on the next disconnect event.
        return true;
    }

    ESP_LOGW(TAG, "Failed to connect to %s", ssid);
    esp_wifi_stop();
    esp_wifi_deinit();
    vEventGroupDelete(s_wifi_events);
    return false;
}

void wifi_manager_start_portal(void)
{
    ESP_LOGI(TAG, "Starting setup portal — connect to AP \"%s\" "
                  "then open http://192.168.4.1", WIFI_AP_SSID);

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid            = WIFI_AP_SSID,
            .ssid_len        = (uint8_t)strlen(WIFI_AP_SSID),
            .password        = WIFI_AP_PASSWORD,
            .max_connection  = 4,
            .authmode        = (strlen(WIFI_AP_PASSWORD) > 0)
                                   ? WIFI_AUTH_WPA2_PSK
                                   : WIFI_AUTH_OPEN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Start HTTP server
    s_portal_done = xSemaphoreCreateBinary();

    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server    = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &httpd_cfg));

    httpd_uri_t root_uri = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = root_handler,
    };
    httpd_uri_t save_uri = {
        .uri     = "/save",
        .method  = HTTP_POST,
        .handler = save_handler,
    };
    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &save_uri);

    // Block until the user submits credentials
    xSemaphoreTake(s_portal_done, portMAX_DELAY);

    vTaskDelay(pdMS_TO_TICKS(2000)); // give the browser time to receive the response
    esp_restart();
}

void wifi_manager_reset_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "WiFi credentials erased — rebooting to portal");
    esp_restart();
}
