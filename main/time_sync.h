#pragma once

#include <stdbool.h>
#include <stdint.h>

// Initialise the SNTP client and set the system timezone.
// Call after WiFi is connected.
void time_sync_init(void);

// Block until the system clock is synchronised with NTP, or timeout_ms elapses.
// Returns true on success, false on timeout.
bool time_sync_wait(uint32_t timeout_ms);
