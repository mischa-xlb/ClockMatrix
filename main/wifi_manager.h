#pragma once

#include <stdbool.h>

// Initialise NVS, the TCP/IP stack and the default event loop.
// Must be called once before any other wifi_manager_* function.
void wifi_manager_init(void);

// Attempt to connect to WiFi using credentials stored in NVS.
// Returns true on success, false if no credentials found or connection fails.
bool wifi_manager_connect_sta(void);

// Start an open access point + HTTP server so the user can enter WiFi
// credentials via a browser (navigate to http://192.168.4.1).
// This function blocks until valid credentials are submitted,
// then saves them to NVS and reboots the device.
void wifi_manager_start_portal(void);

// Erase stored WiFi credentials from NVS and reboot into the setup portal.
// Call this when the user requests a WiFi reset (e.g. long-press of BTN_WIFI).
void wifi_manager_reset_credentials(void);
