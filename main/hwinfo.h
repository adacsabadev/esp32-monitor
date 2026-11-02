#pragma once

#include "esp_http_server.h"

/* Creates the locks and brings up the BLE host. Call after Wi-Fi is started. */
void hwinfo_start(void);

/* Registers /api/hw and /api/hw/scan on the running server. */
void hwinfo_register(httpd_handle_t srv);
