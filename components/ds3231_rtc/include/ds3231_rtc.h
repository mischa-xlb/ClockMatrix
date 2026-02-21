/**
 * @file ds3231_rtc.h
 * @brief DS3231 RTC driver — uses the ESP-IDF new I2C master API (driver/i2c_master.h)
 *        Requires ESP-IDF v5.2 or later (component: esp_driver_i2c).
 */

#ifndef DS3231_RTC_H
#define DS3231_RTC_H

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <time.h>

// I2C Configuration
#define DS3231_I2C_ADDR        0x68

// Register addresses
#define DS3231_REG_SECONDS     0x00
#define DS3231_REG_MINUTES     0x01
#define DS3231_REG_HOURS       0x02
#define DS3231_REG_DAY         0x03
#define DS3231_REG_DATE        0x04
#define DS3231_REG_MONTH       0x05
#define DS3231_REG_YEAR        0x06
#define DS3231_REG_CONTROL     0x0E
#define DS3231_REG_STATUS      0x0F

// Status flags
#define DS3231_STATUS_OSF      0x80  // Oscillator Stop Flag

// Timeout for I2C operations (ms)
#define DS3231_TIMEOUT_MS      100

// BCD conversion macros
#define BCD_TO_DEC(val) (((val) >> 4) * 10 + ((val) & 0x0F))
#define DEC_TO_BCD(val) ((((val) / 10) << 4) | ((val) % 10))

/**
 * @brief RTC handle structure
 */
typedef struct {
    i2c_master_dev_handle_t dev_handle;
    SemaphoreHandle_t      *mutex;     // Pointer to external I2C mutex (may be NULL)
    bool                    available; // true if RTC detected and working
    uint32_t                fallback_counter;
} ds3231_handle_t;

/**
 * @brief Date/time structure
 */
typedef struct {
    uint8_t second;   // 0-59
    uint8_t minute;   // 0-59
    uint8_t hour;     // 0-23
    uint8_t day;      // 1-7 (day of week)
    uint8_t date;     // 1-31
    uint8_t month;    // 1-12
    uint8_t year;     // 0-99 (2000-2099)
} ds3231_datetime_t;

/**
 * @brief Initialize DS3231 RTC.
 *
 * Adds the DS3231 to an already-created I2C master bus.  Any failure is
 * non-fatal — on error handle->available is set to false and ESP_OK is
 * returned so the caller can continue without the RTC.
 *
 * @param handle   Pointer to RTC handle
 * @param i2c_bus  I2C master bus handle (created with i2c_new_master_bus)
 * @param mutex    Pointer to an external mutex for bus sharing (may be NULL)
 * @return ESP_OK always (check handle->available for RTC presence)
 */
esp_err_t ds3231_init(ds3231_handle_t *handle,
                      i2c_master_bus_handle_t i2c_bus,
                      SemaphoreHandle_t *mutex);

/** @return true if RTC is present and functioning */
bool ds3231_is_available(ds3231_handle_t *handle);

esp_err_t ds3231_get_time(ds3231_handle_t *handle, ds3231_datetime_t *datetime);
esp_err_t ds3231_set_time(ds3231_handle_t *handle, const ds3231_datetime_t *datetime);
esp_err_t ds3231_check_oscillator_stopped(ds3231_handle_t *handle, bool *stopped);
esp_err_t ds3231_clear_oscillator_flag(ds3231_handle_t *handle);

/**
 * @brief Sync ESP32 system time from the DS3231.
 *
 * Call setenv("TZ", ...) / tzset() before this so mktime() converts the
 * RTC's local time to the correct UTC epoch value.
 */
esp_err_t ds3231_sync_system_time(ds3231_handle_t *handle);

void ds3231_to_tm(const ds3231_datetime_t *datetime, struct tm *tm_time);
void tm_to_ds3231(const struct tm *tm_time, ds3231_datetime_t *datetime);

/* Kept for source compatibility with other projects */
esp_err_t ds3231_generate_filename(ds3231_handle_t *handle,
                                   char *buffer, size_t buffer_size,
                                   const char *prefix, const char *extension,
                                   uint8_t midi_channel, uint32_t sequence);
esp_err_t ds3231_format_log_timestamp(ds3231_handle_t *handle,
                                      char *buffer, size_t buffer_size);

#endif // DS3231_RTC_H
