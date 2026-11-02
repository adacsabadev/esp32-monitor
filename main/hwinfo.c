#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "sdkconfig.h"
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
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "soc/efuse_reg.h"
#include "esp_flash_encrypt.h"
#include "esp_secure_boot.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "soc/soc.h"
#include "soc/system_reg.h"
#include "esp_private/esp_clk.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "hwinfo.h"

static const char *TAG = "hwinfo";

#define WIFI_MAX 40
#define BLE_MAX 48
#define I2C_MAX 32
#define BLE_SCAN_MS 8000

typedef enum { ST_IDLE, ST_RUNNING, ST_DONE, ST_FAILED, ST_UNAVAILABLE } scan_state_t;

static const char *state_name(scan_state_t s)
{
    switch (s) {
    case ST_RUNNING: return "running";
    case ST_DONE: return "done";
    case ST_FAILED: return "failed";
    case ST_UNAVAILABLE: return "unavailable";
    default: return "idle";
    }
}

static SemaphoreHandle_t s_mux;

/* ---------- chunked JSON output ---------- */

#define OUT_CAP 2048

typedef struct {
    httpd_req_t *req;
    char *buf;
    size_t len;
    esp_err_t err;
} out_t;

static void out_flush(out_t *o)
{
    if (o->len && o->err == ESP_OK) {
        o->err = httpd_resp_send_chunk(o->req, o->buf, o->len);
    }
    o->len = 0;
}

static void out_raw(out_t *o, const char *s, size_t n)
{
    while (n) {
        size_t room = OUT_CAP - o->len;
        size_t c = n < room ? n : room;
        memcpy(o->buf + o->len, s, c);
        o->len += c;
        s += c;
        n -= c;
        if (o->len == OUT_CAP) {
            out_flush(o);
        }
    }
}

static void out(out_t *o, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        out_raw(o, tmp, n < (int)sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
    }
}

/* JSON string. Device-supplied text (SSIDs, BLE names) goes through here, so escape everything. */
static void out_str(out_t *o, const char *s)
{
    out_raw(o, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            char e[2] = {'\\', (char)c};
            out_raw(o, e, 2);
        } else if (c < 0x20) {
            char e[8];
            out_raw(o, e, snprintf(e, sizeof(e), "\\u%04x", c));
        } else {
            out_raw(o, s, 1);
        }
    }
    out_raw(o, "\"", 1);
}

static void out_ks(out_t *o, const char *key, const char *val)
{
    out(o, "\"%s\":", key);
    out_str(o, val);
}

static void mac_str(char *d, const uint8_t *m)
{
    snprintf(d, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static int64_t age_s(int64_t t_us)
{
    return t_us ? (esp_timer_get_time() - t_us) / 1000000 : -1;
}

/* ---------- SoC, memory, flash ---------- */

static const char *flash_vendor(uint8_t id)
{
    switch (id) {
    case 0xEF: return "Winbond";
    case 0xC8: return "GigaDevice";
    case 0xC2: return "Macronix";
    case 0x20: return "XMC";
    case 0x9D: return "ISSI";
    case 0x85: return "Puya";
    case 0x0B: return "XTX";
    case 0x68: return "Boya";
    case 0xA1: return "Fudan Micro";
    case 0x5E: return "Zbit";
    case 0x1F: return "Adesto";
    case 0x01: return "Spansion/Cypress";
    default: return "unknown";
    }
}

static void emit_soc(out_t *o)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t pkg = esp_efuse_get_pkg_ver();
    char pk[40], m[18];
    uint8_t mac[6];

    if (pkg == 0) {
        strcpy(pk, "ESP32-S3 (QFN56)");
    } else if (pkg == 1) {
        strcpy(pk, "ESP32-S3-PICO-1 (LGA56)");
    } else {
        snprintf(pk, sizeof(pk), "package id %u", (unsigned)pkg);
    }

    out(o, "\"soc\":{\"model\":\"ESP32-S3\",\"rev\":\"v%u.%u\",", ci.revision / 100, ci.revision % 100);
    out_ks(o, "package", pk);
    out(o, ",\"cores\":%d,\"features\":[", ci.cores);
    bool first = true;
#define FEAT(cond, name) \
    do { if (cond) { out(o, "%s\"%s\"", first ? "" : ",", name); first = false; } } while (0)
    FEAT(ci.features & CHIP_FEATURE_WIFI_BGN, "Wi-Fi 802.11 b/g/n 2.4 GHz");
    FEAT(ci.features & CHIP_FEATURE_BLE, "Bluetooth LE 5");
    FEAT(ci.features & CHIP_FEATURE_BT, "Bluetooth Classic");
    FEAT(ci.features & CHIP_FEATURE_IEEE802154, "IEEE 802.15.4");
    FEAT(ci.features & CHIP_FEATURE_EMB_FLASH, "Embedded flash");
    FEAT(ci.features & CHIP_FEATURE_EMB_PSRAM, "Embedded PSRAM");
#undef FEAT
    out(o, "],\"cpu_mhz\":%u,\"apb_mhz\":%d,\"xtal_mhz\":%d,\"flash_enc\":%s,\"secure_boot\":%s,",
        (unsigned)esp_rom_get_cpu_ticks_per_us(), esp_clk_apb_freq() / 1000000, CONFIG_XTAL_FREQ,
        esp_efuse_is_flash_encryption_enabled() ? "true" : "false", esp_secure_boot_enabled() ? "true" : "false");
    out_ks(o, "idf", esp_get_idf_version());
    out(o, ",\"mac\":{");
    esp_efuse_mac_get_default(mac);
    mac_str(m, mac);
    out_ks(o, "base", m);
    out(o, ",");
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    mac_str(m, mac);
    out_ks(o, "wifi_sta", m);
    out(o, ",");
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    mac_str(m, mac);
    out_ks(o, "wifi_ap", m);
    out(o, ",");
    esp_read_mac(mac, ESP_MAC_BT);
    mac_str(m, mac);
    out_ks(o, "bluetooth", m);
    out(o, "}}");
}

static void emit_memory(out_t *o)
{
    static const struct {
        const char *name;
        uint32_t caps;
    } regions[] = {
        {"Internal RAM (data)", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT},
        {"DMA-capable RAM", MALLOC_CAP_DMA},
        {"RTC RAM", MALLOC_CAP_RTCRAM},
        {"PSRAM (external)", MALLOC_CAP_SPIRAM},
    };
    out(o, "\"mem\":[");
    bool first = true;
    for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
        size_t total = heap_caps_get_total_size(regions[i].caps);
        if (!total) {
            continue;
        }
        out(o, "%s{", first ? "" : ",");
        first = false;
        out_ks(o, "n", regions[i].name);
        out(o, ",\"total\":%u,\"free\":%u,\"min\":%u,\"largest\":%u}", (unsigned)total,
            (unsigned)heap_caps_get_free_size(regions[i].caps),
            (unsigned)heap_caps_get_minimum_free_size(regions[i].caps),
            (unsigned)heap_caps_get_largest_free_block(regions[i].caps));
    }
    out(o, "]");
}

static void emit_flash_psram(out_t *o)
{
    uint32_t size = 0, id = 0;
    uint64_t uid = 0;
    esp_flash_get_size(NULL, &size);
    bool have_id = esp_flash_read_id(NULL, &id) == ESP_OK;
    bool have_uid = esp_flash_read_unique_chip_id(NULL, &uid) == ESP_OK;
    char tmp[40];

    out(o, "\"flash\":{\"size\":%u,", (unsigned)size);
    snprintf(tmp, sizeof(tmp), have_id ? "0x%06X" : "-", (unsigned)id);
    out_ks(o, "jedec", tmp);
    out(o, ",");
    out_ks(o, "vendor", have_id ? flash_vendor(id >> 16) : "-");
    out(o, ",");
    snprintf(tmp, sizeof(tmp), have_uid ? "%016llX" : "-", (unsigned long long)uid);
    out_ks(o, "uid", tmp);
    out(o, ",");
    out_ks(o, "mode", CONFIG_ESPTOOLPY_FLASHMODE);
    out(o, ",");
    out_ks(o, "freq", CONFIG_ESPTOOLPY_FLASHFREQ);
    out(o, "},\"psram\":{\"present\":%s,\"size\":%u,",
        esp_psram_is_initialized() ? "true" : "false", (unsigned)esp_psram_get_size());
#if CONFIG_SPIRAM_MODE_OCT
    out_ks(o, "mode", "Octal (8-line, DDR)");
#else
    out_ks(o, "mode", "Quad (4-line)");
#endif
    out(o, ",\"mhz\":%d}", CONFIG_SPIRAM_SPEED);
}

static const char *part_subtype(esp_partition_type_t t, esp_partition_subtype_t st, char *buf, size_t n)
{
    if (t == ESP_PARTITION_TYPE_APP) {
        if (st == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
            return "factory";
        }
        if (st >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && st < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
            snprintf(buf, n, "ota_%d", st - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
            return buf;
        }
        if (st == ESP_PARTITION_SUBTYPE_APP_TEST) {
            return "test";
        }
    } else if (t == ESP_PARTITION_TYPE_DATA) {
        switch (st) {
        case ESP_PARTITION_SUBTYPE_DATA_OTA: return "ota data";
        case ESP_PARTITION_SUBTYPE_DATA_PHY: return "phy";
        case ESP_PARTITION_SUBTYPE_DATA_NVS: return "nvs";
        case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return "coredump";
        case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS: return "nvs keys";
        case ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM: return "efuse emulation";
        case ESP_PARTITION_SUBTYPE_DATA_FAT: return "fat";
        case ESP_PARTITION_SUBTYPE_DATA_SPIFFS: return "spiffs";
        default: break;
        }
    }
    snprintf(buf, n, "0x%02x", (unsigned)st);
    return buf;
}

static void emit_partitions(out_t *o)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    char sub[16];

    out(o, "\"parts\":[{\"n\":\"bootloader\",\"t\":\"system\",\"s\":\"2nd stage\",\"off\":0,\"size\":32768,\"run\":false,\"enc\":false},"
           "{\"n\":\"partition table\",\"t\":\"system\",\"s\":\"\",\"off\":32768,\"size\":4096,\"run\":false,\"enc\":false}");
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        out(o, ",{");
        out_ks(o, "n", p->label);
        out(o, ",\"t\":\"%s\",", p->type == ESP_PARTITION_TYPE_APP ? "app" : "data");
        out_ks(o, "s", part_subtype(p->type, p->subtype, sub, sizeof(sub)));
        out(o, ",\"off\":%u,\"size\":%u,\"run\":%s,\"enc\":%s}", (unsigned)p->address, (unsigned)p->size,
            run && run->address == p->address ? "true" : "false", p->encrypted ? "true" : "false");
    }
    out(o, "]");
}

/* ---------- peripherals ---------- */

static void emit_peripherals(out_t *o)
{
    static const struct {
        const char *name;
        const char *group;
        uint8_t reg;
        uint32_t mask;
    } p[] = {
        {"UART0", "Serial", 0, SYSTEM_UART_CLK_EN},
        {"UART1", "Serial", 0, SYSTEM_UART1_CLK_EN},
        {"UART2", "Serial", 1, SYSTEM_UART2_CLK_EN},
        {"USB Serial/JTAG", "Serial", 1, SYSTEM_USB_DEVICE_CLK_EN},
        {"USB OTG", "Serial", 0, SYSTEM_USB_CLK_EN},
        {"I2C0", "Buses", 0, SYSTEM_I2C_EXT0_CLK_EN},
        {"I2C1", "Buses", 0, SYSTEM_I2C_EXT1_CLK_EN},
        {"SPI0/1 (flash cache)", "Buses", 0, SYSTEM_SPI01_CLK_EN},
        {"SPI2", "Buses", 0, SYSTEM_SPI2_CLK_EN},
        {"SPI3", "Buses", 0, SYSTEM_SPI3_CLK_EN},
        {"I2S0", "Buses", 0, SYSTEM_I2S0_CLK_EN},
        {"I2S1", "Buses", 0, SYSTEM_I2S1_CLK_EN},
        {"TWAI (CAN)", "Buses", 0, SYSTEM_TWAI_CLK_EN},
        {"SD/MMC host", "Buses", 1, SYSTEM_SDIO_HOST_CLK_EN},
        {"LCD/CAM", "Buses", 1, SYSTEM_LCD_CAM_CLK_EN},
        {"LEDC (PWM)", "Timing and I/O", 0, SYSTEM_LEDC_CLK_EN},
        {"MCPWM0", "Timing and I/O", 0, SYSTEM_PWM0_CLK_EN},
        {"MCPWM1", "Timing and I/O", 0, SYSTEM_PWM1_CLK_EN},
        {"RMT", "Timing and I/O", 0, SYSTEM_RMT_CLK_EN},
        {"Pulse counter", "Timing and I/O", 0, SYSTEM_PCNT_CLK_EN},
        {"Timer group 0", "Timing and I/O", 0, SYSTEM_TIMERGROUP_CLK_EN},
        {"Timer group 1", "Timing and I/O", 0, SYSTEM_TIMERGROUP1_CLK_EN},
        {"System timer", "Timing and I/O", 0, SYSTEM_SYSTIMER_CLK_EN},
        {"SAR ADC", "Analog", 0, SYSTEM_APB_SARADC_CLK_EN},
        {"GDMA", "Core", 1, SYSTEM_DMA_CLK_EN},
        {"eFuse", "Core", 0, SYSTEM_EFUSE_CLK_EN},
        {"AES", "Crypto", 1, SYSTEM_CRYPTO_AES_CLK_EN},
        {"SHA", "Crypto", 1, SYSTEM_CRYPTO_SHA_CLK_EN},
        {"RSA", "Crypto", 1, SYSTEM_CRYPTO_RSA_CLK_EN},
        {"Digital signature", "Crypto", 1, SYSTEM_CRYPTO_DS_CLK_EN},
        {"HMAC", "Crypto", 1, SYSTEM_CRYPTO_HMAC_CLK_EN},
    };
    uint32_t r[2] = {REG_READ(SYSTEM_PERIP_CLK_EN0_REG), REG_READ(SYSTEM_PERIP_CLK_EN1_REG)};

    out(o, "\"periph\":[");
    for (size_t i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
        out(o, "%s{", i ? "," : "");
        out_ks(o, "n", p[i].name);
        out(o, ",");
        out_ks(o, "g", p[i].group);
        out(o, ",\"on\":%s}", (r[p[i].reg] & p[i].mask) ? "true" : "false");
    }
    out(o, "]");
}

/* ---------- GPIO ---------- */

static const char *pin_note(int n)
{
    if (n == CONFIG_MON_I2C_SDA) {
        return "I2C scan SDA (Hardware page)";
    }
    if (n == CONFIG_MON_I2C_SCL) {
        return "I2C scan SCL (Hardware page)";
    }
    switch (n) {
    case 0: return "strapping: low at reset = download mode";
    case 3: return "strapping: JTAG signal source";
    case 19: return "USB D- (USB Serial/JTAG)";
    case 20: return "USB D+ (USB Serial/JTAG)";
    case 26: return "SPI flash/PSRAM bus (reserved)";
    case 27: case 28: case 29: case 30: case 31: case 32:
        return "SPI flash bus (reserved)";
    case 33: case 34: case 35: case 36: case 37:
#if CONFIG_SPIRAM_MODE_OCT
        return "octal PSRAM bus (reserved)";
#else
        return "octal flash/PSRAM pin (free with quad parts)";
#endif
    case 39: return "JTAG MTCK";
    case 40: return "JTAG MTDO";
    case 41: return "JTAG MTDI";
    case 42: return "JTAG MTMS";
    case 43: return "U0TXD (console UART)";
    case 44: return "U0RXD (console UART)";
    case 45: return "strapping: VDD_SPI voltage";
    case 46: return "strapping: ROM log / boot";
    case 48: return "board dependent (RGB LED on many devkits)";
    default: return "";
    }
}

static void emit_gpio(out_t *o)
{
    out(o, "\"gpio\":[");
    bool first = true;
    for (int n = 0; n < GPIO_NUM_MAX; n++) {
        gpio_io_config_t c;
        if (!GPIO_IS_VALID_GPIO(n) || gpio_get_io_config((gpio_num_t)n, &c) != ESP_OK) {
            continue;
        }
        out(o, "%s{\"n\":%d,\"lv\":%d,\"ie\":%d,\"oe\":%d,\"pu\":%d,\"pd\":%d,\"od\":%d,\"fun\":%d,\"sig\":%d,\"drv\":%d,",
            first ? "" : ",", n, gpio_get_level((gpio_num_t)n), c.ie, c.oe, c.pu, c.pd, c.od, c.fun_sel,
            c.sig_out, c.drv);
        first = false;
        out_ks(o, "note", pin_note(n));
        out(o, "}");
    }
    out(o, "]");
}

/* ---------- network, Wi-Fi radio, Bluetooth controller ---------- */

static void emit_netifs(out_t *o)
{
    out(o, "\"net\":[");
    bool first = true;
    for (esp_netif_t *n = esp_netif_next_unsafe(NULL); n; n = esp_netif_next_unsafe(n)) {
        esp_netif_ip_info_t ip = {0};
        esp_netif_dns_info_t dns = {0};
        esp_netif_dhcp_status_t dh = ESP_NETIF_DHCP_INIT;
        uint8_t mac[6] = {0};
        char m[18], s[20];
        const char *host = NULL;
        esp_netif_get_ip_info(n, &ip);
        esp_netif_get_mac(n, mac);
        esp_netif_get_hostname(n, &host);
        esp_netif_get_dns_info(n, ESP_NETIF_DNS_MAIN, &dns);
        bool has_dhcpc = esp_netif_dhcpc_get_status(n, &dh) == ESP_OK;

        out(o, "%s{", first ? "" : ",");
        first = false;
        out_ks(o, "name", esp_netif_get_desc(n) ? esp_netif_get_desc(n) : "?");
        out(o, ",\"up\":%s,", esp_netif_is_netif_up(n) ? "true" : "false");
        mac_str(m, mac);
        out_ks(o, "mac", m);
        out(o, ",");
        snprintf(s, sizeof(s), IPSTR, IP2STR(&ip.ip));
        out_ks(o, "ip", s);
        out(o, ",");
        snprintf(s, sizeof(s), IPSTR, IP2STR(&ip.netmask));
        out_ks(o, "mask", s);
        out(o, ",");
        snprintf(s, sizeof(s), IPSTR, IP2STR(&ip.gw));
        out_ks(o, "gw", s);
        out(o, ",");
        snprintf(s, sizeof(s), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        out_ks(o, "dns", s);
        out(o, ",");
        out_ks(o, "host", host ? host : "");
        out(o, ",\"dhcp_client\":%s}", has_dhcpc && dh == ESP_NETIF_DHCP_STARTED ? "true" : "false");
    }
    out(o, "]");
}

static const char *auth_name(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN: return "Open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA3_PSK: return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3-PSK";
    case WIFI_AUTH_WAPI_PSK: return "WAPI-PSK";
    case WIFI_AUTH_OWE: return "OWE";
    default: return "Enterprise/other";
    }
}

static void emit_wifi_radio(out_t *o)
{
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        out(o, "\"wifi\":{\"init\":false}");
        return;
    }
    char tmp[64];
    wifi_country_t cc = {0};
    uint8_t proto = 0, prim = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    wifi_bandwidth_t bw = WIFI_BW20;
    wifi_ps_type_t ps = WIFI_PS_NONE;
    int8_t pw = 0;
    esp_wifi_get_country(&cc);
    esp_wifi_get_protocol(WIFI_IF_STA, &proto);
    esp_wifi_get_channel(&prim, &sec);
    esp_wifi_get_bandwidth(WIFI_IF_STA, &bw);
    esp_wifi_get_ps(&ps);
    esp_wifi_get_max_tx_power(&pw);

    out(o, "\"wifi\":{\"init\":true,");
    out_ks(o, "mode", mode == WIFI_MODE_STA ? "station" : mode == WIFI_MODE_AP ? "access point" : "station + AP");
    snprintf(tmp, sizeof(tmp), "%s%s%s%s", (proto & WIFI_PROTOCOL_11B) ? "b " : "",
             (proto & WIFI_PROTOCOL_11G) ? "g " : "", (proto & WIFI_PROTOCOL_11N) ? "n " : "",
             (proto & WIFI_PROTOCOL_LR) ? "LR" : "");
    out(o, ",");
    out_ks(o, "protocols", tmp);
    snprintf(tmp, sizeof(tmp), "%c%c%c, ch %d-%d, max %d dBm, %s", cc.cc[0] ? cc.cc[0] : '?',
             cc.cc[1] ? cc.cc[1] : '?', cc.cc[2] && cc.cc[2] > ' ' ? cc.cc[2] : ' ', cc.schan,
             cc.schan + cc.nchan - 1, cc.max_tx_power, cc.policy == WIFI_COUNTRY_POLICY_AUTO ? "auto" : "manual");
    out(o, ",");
    out_ks(o, "country", tmp);
    out(o, ",");
    out_ks(o, "bandwidth", bw == WIFI_BW40 ? "HT40" : "HT20");
    out(o, ",");
    out_ks(o, "power_save", ps == WIFI_PS_NONE ? "off" : ps == WIFI_PS_MIN_MODEM ? "min modem" : "max modem");
    snprintf(tmp, sizeof(tmp), "%.2f dBm", pw / 4.0);
    out(o, ",");
    out_ks(o, "tx_power", tmp);
    out(o, ",\"channel\":%d,\"ap\":", prim);

    wifi_ap_record_t ap;
    if ((mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        char b[18], ssid[33];
        memcpy(ssid, ap.ssid, 32);
        ssid[32] = 0;
        mac_str(b, ap.bssid);
        out(o, "{");
        out_ks(o, "ssid", ssid);
        out(o, ",");
        out_ks(o, "bssid", b);
        out(o, ",\"rssi\":%d,\"ch\":%d,", ap.rssi, ap.primary);
        out_ks(o, "auth", auth_name(ap.authmode));
        out(o, "}");
    } else {
        out(o, "null");
    }

    out(o, ",\"clients\":[");
    wifi_sta_list_t stas = {0};
    if ((mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) && esp_wifi_ap_get_sta_list(&stas) == ESP_OK) {
        for (int i = 0; i < stas.num; i++) {
            char m[18];
            mac_str(m, stas.sta[i].mac);
            out(o, "%s{", i ? "," : "");
            out_ks(o, "mac", m);
            out(o, ",\"rssi\":%d}", stas.sta[i].rssi);
        }
    }
    out(o, "]}");
}

/* ---------- BLE scan ---------- */

typedef struct {
    uint8_t addr[6];
    uint8_t type;
    uint8_t evt;
    int8_t rssi;
    int8_t tx_pwr;
    bool has_tx;
    bool has_company;
    uint16_t company;
    uint8_t n_uuid;
    uint16_t uuid[4];
    uint32_t hits;
    char name[32];
} ble_dev_t;

static struct {
    volatile scan_state_t st;
    bool synced;
    uint8_t own_type;
    char own_addr[18];
    char err[48];
    int64_t t_done;
    int n;
    ble_dev_t dev[BLE_MAX];
} s_ble;

static const char *company_name(uint16_t id)
{
    switch (id) {
    case 0x004C: return "Apple";
    case 0x0006: return "Microsoft";
    case 0x0075: return "Samsung";
    case 0x00E0: return "Google";
    case 0x0087: return "Garmin";
    case 0x02E5: return "Espressif";
    case 0x0059: return "Nordic Semiconductor";
    case 0x000F: return "Broadcom";
    case 0x009E: return "Bose";
    case 0x012D: return "Sony";
    case 0x038F: return "Xiaomi";
    case 0x027D: return "Huawei";
    case 0x0499: return "Ruuvi";
    case 0x0171: return "Amazon";
    default: return "";
    }
}

static void ble_on_adv(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    bool parsed = ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    ble_dev_t *dev = NULL;
    for (int i = 0; i < s_ble.n; i++) {
        if (s_ble.dev[i].type == d->addr.type && !memcmp(s_ble.dev[i].addr, d->addr.val, 6)) {
            dev = &s_ble.dev[i];
            break;
        }
    }
    if (!dev && s_ble.n < BLE_MAX) {
        dev = &s_ble.dev[s_ble.n++];
        memset(dev, 0, sizeof(*dev));
        memcpy(dev->addr, d->addr.val, 6);
        dev->type = d->addr.type;
        dev->evt = d->event_type;
    }
    if (dev) {
        dev->rssi = d->rssi;
        dev->hits++;
        if (d->event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
            dev->evt = d->event_type;
        }
        if (parsed) {
            if (f.name && f.name_len) {
                size_t l = f.name_len < sizeof(dev->name) - 1 ? f.name_len : sizeof(dev->name) - 1;
                memcpy(dev->name, f.name, l);
                dev->name[l] = 0;
            }
            if (f.tx_pwr_lvl_is_present) {
                dev->has_tx = true;
                dev->tx_pwr = f.tx_pwr_lvl;
            }
            if (f.mfg_data && f.mfg_data_len >= 2) {
                dev->has_company = true;
                dev->company = f.mfg_data[0] | (f.mfg_data[1] << 8);
            }
            if (f.uuids16 && f.num_uuids16) {
                dev->n_uuid = f.num_uuids16 < 4 ? f.num_uuids16 : 4;
                for (int i = 0; i < dev->n_uuid; i++) {
                    dev->uuid[i] = f.uuids16[i].value;
                }
            }
        }
    }
    xSemaphoreGive(s_mux);
}

static int ble_gap_cb(struct ble_gap_event *ev, void *arg)
{
    if (ev->type == BLE_GAP_EVENT_DISC) {
        ble_on_adv(&ev->disc);
    } else if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        s_ble.t_done = esp_timer_get_time();
        s_ble.st = ST_DONE;
    }
    return 0;
}

static void ble_on_sync(void)
{
    uint8_t addr[6] = {0};
    if (ble_hs_util_ensure_addr(0) == 0 && ble_hs_id_infer_auto(0, &s_ble.own_type) == 0 &&
        ble_hs_id_copy_addr(s_ble.own_type, addr, NULL) == 0) {
        /* ble_hs_id_copy_addr gives the address in little-endian order */
        uint8_t be[6] = {addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]};
        mac_str(s_ble.own_addr, be);
        s_ble.synced = true;
        ESP_LOGI(TAG, "BLE host synced, address %s", s_ble.own_addr);
    } else {
        ESP_LOGE(TAG, "BLE address setup failed");
    }
}

static void ble_on_reset(int reason)
{
    s_ble.synced = false;
    ESP_LOGW(TAG, "BLE host reset, reason %d", reason);
}

static void ble_host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ble_scan_start(void)
{
    if (!s_ble.synced) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ble.st == ST_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    struct ble_gap_disc_params p = {0};
    p.passive = 0;
    p.filter_duplicates = 0;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_ble.n = 0;
    s_ble.st = ST_RUNNING;
    xSemaphoreGive(s_mux);

    int rc = ble_gap_disc(s_ble.own_type, BLE_SCAN_MS, &p, ble_gap_cb, NULL);
    if (rc != 0) {
        snprintf(s_ble.err, sizeof(s_ble.err), "ble_gap_disc rc=%d", rc);
        s_ble.st = ST_FAILED;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void ble_init(void)
{
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) {
        snprintf(s_ble.err, sizeof(s_ble.err), "%s", esp_err_to_name(e));
        s_ble.st = ST_UNAVAILABLE;
        ESP_LOGE(TAG, "nimble_port_init: %s", s_ble.err);
        return;
    }
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    nimble_port_freertos_init(ble_host_task);
}

static void emit_bt(out_t *o)
{
    out(o, "\"bt\":{\"ble\":%s,\"synced\":%s,",
        s_ble.st == ST_UNAVAILABLE ? "false" : "true", s_ble.synced ? "true" : "false");
    out_ks(o, "addr", s_ble.synced ? s_ble.own_addr : "-");
    out(o, ",");
    out_ks(o, "err", s_ble.err);
    out(o, ",\"scan_ms\":%d}", BLE_SCAN_MS);
}

/* ---------- Wi-Fi scan ---------- */

static struct {
    volatile scan_state_t st;
    char err[40];
    int64_t t_done;
    uint16_t n;
    wifi_ap_record_t *aps;
} s_wifi;

static int rssi_desc(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

static void wifi_scan_task(void *arg)
{
    wifi_scan_config_t sc = {.show_hidden = true};
    wifi_ap_record_t *recs = NULL;
    uint16_t n = 0;
    esp_err_t err = esp_wifi_scan_start(&sc, true);

    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_num(&n);
        if (n > WIFI_MAX) {
            n = WIFI_MAX;
        }
        recs = calloc(n ? n : 1, sizeof(*recs));
        if (recs) {
            err = esp_wifi_scan_get_ap_records(&n, recs);
        } else {
            err = ESP_ERR_NO_MEM;
            esp_wifi_clear_ap_list();
        }
    }
    if (err == ESP_OK && n > 1) {
        qsort(recs, n, sizeof(*recs), rssi_desc);
    }

    xSemaphoreTake(s_mux, portMAX_DELAY);
    free(s_wifi.aps);
    s_wifi.aps = err == ESP_OK ? recs : NULL;
    s_wifi.n = err == ESP_OK ? n : 0;
    if (err != ESP_OK) {
        free(recs);
        snprintf(s_wifi.err, sizeof(s_wifi.err), "%s", esp_err_to_name(err));
    }
    s_wifi.t_done = esp_timer_get_time();
    s_wifi.st = err == ESP_OK ? ST_DONE : ST_FAILED;
    xSemaphoreGive(s_mux);
    vTaskDelete(NULL);
}

static esp_err_t wifi_scan_start(void)
{
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK || s_wifi.st == ST_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    s_wifi.st = ST_RUNNING;
    if (xTaskCreate(wifi_scan_task, "hw_wifi", 4096, NULL, 5, NULL) != pdPASS) {
        s_wifi.st = ST_FAILED;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---------- I2C scan ---------- */

static struct {
    volatile scan_state_t st;
    char err[40];
    int64_t t_done;
    int n;
    uint8_t addr[I2C_MAX];
} s_i2c;

static const char *i2c_hint(uint8_t a)
{
    switch (a) {
    case 0x0D: return "QMC5883L magnetometer";
    case 0x1E: return "HMC5883L magnetometer";
    case 0x23: case 0x5C: return "BH1750 light sensor";
    case 0x29: return "VL53L0X / TSL2561";
    case 0x3C: case 0x3D: return "SSD1306 / SH1106 OLED";
    case 0x40: return "INA219 / HTU21D / Si7021 / PCA9685";
    case 0x44: case 0x45: return "SHT3x temperature/humidity";
    case 0x51: return "PCF8563 RTC / EEPROM";
    case 0x53: return "ADXL345 accelerometer / EEPROM";
    case 0x5A: return "CCS811 / MLX90614";
    case 0x68: return "MPU6050 / DS3231 / DS1307";
    case 0x69: return "MPU6050 (alt address)";
    case 0x6B: return "LSM6DS / IMU";
    case 0x76: return "BMP280 / BME280 / BME680";
    case 0x77: return "BMP180 / BME280 / BME680";
    default: break;
    }
    if (a >= 0x20 && a <= 0x27) {
        return "PCF8574 / MCP23008 I/O expander";
    }
    if (a >= 0x38 && a <= 0x3F) {
        return "PCF8574A expander / touch controller";
    }
    if (a >= 0x48 && a <= 0x4B) {
        return "ADS1115 / TMP102 / PCF8591";
    }
    if (a >= 0x50 && a <= 0x57) {
        return "AT24Cxx EEPROM";
    }
    return "";
}

static void i2c_scan_task(void *arg)
{
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_MON_I2C_SDA,
        .scl_io_num = CONFIG_MON_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    uint8_t found[I2C_MAX];
    int n = 0;
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);

    if (err == ESP_OK) {
        for (uint8_t a = 0x08; a <= 0x77 && n < I2C_MAX; a++) {
            if (i2c_master_probe(bus, a, 30) == ESP_OK) {
                found[n++] = a;
            }
        }
        i2c_del_master_bus(bus);
    }

    xSemaphoreTake(s_mux, portMAX_DELAY);
    memcpy(s_i2c.addr, found, n);
    s_i2c.n = n;
    if (err != ESP_OK) {
        snprintf(s_i2c.err, sizeof(s_i2c.err), "%s", esp_err_to_name(err));
    }
    s_i2c.t_done = esp_timer_get_time();
    s_i2c.st = err == ESP_OK ? ST_DONE : ST_FAILED;
    xSemaphoreGive(s_mux);
    vTaskDelete(NULL);
}

static esp_err_t i2c_scan_start(void)
{
    if (s_i2c.st == ST_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    s_i2c.st = ST_RUNNING;
    if (xTaskCreate(i2c_scan_task, "hw_i2c", 4096, NULL, 5, NULL) != pdPASS) {
        s_i2c.st = ST_FAILED;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---------- HTTP handlers ---------- */

static bool out_begin(out_t *o, httpd_req_t *req)
{
    memset(o, 0, sizeof(*o));
    o->req = req;
    o->buf = malloc(OUT_CAP);
    if (!o->buf) {
        httpd_resp_send_500(req);
        return false;
    }
    httpd_resp_set_type(req, "application/json");
    return true;
}

static esp_err_t out_end(out_t *o)
{
    out_flush(o);
    if (o->err == ESP_OK) {
        o->err = httpd_resp_send_chunk(o->req, NULL, 0);
    }
    free(o->buf);
    return o->err;
}

static void emit_efuse(out_t *o)
{
    /* Read-only: only reads eFuse shadow registers, never programs anything. Key blocks' data is never emitted. */
#define F(x) {#x, ESP_EFUSE_##x}
    static const struct { const char *n; const esp_efuse_desc_t **d; } f[] = {
        F(WR_DIS), F(RD_DIS), F(SPI_BOOT_CRYPT_CNT), F(SECURE_BOOT_EN), F(SECURE_BOOT_KEY_REVOKE0),
        F(SECURE_BOOT_KEY_REVOKE1), F(SECURE_BOOT_KEY_REVOKE2), F(SECURE_BOOT_AGGRESSIVE_REVOKE),
        F(KEY_PURPOSE_0), F(KEY_PURPOSE_1), F(KEY_PURPOSE_2), F(KEY_PURPOSE_3), F(KEY_PURPOSE_4),
        F(KEY_PURPOSE_5), F(DIS_DOWNLOAD_MODE), F(ENABLE_SECURITY_DOWNLOAD), F(DIS_FORCE_DOWNLOAD),
        F(DIS_DOWNLOAD_MANUAL_ENCRYPT), F(DIS_DOWNLOAD_ICACHE), F(DIS_DOWNLOAD_DCACHE), F(DIS_ICACHE),
        F(DIS_DCACHE), F(DIS_DIRECT_BOOT), F(DIS_USB_JTAG), F(DIS_USB_SERIAL_JTAG), F(DIS_PAD_JTAG),
        F(SOFT_DIS_JTAG), F(DIS_USB_OTG), F(DIS_USB_OTG_DOWNLOAD_MODE), F(DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE),
        F(DIS_USB_SERIAL_JTAG_ROM_PRINT), F(USB_PHY_SEL), F(USB_EXCHG_PINS), F(DIS_TWAI), F(DIS_APP_CPU),
        F(UART_PRINT_CONTROL), F(FORCE_SEND_RESUME), F(WDT_DELAY_SEL), F(VDD_SPI_XPD), F(VDD_SPI_TIEH),
        F(VDD_SPI_FORCE), F(PIN_POWER_SELECTION), F(FLASH_TPUW), F(FLASH_PAGE_SIZE), F(FLASH_CAP),
        F(PSRAM_CAP), F(PSRAM_VENDOR), F(PSRAM_TEMP), F(PKG_VERSION), F(BLK_VERSION_MAJOR),
        F(BLK_VERSION_MINOR), F(DISABLE_WAFER_VERSION_MAJOR),
    };
#undef F
    out(o, "\"efuse\":{\"fields\":[");
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        uint32_t v = 0;
        int bits = esp_efuse_get_field_size(f[i].d);
        if (bits <= 0 || bits > 32 || esp_efuse_read_field_blob(f[i].d, &v, bits) != ESP_OK) {
            continue;
        }
        out(o, "%s{", i ? "," : "");
        out_ks(o, "n", f[i].n);
        out(o, ",\"bits\":%d,\"v\":%u}", bits, (unsigned)v);
    }
    out(o, "],\"blocks\":[");
    static const struct { const char *n; uint32_t off; int words; } b[] = {
        {"BLOCK0 (config)", 0x2c, 6}, {"BLOCK1 (MAC/SPI pads/calib)", 0x44, 6},
        {"BLOCK2 (system data)", 0x5c, 8}, {"BLOCK3 (user data)", 0x7c, 8},
    };
    for (int i = 0; i < 4; i++) {
        out(o, "%s{", i ? "," : "");
        out_ks(o, "n", b[i].n);
        out(o, ",\"w\":[");
        for (int w = 0; w < b[i].words; w++) {
            out(o, "%s%u", w ? "," : "", (unsigned)REG_READ(DR_REG_EFUSE_BASE + b[i].off + 4 * w));
        }
        out(o, "]}");
    }
    out(o, "]}");
}

static esp_err_t hw_handler(httpd_req_t *req)
{
    out_t o;
    if (!out_begin(&o, req)) {
        return ESP_FAIL;
    }
    out(&o, "{");
    emit_soc(&o);
    out(&o, ",");
    emit_efuse(&o);
    out(&o, ",");
    emit_memory(&o);
    out(&o, ",");
    emit_flash_psram(&o);
    out(&o, ",");
    emit_partitions(&o);
    out(&o, ",");
    emit_peripherals(&o);
    out(&o, ",");
    emit_gpio(&o);
    out(&o, ",");
    emit_netifs(&o);
    out(&o, ",");
    emit_wifi_radio(&o);
    out(&o, ",");
    emit_bt(&o);
    out(&o, ",\"i2c\":{\"sda\":%d,\"scl\":%d}}", CONFIG_MON_I2C_SDA, CONFIG_MON_I2C_SCL);
    return out_end(&o);
}

/* Results are copied out under the lock so a slow client can't stall the BLE host task. */
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_ap_record_t *aps = malloc(sizeof(*aps) * WIFI_MAX);
    ble_dev_t *devs = malloc(sizeof(*devs) * BLE_MAX);
    uint8_t i2c_addr[I2C_MAX];
    int n_ap, n_ble, n_i2c;
    scan_state_t st_wifi, st_ble, st_i2c;
    char err_wifi[40], err_ble[48], err_i2c[40];
    int64_t t_wifi, t_ble, t_i2c;
    out_t o;

    if (!aps || !devs) {
        free(aps);
        free(devs);
        return httpd_resp_send_500(req);
    }

    xSemaphoreTake(s_mux, portMAX_DELAY);
    st_wifi = s_wifi.st;
    n_ap = s_wifi.n;
    t_wifi = s_wifi.t_done;
    memcpy(err_wifi, s_wifi.err, sizeof(err_wifi));
    if (s_wifi.aps) {
        memcpy(aps, s_wifi.aps, sizeof(*aps) * n_ap);
    }
    st_ble = s_ble.st;
    n_ble = s_ble.n;
    t_ble = s_ble.t_done;
    memcpy(err_ble, s_ble.err, sizeof(err_ble));
    memcpy(devs, s_ble.dev, sizeof(*devs) * n_ble);
    st_i2c = s_i2c.st;
    n_i2c = s_i2c.n;
    t_i2c = s_i2c.t_done;
    memcpy(err_i2c, s_i2c.err, sizeof(err_i2c));
    memcpy(i2c_addr, s_i2c.addr, sizeof(i2c_addr));
    xSemaphoreGive(s_mux);

    if (!out_begin(&o, req)) {
        free(aps);
        free(devs);
        return ESP_FAIL;
    }

    out(&o, "{\"wifi\":{\"state\":\"%s\",", state_name(st_wifi));
    out_ks(&o, "err", err_wifi);
    out(&o, ",\"age_s\":%lld,\"aps\":[", (long long)age_s(t_wifi));
    for (int i = 0; i < n_ap; i++) {
        char b[18], ssid[33];
        memcpy(ssid, aps[i].ssid, 32);
        ssid[32] = 0;
        mac_str(b, aps[i].bssid);
        out(&o, "%s{", i ? "," : "");
        out_ks(&o, "ssid", ssid);
        out(&o, ",");
        out_ks(&o, "bssid", b);
        out(&o, ",\"ch\":%d,\"rssi\":%d,", aps[i].primary, aps[i].rssi);
        out_ks(&o, "auth", auth_name(aps[i].authmode));
        out(&o, ",\"b\":%d,\"g\":%d,\"n\":%d,\"lr\":%d,\"wps\":%d}", aps[i].phy_11b, aps[i].phy_11g,
            aps[i].phy_11n, aps[i].phy_lr, aps[i].wps);
    }

    out(&o, "]},\"ble\":{\"state\":\"%s\",", state_name(st_ble));
    out_ks(&o, "err", err_ble);
    out(&o, ",\"age_s\":%lld,\"devs\":[", (long long)age_s(t_ble));
    for (int i = 0; i < n_ble; i++) {
        char m[18];
        mac_str(m, devs[i].addr);
        out(&o, "%s{", i ? "," : "");
        out_ks(&o, "addr", m);
        out(&o, ",\"atype\":%d,\"rssi\":%d,\"hits\":%u,\"conn\":%s,", devs[i].type, devs[i].rssi,
            (unsigned)devs[i].hits, devs[i].evt <= BLE_HCI_ADV_RPT_EVTYPE_DIR_IND ? "true" : "false");
        out_ks(&o, "name", devs[i].name);
        if (devs[i].has_tx) {
            out(&o, ",\"txp\":%d", devs[i].tx_pwr);
        }
        out(&o, ",");
        out_ks(&o, "vendor", devs[i].has_company ? company_name(devs[i].company) : "");
        out(&o, ",\"company\":%d,\"uuids\":[", devs[i].has_company ? devs[i].company : -1);
        for (int u = 0; u < devs[i].n_uuid; u++) {
            out(&o, "%s%u", u ? "," : "", devs[i].uuid[u]);
        }
        out(&o, "]}");
    }

    out(&o, "]},\"i2c\":{\"state\":\"%s\",", state_name(st_i2c));
    out_ks(&o, "err", err_i2c);
    out(&o, ",\"age_s\":%lld,\"sda\":%d,\"scl\":%d,\"devs\":[", (long long)age_s(t_i2c), CONFIG_MON_I2C_SDA,
        CONFIG_MON_I2C_SCL);
    for (int i = 0; i < n_i2c; i++) {
        out(&o, "%s{\"addr\":%d,", i ? "," : "", i2c_addr[i]);
        out_ks(&o, "hint", i2c_hint(i2c_addr[i]));
        out(&o, "}");
    }
    out(&o, "]}}");

    free(aps);
    free(devs);
    return out_end(&o);
}

static esp_err_t scan_post_handler(httpd_req_t *req)
{
    /* Custom header forces a CORS preflight, so other web pages can't trigger scans from a browser. */
    if (httpd_req_get_hdr_value_len(req, "X-Monitor") == 0) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "missing header");
    }
    char q[48], type[16] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "type", type, sizeof(type));
    }
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (!strcmp(type, "wifi")) {
        e = wifi_scan_start();
    } else if (!strcmp(type, "ble")) {
        e = ble_scan_start();
    } else if (!strcmp(type, "i2c")) {
        e = i2c_scan_start();
    }
    char r[80];
    snprintf(r, sizeof(r), "{\"ok\":%s,\"err\":\"%s\"}", e == ESP_OK ? "true" : "false", esp_err_to_name(e));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, r);
}

void hwinfo_start(void)
{
    s_mux = xSemaphoreCreateMutex();
    ble_init();
}

void hwinfo_register(httpd_handle_t srv)
{
    httpd_uri_t hw = {.uri = "/api/hw", .method = HTTP_GET, .handler = hw_handler};
    httpd_uri_t sg = {.uri = "/api/hw/scan", .method = HTTP_GET, .handler = scan_get_handler};
    httpd_uri_t sp = {.uri = "/api/hw/scan", .method = HTTP_POST, .handler = scan_post_handler};
    httpd_register_uri_handler(srv, &hw);
    httpd_register_uri_handler(srv, &sg);
    httpd_register_uri_handler(srv, &sp);
}
