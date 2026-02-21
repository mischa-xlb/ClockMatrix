#pragma once

#include <stdint.h>
#include <stdbool.h>

// Initialise SPI bus and all MAX7219 modules in the chain.
// Must be called once before any other max7219_* function.
void max7219_init(void);

// Write a single decimal digit (0-9) to one module.
// module: 0 = leftmost panel, (NUM_MODULES-1) = rightmost panel.
// digit:  0-9
void max7219_set_digit(uint8_t module, uint8_t digit);

// Write a digit into the frame buffer only — does NOT push to hardware.
// Call max7219_refresh_digits() once all four modules are updated.
void max7219_put_digit(uint8_t module, uint8_t digit);

// Blank a module's digit rows in the frame buffer (rows 1-6).
// Useful for suppressing a leading zero.
void max7219_put_blank(uint8_t module);

// Set colon visibility.  Applied automatically during max7219_refresh_digits().
// The colon appears as two pixels in the rightmost area of module 1 (between HH and MM).
void max7219_set_colon(bool visible);

// Push rows 2-7 (digit area) from the frame buffer to all modules at once.
// Also applies the current colon state.
void max7219_refresh_digits(void);

// Update the bottom-row seconds progress bar and flush row 8 immediately.
//
// Seconds 0-30  : LEDs fill left-to-right  (0 s = blank, 30 s = 30 LEDs lit)
// Seconds 31-59 : right edge stays at LED 30, left edge retreats right
//                 (31 s = LEDs 2-30, 59 s = LED 30 only)
void max7219_set_seconds_bar(uint8_t second);

// Clear (blank) all modules.
void max7219_clear_all(void);

// Set brightness of all modules.
// intensity: 0 (dimmest) to 15 (brightest)
void max7219_set_intensity(uint8_t intensity);
