#pragma once

#include <stdint.h>

typedef enum {
    BTN_EVT_NONE = 0,
    BTN_EVT_WIFI_SHORT,   // WiFi button: short press  → time-set / increment field
    BTN_EVT_WIFI_LONG,    // WiFi button: long press   → reset WiFi credentials
    BTN_EVT_MODE_SHORT,   // Mode button: short press  → next scene / next field
    BTN_EVT_MODE_LONG,    // Mode button: long press   → cancel time-set
} btn_event_t;

// Initialise GPIO buttons and LDR ADC. Starts a background polling task.
void inputs_init(void);

// Returns the next pending button event, or BTN_EVT_NONE if the queue is empty.
btn_event_t inputs_get_event(void);

// Returns the current LDR-derived brightness (BRIGHTNESS_MIN..BRIGHTNESS_MAX).
uint8_t inputs_get_brightness(void);
