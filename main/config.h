#pragma once

// ---------------------------------------------------------------------------
// MAX7219 SPI pin assignments
// Wiring: ESP32-S3 -> Hailege 4-in-1 8x8 module
//   GPIO 7  -> DIN
//   GPIO 6  -> CLK
//   GPIO 5  -> CS
// ---------------------------------------------------------------------------
#define MAX7219_PIN_MOSI    7
#define MAX7219_PIN_CLK     6
#define MAX7219_PIN_CS      5

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
