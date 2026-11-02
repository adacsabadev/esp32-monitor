#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "driver/temperature_sensor.h"
#include "wifi_credentials.h"
#include "hwinfo.h"
#include "bench.h"
#include "ulp_riscv.h"
#include "ulp_main.h"

extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t ulp_main_bin_end[] asm("_binary_ulp_main_bin_end");

static const char *TAG = "monitor";

#define MAX_TASKS 40
#define JSON_SIZE 6144

typedef struct {
    char name[configMAX_TASK_NAME_LEN];
    char state;
    unsigned prio;
    uint32_t stack_free;
    float cpu;
} task_info_t;

static task_info_t s_tasks[MAX_TASKS];
static int s_task_count;
static float s_core_load[portNUM_PROCESSORS];
static SemaphoreHandle_t s_lock;
static temperature_sensor_handle_t s_tsens;
static char s_ip[16] = "-";
static bool s_sta_mode;
static bool s_ulp_running;
static uint32_t s_ulp_rate; /* ULP loop iterations per second, as seen from the main CPU */

static void ulp_start(void)
{
    /* The ULP survives chip resets; stop the old instance before overwriting its memory. */
    ulp_riscv_timer_stop();
    ulp_riscv_halt();
    ulp_period_ms = 10;
    ulp_counter = 0;
    if (ulp_riscv_load_binary(ulp_main_bin_start, ulp_main_bin_end - ulp_main_bin_start) != ESP_OK) {
        ESP_LOGE(TAG, "ULP load failed");
        return;
    }
    s_ulp_running = ulp_riscv_run() == ESP_OK;
    ESP_LOGI(TAG, "ULP-RISC-V %s", s_ulp_running ? "running" : "failed to start");
}

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

/* Once per second: per-task and per-core CPU share from FreeRTOS run-time counters. */
static void sampler_task(void *arg)
{
    static TaskStatus_t cur[MAX_TASKS], prev[MAX_TASKS];
    UBaseType_t prev_n = 0;
    uint32_t prev_total = 0;
    uint32_t prev_ulp = ulp_counter;
    static const char states[] = {'R', 'r', 'B', 'S', 'D', 'I'};

    for (;;) {
        uint32_t total;
        UBaseType_t n = uxTaskGetSystemState(cur, MAX_TASKS, &total);
        uint32_t elapsed = total - prev_total;

        if (prev_n && elapsed) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_task_count = 0;
            for (int c = 0; c < portNUM_PROCESSORS; c++) {
                s_core_load[c] = 0;
            }
            for (UBaseType_t i = 0; i < n && s_task_count < MAX_TASKS; i++) {
                uint32_t delta = cur[i].ulRunTimeCounter;
                for (UBaseType_t j = 0; j < prev_n; j++) {
                    if (prev[j].xHandle == cur[i].xHandle) {
                        delta -= prev[j].ulRunTimeCounter;
                        break;
                    }
                }
                float pct = 100.0f * delta / elapsed;
                task_info_t *t = &s_tasks[s_task_count++];
                strlcpy(t->name, cur[i].pcTaskName, sizeof(t->name));
                t->state = cur[i].eCurrentState < sizeof(states) ? states[cur[i].eCurrentState] : '?';
                t->prio = cur[i].uxCurrentPriority;
                t->stack_free = cur[i].usStackHighWaterMark * sizeof(StackType_t);
                t->cpu = pct;
                /* The IDLE task of each core runs whenever that core has nothing else to do. */
                if (strncmp(t->name, "IDLE", 4) == 0 && t->name[4] >= '0' &&
                    t->name[4] < '0' + portNUM_PROCESSORS) {
                    s_core_load[t->name[4] - '0'] = pct > 100.0f ? 0.0f : 100.0f - pct;
                }
            }
            xSemaphoreGive(s_lock);
        }
        memcpy(prev, cur, sizeof(TaskStatus_t) * n);
        uint32_t u = ulp_counter;
        s_ulp_rate = u - prev_ulp;
        prev_ulp = u;
        prev_n = n;
        prev_total = total;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static int jprintf(char *buf, int size, int pos, const char *fmt, ...)
{
    if (pos >= size) {
        return pos;
    }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + pos, size - pos, fmt, ap);
    va_end(ap);
    return w < 0 ? pos : pos + w;
}

static int heap_json(char *b, int sz, int p, const char *key, uint32_t caps)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);
    uint32_t total = info.total_free_bytes + info.total_allocated_bytes;
    return jprintf(b, sz, p,
                   "\"%s\":{\"total\":%u,\"free\":%u,\"min_free\":%u,\"largest\":%u}",
                   key, (unsigned)total, (unsigned)info.total_free_bytes,
                   (unsigned)info.minimum_free_bytes, (unsigned)info.largest_free_block);
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "USB";
    default: return "unknown";
    }
}

static esp_err_t stats_handler(httpd_req_t *req)
{
    char *b = malloc(JSON_SIZE);
    if (!b) {
        return httpd_resp_send_500(req);
    }
    int p = 0;

    float temp = 0;
    temperature_sensor_get_celsius(s_tsens, &temp);

    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash_sz = 0;
    esp_flash_get_size(NULL, &flash_sz);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const esp_app_desc_t *app = esp_app_get_description();

    p = jprintf(b, JSON_SIZE, p, "{\"temp_c\":%.2f,\"cpu_mhz\":%u,\"uptime_s\":%llu,\"reset_reason\":\"%s\",",
                temp, (unsigned)esp_rom_get_cpu_ticks_per_us(),
                (unsigned long long)(esp_timer_get_time() / 1000000), reset_reason_str(esp_reset_reason()));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    p = jprintf(b, JSON_SIZE, p, "\"cores\":[");
    for (int c = 0; c < portNUM_PROCESSORS; c++) {
        p = jprintf(b, JSON_SIZE, p, "%s%.1f", c ? "," : "", s_core_load[c]);
    }
    p = jprintf(b, JSON_SIZE, p, "],");
    p = heap_json(b, JSON_SIZE, p, "heap", MALLOC_CAP_INTERNAL);
    p = jprintf(b, JSON_SIZE, p, ",");
    p = heap_json(b, JSON_SIZE, p, "psram", MALLOC_CAP_SPIRAM);

    p = jprintf(b, JSON_SIZE, p, ",\"tasks\":[");
    for (int i = 0; i < s_task_count; i++) {
        p = jprintf(b, JSON_SIZE, p,
                    "%s{\"name\":\"%s\",\"state\":\"%c\",\"prio\":%u,\"cpu\":%.1f,\"stack_free\":%u}",
                    i ? "," : "", s_tasks[i].name, s_tasks[i].state, s_tasks[i].prio,
                    s_tasks[i].cpu, (unsigned)s_tasks[i].stack_free);
    }
    p = jprintf(b, JSON_SIZE, p, "]");
    xSemaphoreGive(s_lock);

    p = jprintf(b, JSON_SIZE, p,
                ",\"ulp\":{\"running\":%s,\"counter\":%u,\"rate\":%u,\"period_ms\":%u}",
                s_ulp_running && s_ulp_rate ? "true" : "false", (unsigned)ulp_counter,
                (unsigned)s_ulp_rate, (unsigned)ulp_period_ms);

    wifi_ap_record_t ap;
    char rssi[24] = "-";
    if (s_sta_mode && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(rssi, sizeof(rssi), "%d dBm", ap.rssi);
    }
    wifi_sta_list_t stas = {0};
    esp_wifi_ap_get_sta_list(&stas);
    p = jprintf(b, JSON_SIZE, p,
                ",\"wifi\":{\"mode\":\"%s\",\"ip\":\"%s\",\"rssi\":\"%s\",\"clients\":%d,"
                "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}",
                s_sta_mode ? "station" : "access point", s_ip, rssi, stas.num,
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    p = jprintf(b, JSON_SIZE, p,
                ",\"chip\":{\"model\":\"ESP32-S3 rev %u.%u\",\"cores\":%d,\"flash\":\"%u MB\","
                "\"idf\":\"%s\",\"app\":\"%s\",\"built\":\"%s %s\"}}",
                ci.revision / 100, ci.revision % 100, ci.cores, (unsigned)(flash_sz >> 20),
                esp_get_idf_version(), app->project_name, app->date, app->time);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, b, p < JSON_SIZE ? p : JSON_SIZE - 1);
    free(b);
    return err;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static void start_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    httpd_handle_t srv;
    ESP_ERROR_CHECK(httpd_start(&srv, &cfg));
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    httpd_uri_t api = {.uri = "/api/stats", .method = HTTP_GET, .handler = stats_handler};
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &api);
    hwinfo_register(srv);
    bench_register(srv);
}

static void scan_report(void)
{
    wifi_scan_config_t sc = {.show_hidden = true};
    esp_err_t se = esp_wifi_scan_start(&sc, true);
    ESP_LOGI(TAG, "scan -> %s", esp_err_to_name(se));
    uint16_t n = 20;
    wifi_ap_record_t recs[20];
    esp_wifi_scan_get_ap_records(&n, recs);
    ESP_LOGI(TAG, "scan found %u networks:", n);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  '%s' ch%d %d dBm auth %d", recs[i].ssid, recs[i].primary, recs[i].rssi, recs[i].authmode);
    }
}

static void scan_task(void *arg)
{
    scan_report();
    esp_wifi_connect();
    vTaskDelete(NULL);
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xTaskCreate(scan_task, "scan", 4096, NULL, 5, NULL);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected from '%s', reason %d; retrying", WIFI_SSID, d->reason);
        strcpy(s_ip, "-");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "dashboard: http://%s/", s_ip);
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t cfg = {0};

    if (1) {
        s_sta_mode = true;
        esp_netif_create_default_wifi_sta();
        ESP_ERROR_CHECK(esp_wifi_init(&init));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_ip, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL));
        strlcpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid));
        strlcpy((char *)cfg.sta.password, WIFI_PASSWORD, sizeof(cfg.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    } else {
        esp_netif_create_default_wifi_ap();
        ESP_ERROR_CHECK(esp_wifi_init(&init));
        strlcpy((char *)cfg.ap.ssid, "esp32s3-monitor", sizeof(cfg.ap.ssid));
        strlcpy((char *)cfg.ap.password, CONFIG_MON_AP_PASSWORD, sizeof(cfg.ap.password));
        cfg.ap.ssid_len = strlen("esp32s3-monitor");
        cfg.ap.max_connection = 4;
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        strcpy(s_ip, "192.168.4.1");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
        ESP_LOGI(TAG, "join Wi-Fi 'esp32s3-monitor', then open http://192.168.4.1/");
    }
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_lock = xSemaphoreCreateMutex();
    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    ESP_ERROR_CHECK(temperature_sensor_install(&tc, &s_tsens));
    ESP_ERROR_CHECK(temperature_sensor_enable(s_tsens));

    ulp_start();

    xTaskCreate(sampler_task, "sampler", 4096, NULL, 5, NULL);
    wifi_start();
    hwinfo_start();
    bench_start();
    start_server();
}
