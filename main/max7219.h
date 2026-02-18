#pragma once

#include <stdint.h>

// Initialise SPI bus and all MAX7219 modules in the chain.
// Must be called once before any other max7219_* function.
void max7219_init(void);

// Write a single decimal digit (0-9) to one module.
// module: 0 = leftmost panel, (NUM_MODULES-1) = rightmost panel.
// digit:  0-9
void max7219_set_digit(uint8_t module, uint8_t digit);

// Clear (blank) all modules.
void max7219_clear_all(void);

// Set brightness of all modules.
// intensity: 0 (dimmest) to 15 (brightest)
void max7219_set_intensity(uint8_t intensity);
