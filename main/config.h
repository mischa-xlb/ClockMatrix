#pragma once

// ---------------------------------------------------------------------------
// MAX7219 SPI pin assignments
// Wiring: ESP32-S3 -> Hailege 4-in-1 8x8 module
//   GPIO 7  -> DIN
//   GPIO 6  -> CLK
//   GPIO 5  -> CS
// ---------------------------------------------------------------------------

// new esp32 module
#define MAX7219_PIN_MOSI    19
#define MAX7219_PIN_CS      23
#define MAX7219_PIN_CLK     5


// I2C (NeoKey, RTC, Display) Configuration
#define I2C_MASTER_NUM          I2C_NUM_0
#define I2C_MASTER_SCL_IO       GPIO_NUM_26
#define I2C_MASTER_SDA_IO       GPIO_NUM_18
#define I2C_MASTER_FREQ_HZ      400000      // 400kHz (can change to 100000 for 100kHz)

// I2C Device Addresses
#define RTC_I2C_ADDR            0x68        // DS3231/DS1307 typical



// previous module
// #define MAX7219_PIN_MOSI    1
// #define MAX7219_PIN_CS      2
// #define MAX7219_PIN_CLK     42

// SPI host to use (SPI2_HOST = HSPI on ESP32-S3)
#define MAX7219_SPI_HOST    SPI2_HOST

// SPI clock speed in Hz (MAX7219 supports up to 10 MHz)
#define MAX7219_SPI_CLOCK   1000000

// Number of MAX7219 modules in the daisy chain
#define MAX7219_NUM_MODULES 4

// Display brightness: 0 (dimmest) to 15 (brightest)
#define MAX7219_INTENSITY   4

// Set to 1 if the leftmost digit appears at the far end of the chain
// (i.e. DIN enters from the right side of the module strip).
// Flip this if your digits appear in the wrong order.
#define MAX7219_REVERSE_CHAIN  1

// ---------------------------------------------------------------------------
// WiFi provisioning
// On first boot (or when stored credentials fail) the clock starts an open
// access point named WIFI_AP_SSID and serves a web page at 192.168.4.1
// where you can enter your home WiFi SSID and password.
// ---------------------------------------------------------------------------
#define WIFI_AP_SSID              "ClockMatrix-Setup"
#define WIFI_AP_PASSWORD          ""       // leave empty for an open AP
#define WIFI_STA_MAX_RETRIES      5
#define WIFI_CONNECT_TIMEOUT_MS   15000    // ms before falling back to portal

// ---------------------------------------------------------------------------
// NTP / time zone
// POSIX TZ strings: https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
// Examples:
//   UTC:            "UTC0"
//   UK (BST):       "GMT0BST,M3.5.0/1,M10.5.0"
//   US Eastern:     "EST5EDT,M3.2.0,M11.1.0"
//   Australia/Syd:  "AEST-10AEDT,M10.1.0,M4.1.0/3"
// ---------------------------------------------------------------------------
#define NTP_SERVER           "pool.ntp.org"
#define NTP_TIMEZONE         "GMT0BST,M3.5.0/1,M10.5.0"
#define NTP_SYNC_TIMEOUT_MS  60000

// ---------------------------------------------------------------------------
// Button inputs — active LOW (connect button between pin and GND).
// Internal pull-up is enabled; no external resistor needed.
//   BTN_WIFI short press : enter manual time-set mode (or increment field)
//   BTN_WIFI long press  : clear stored WiFi credentials → restart portal
//   BTN_MODE short press : next field in time-set, or next scene in clock mode
//   BTN_MODE long press  : cancel time-set without saving
// ---------------------------------------------------------------------------
#define BTN_WIFI_PIN   33   // safe on ESP32 (was GPIO 8, a flash pin on classic ESP32)
#define BTN_MODE_PIN   3   // safe on ESP32 (was GPIO 9, a flash pin on classic ESP32)

// ---------------------------------------------------------------------------
// Light-dependent resistor (LDR) — automatic brightness control
// Wiring: LDR between 3.3 V and ADC pin; 10 kΩ resistor between ADC pin and GND.
// ADC1_CH3 = GPIO 4 on ESP32-S3.
// Set LDR_INVERT 1 if your divider is the other way round (high reading = dark).
// ---------------------------------------------------------------------------
#define LDR_ADC_CHANNEL  3     // ADC1_CH3 = GPIO 4
#define LDR_INVERT       1
#define BRIGHTNESS_MIN   1     // minimum intensity at night  (0–15)
#define BRIGHTNESS_MAX   12    // maximum intensity in daylight (0–15)
