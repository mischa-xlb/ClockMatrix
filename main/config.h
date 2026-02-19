#pragma once

// ---------------------------------------------------------------------------
// MAX7219 SPI pin assignments
// Wiring: ESP32-S3 -> Hailege 4-in-1 8x8 module
//   GPIO 7  -> DIN
//   GPIO 6  -> CLK
//   GPIO 5  -> CS
// ---------------------------------------------------------------------------
#define MAX7219_PIN_MOSI    1
#define MAX7219_PIN_CLK     42
#define MAX7219_PIN_CS      2

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
#define MAX7219_REVERSE_CHAIN  0

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
#define NTP_SYNC_TIMEOUT_MS  30000
