/**
 * @file ds3231_rtc.c
 * @brief DS3231 RTC driver — new ESP-IDF I2C master API (ESP-IDF >= v5.2)
 */

#include "ds3231_rtc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "DS3231";

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static esp_err_t i2c_take_mutex(ds3231_handle_t *h)
{
    if (h->mutex && *h->mutex) {
        if (xSemaphoreTake(*h->mutex, pdMS_TO_TICKS(DS3231_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to acquire I2C mutex");
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

static void i2c_give_mutex(ds3231_handle_t *h)
{
    if (h->mutex && *h->mutex) {
        xSemaphoreGive(*h->mutex);
    }
}

// Write register address, then read `len` bytes (repeated-start).
static esp_err_t rtc_read_regs(ds3231_handle_t *h, uint8_t reg,
                                uint8_t *data, size_t len)
{
    esp_err_t ret = i2c_take_mutex(h);
    if (ret != ESP_OK) return ret;

    ret = i2c_master_transmit_receive(h->dev_handle, &reg, 1, data, len,
                                      DS3231_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Read reg 0x%02X failed: %s", reg, esp_err_to_name(ret));
    }

    i2c_give_mutex(h);
    return ret;
}

// Write register address followed by one data byte.
static esp_err_t rtc_write_reg(ds3231_handle_t *h, uint8_t reg, uint8_t data)
{
    if (!h->available) return ESP_ERR_NOT_FOUND;

    esp_err_t ret = i2c_take_mutex(h);
    if (ret != ESP_OK) return ret;

    uint8_t buf[2] = { reg, data };
    ret = i2c_master_transmit(h->dev_handle, buf, 2, DS3231_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write reg 0x%02X failed: %s", reg, esp_err_to_name(ret));
    }

    i2c_give_mutex(h);
    return ret;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t ds3231_init(ds3231_handle_t *handle,
                      i2c_master_bus_handle_t i2c_bus,
                      SemaphoreHandle_t *mutex)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    memset(handle, 0, sizeof(ds3231_handle_t));
    handle->mutex     = mutex;
    handle->available = false;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = DS3231_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };

    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &handle->dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "DS3231 not found on I2C bus — continuing without RTC");
        return ESP_OK;   // non-fatal
    }

    // Verify the device responds
    uint8_t status;
    ret = rtc_read_regs(handle, DS3231_REG_STATUS, &status, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "DS3231 not responding — continuing without RTC");
        i2c_master_bus_rm_device(handle->dev_handle);
        handle->dev_handle = NULL;
        return ESP_OK;   // non-fatal
    }

    handle->available = true;
    ESP_LOGI(TAG, "DS3231 detected (status=0x%02X)", status);

    if (status & DS3231_STATUS_OSF) {
        ESP_LOGW(TAG, "DS3231 oscillator was stopped — time may be invalid");
        ds3231_clear_oscillator_flag(handle);
    }

    return ESP_OK;
}

bool ds3231_is_available(ds3231_handle_t *handle)
{
    return handle && handle->available;
}

esp_err_t ds3231_get_time(ds3231_handle_t *handle, ds3231_datetime_t *datetime)
{
    if (!handle || !datetime) return ESP_ERR_INVALID_ARG;
    if (!handle->available)   return ESP_ERR_NOT_FOUND;

    uint8_t data[7];
    esp_err_t ret = rtc_read_regs(handle, DS3231_REG_SECONDS, data, 7);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read time registers");
        return ret;
    }

    datetime->second = BCD_TO_DEC(data[0] & 0x7F);
    datetime->minute = BCD_TO_DEC(data[1] & 0x7F);
    datetime->hour   = BCD_TO_DEC(data[2] & 0x3F);  // 24-hour mode
    datetime->day    = BCD_TO_DEC(data[3] & 0x07);
    datetime->date   = BCD_TO_DEC(data[4] & 0x3F);
    datetime->month  = BCD_TO_DEC(data[5] & 0x1F);
    datetime->year   = BCD_TO_DEC(data[6]);

    return ESP_OK;
}

esp_err_t ds3231_set_time(ds3231_handle_t *handle, const ds3231_datetime_t *datetime)
{
    if (!handle || !datetime) return ESP_ERR_INVALID_ARG;
    if (!handle->available)   return ESP_ERR_NOT_FOUND;

    if (datetime->second > 59 || datetime->minute > 59 || datetime->hour > 23 ||
        datetime->day < 1 || datetime->day > 7 ||
        datetime->date < 1 || datetime->date > 31 ||
        datetime->month < 1 || datetime->month > 12 || datetime->year > 99) {
        ESP_LOGE(TAG, "Invalid datetime values");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;
    ret = rtc_write_reg(handle, DS3231_REG_SECONDS, DEC_TO_BCD(datetime->second));
    if (ret != ESP_OK) return ret;
    ret = rtc_write_reg(handle, DS3231_REG_MINUTES, DEC_TO_BCD(datetime->minute));
    if (ret != ESP_OK) return ret;
    ret = rtc_write_reg(handle, DS3231_REG_HOURS,   DEC_TO_BCD(datetime->hour));
    if (ret != ESP_OK) return ret;
    ret = rtc_write_reg(handle, DS3231_REG_DAY,     DEC_TO_BCD(datetime->day));
    if (ret != ESP_OK) return ret;
    ret = rtc_write_reg(handle, DS3231_REG_DATE,    DEC_TO_BCD(datetime->date));
    if (ret != ESP_OK) return ret;
    ret = rtc_write_reg(handle, DS3231_REG_MONTH,   DEC_TO_BCD(datetime->month));
    if (ret != ESP_OK) return ret;
    ret = rtc_write_reg(handle, DS3231_REG_YEAR,    DEC_TO_BCD(datetime->year));
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Time set: 20%02d-%02d-%02d %02d:%02d:%02d",
             datetime->year, datetime->month, datetime->date,
             datetime->hour, datetime->minute, datetime->second);
    return ESP_OK;
}

esp_err_t ds3231_check_oscillator_stopped(ds3231_handle_t *handle, bool *stopped)
{
    if (!handle || !stopped) return ESP_ERR_INVALID_ARG;
    if (!handle->available)  return ESP_ERR_NOT_FOUND;

    uint8_t status;
    esp_err_t ret = rtc_read_regs(handle, DS3231_REG_STATUS, &status, 1);
    if (ret != ESP_OK) return ret;

    *stopped = (status & DS3231_STATUS_OSF) != 0;
    return ESP_OK;
}

esp_err_t ds3231_clear_oscillator_flag(ds3231_handle_t *handle)
{
    if (!handle || !handle->available) return ESP_ERR_NOT_FOUND;

    uint8_t status;
    esp_err_t ret = rtc_read_regs(handle, DS3231_REG_STATUS, &status, 1);
    if (ret != ESP_OK) return ret;

    status &= ~DS3231_STATUS_OSF;
    return rtc_write_reg(handle, DS3231_REG_STATUS, status);
}

void ds3231_to_tm(const ds3231_datetime_t *datetime, struct tm *tm_time)
{
    if (!datetime || !tm_time) return;
    memset(tm_time, 0, sizeof(struct tm));
    tm_time->tm_sec  = datetime->second;
    tm_time->tm_min  = datetime->minute;
    tm_time->tm_hour = datetime->hour;
    tm_time->tm_mday = datetime->date;
    tm_time->tm_mon  = datetime->month - 1;    // 0-based
    tm_time->tm_year = datetime->year + 100;   // years since 1900
    tm_time->tm_wday = datetime->day - 1;      // 0 = Sunday
}

void tm_to_ds3231(const struct tm *tm_time, ds3231_datetime_t *datetime)
{
    if (!tm_time || !datetime) return;
    memset(datetime, 0, sizeof(ds3231_datetime_t));
    datetime->second = (uint8_t)tm_time->tm_sec;
    datetime->minute = (uint8_t)tm_time->tm_min;
    datetime->hour   = (uint8_t)tm_time->tm_hour;
    datetime->date   = (uint8_t)tm_time->tm_mday;
    datetime->month  = (uint8_t)(tm_time->tm_mon + 1);
    datetime->year   = (uint8_t)(tm_time->tm_year - 100);
    datetime->day    = (uint8_t)(tm_time->tm_wday + 1);
}

esp_err_t ds3231_sync_system_time(ds3231_handle_t *handle)
{
    if (!handle)            return ESP_ERR_INVALID_ARG;
    if (!handle->available) {
        ESP_LOGW(TAG, "Cannot sync — RTC not available");
        return ESP_ERR_NOT_FOUND;
    }

    ds3231_datetime_t rtc_time;
    esp_err_t ret = ds3231_get_time(handle, &rtc_time);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read RTC for sync");
        return ESP_FAIL;
    }

    struct tm timeinfo = {0};
    ds3231_to_tm(&rtc_time, &timeinfo);
    time_t timestamp = mktime(&timeinfo);

    struct timeval tv = { .tv_sec = timestamp, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGE(TAG, "settimeofday failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "System time synced from RTC: 20%02d-%02d-%02d %02d:%02d:%02d",
             rtc_time.year, rtc_time.month, rtc_time.date,
             rtc_time.hour, rtc_time.minute, rtc_time.second);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Legacy filename / log helpers
// ---------------------------------------------------------------------------

esp_err_t ds3231_generate_filename(ds3231_handle_t *handle,
                                   char *buffer, size_t buffer_size,
                                   const char *prefix, const char *extension,
                                   uint8_t midi_channel, uint32_t sequence)
{
    if (!handle || !buffer || !prefix || !extension) return ESP_ERR_INVALID_ARG;

    if (handle->available) {
        ds3231_datetime_t dt;
        if (ds3231_get_time(handle, &dt) == ESP_OK) {
            snprintf(buffer, buffer_size,
                     "midi_%05" PRIu32 "_20%02d%02d%02d_%02d%02d%02d_CH%02d%s",
                     sequence, dt.year, dt.month, dt.date,
                     dt.hour, dt.minute, dt.second, midi_channel, extension);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "RTC read failed, using fallback filename");
    }

    snprintf(buffer, buffer_size, "midi_%05" PRIu32 "_CH%02d%s",
             sequence, midi_channel, extension);
    return ESP_OK;
}

esp_err_t ds3231_format_log_timestamp(ds3231_handle_t *handle,
                                      char *buffer, size_t buffer_size)
{
    if (!handle || !buffer) return ESP_ERR_INVALID_ARG;

    if (handle->available) {
        ds3231_datetime_t dt;
        if (ds3231_get_time(handle, &dt) == ESP_OK) {
            snprintf(buffer, buffer_size, "[20%02d-%02d-%02d %02d:%02d:%02d]",
                     dt.year, dt.month, dt.date,
                     dt.hour, dt.minute, dt.second);
            return ESP_OK;
        }
    }

    int64_t us = esp_timer_get_time();
    int s = (int)(us / 1000000);
    snprintf(buffer, buffer_size, "[uptime %02d:%02d:%02d]",
             s / 3600, (s % 3600) / 60, s % 60);
    return ESP_OK;
}
