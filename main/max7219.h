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

// Sentinel value for max7219_put_digit / animation functions: module shows blank.
#define MAX7219_BLANK 10

// Number of frames for each animation type (at 50 ms per render tick;
// actual frame duration = ANIM_TICKS_PER_FRAME * 50 ms).
#define ANIM_FRAMES_SCROLL  6   // old digit slides down, new enters from above
#define ANIM_FRAMES_EXPLODE 7   // shrink to dot, explosion rings, blank
#define ANIM_FRAMES_DECAY   5   // columns fade out in scattered order, new digit appears
#define ANIM_FRAMES_MELT    8   // pixels fall to heap at bottom, new digit rises from below
#define ANIM_FRAMES_WIPER   6   // vertical bar sweeps left-to-right revealing new digit
#define ANIM_FRAMES_BLINK   6   // old digit flickers out, new digit flickers in
#define ANIM_FRAMES_BLEND   7   // cross-dissolve: old fades out centre-last, new fades in centre-first

// All animation functions write into the frame buffer for `module`.
// old_digit / new_digit: 0-9 or MAX7219_BLANK.
// frame: 1-based, advances each ANIM_TICKS_PER_FRAME render ticks.
// Does NOT flush to hardware — call max7219_refresh_digits() afterwards.
void max7219_anim_scroll  (uint8_t module, uint8_t old_digit, uint8_t new_digit, int frame);
void max7219_anim_explode (uint8_t module, uint8_t old_digit, uint8_t new_digit, int frame);
void max7219_anim_decay   (uint8_t module, uint8_t old_digit, uint8_t new_digit, int frame);
void max7219_anim_melt    (uint8_t module, uint8_t old_digit, uint8_t new_digit, int frame);
void max7219_anim_wiper   (uint8_t module, uint8_t old_digit, uint8_t new_digit, int frame);
void max7219_anim_blink   (uint8_t module, uint8_t old_digit, uint8_t new_digit, int frame);
void max7219_anim_blend   (uint8_t module, uint8_t old_digit, uint8_t new_digit, int frame);

// Draw `count` dots in the top row (row 0) of module 0, leftmost pixels.
// Call after max7219_refresh_digits() — the digit flush does not touch row 0.
void max7219_set_indicator(uint8_t count);

// Enable or disable 180° output rotation at runtime.
// When enabled, module order, row order, and bit order are all reversed so the
// display reads correctly when physically mounted upside-down.
// Default comes from MAX7219_ROTATE_180 in config.h; call this to override.
void max7219_set_rotate(bool rotate);
