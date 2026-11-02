#pragma once

#include "esp_http_server.h"

/* Creates the lock used to guard benchmark state. Call once at boot. */
void bench_start(void);

/* Registers GET and POST /api/bench on the running server. */
void bench_register(httpd_handle_t srv);
