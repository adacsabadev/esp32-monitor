/*
 * On-demand benchmarks: CPU (Whetstone / Dhrystone-like), memory bandwidth and
 * latency, and cryptography (hardware engines vs. software reference code).
 * Nothing here touches eFuses or keys: only the plain AES, SHA and MPI engines
 * are used (never HMAC / DS / key-manager).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_rom_md5.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/sha_types.h"
#include "aes/esp_aes.h"
#include "aes/esp_aes_gcm.h"
#include "sha/sha_core.h"
#include "mbedtls/bignum.h"
#include "psa/crypto.h"

#include "bench.h"

static const char *TAG = "bench";

#define MAX_ROWS   56
#define CPU_MHZ    CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#define WINDOW_US  300000

typedef struct {
    char t;            /* 'c' cpu, 'm' mem, 'x' crypto */
    char grp[20];
    char name[44];
    double a, b;       /* < 0 means "not available" */
    char unit[12];
    char note[64];
} row_t;

static SemaphoreHandle_t s_mux;
static row_t s_rows[MAX_ROWS];
static int s_nrows;
static bool s_running;
static char s_type;
static char s_prog[48] = "";
static int64_t s_last_ms;

static inline int64_t now_us(void) { return esp_timer_get_time(); }

static void set_prog(const char *p)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    snprintf(s_prog, sizeof(s_prog), "%s", p);
    xSemaphoreGive(s_mux);
}

static void add_row(char t, const char *grp, const char *name, double a, double b,
                    const char *unit, const char *note)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_nrows < MAX_ROWS) {
        row_t *r = &s_rows[s_nrows++];
        r->t = t;
        snprintf(r->grp, sizeof(r->grp), "%s", grp);
        snprintf(r->name, sizeof(r->name), "%s", name);
        r->a = a;
        r->b = b;
        snprintf(r->unit, sizeof(r->unit), "%s", unit);
        snprintf(r->note, sizeof(r->note), "%s", note ? note : "");
    }
    xSemaphoreGive(s_mux);
}

static void drop_rows(char t)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int k = 0;
    for (int i = 0; i < s_nrows; i++) {
        if (s_rows[i].t != t) {
            s_rows[k++] = s_rows[i];
        }
    }
    s_nrows = k;
    xSemaphoreGive(s_mux);
}

/* ------------------------------------------------------------------ CPU */

static volatile double s_sink;
static volatile int s_isink;

#define WHETSTONE(NAME, T, SIN, COS, ATAN, EXP, LOG, SQRT)                                  \
    static void NAME##_pa(T *e, T t, T t2)                                                  \
    {                                                                                       \
        for (int j = 0; j < 6; j++) {                                                       \
            e[0] = (e[0] + e[1] + e[2] - e[3]) * t;                                         \
            e[1] = (e[0] + e[1] - e[2] + e[3]) * t;                                         \
            e[2] = (e[0] - e[1] + e[2] + e[3]) * t;                                         \
            e[3] = (-e[0] + e[1] + e[2] + e[3]) / t2;                                       \
        }                                                                                   \
    }                                                                                       \
    static void NAME##_p3(T x, T y, T *z, T t, T t2)                                        \
    {                                                                                       \
        T x1 = t * (x + y);                                                                 \
        T y1 = t * (x1 + y);                                                                \
        *z = (x1 + y1) / t2;                                                                \
    }                                                                                       \
    static double NAME(int passes)                                                          \
    {                                                                                       \
        const T t = (T)0.499975, t1 = (T)0.50025, t2 = (T)2.0;                              \
        T e[4] = {1, -1, -1, -1};                                                           \
        T x, y, z = 0;                                                                      \
        int j, k, l;                                                                        \
        for (int i = 1; i <= passes; i++) {                                                 \
            for (int n = 0; n < 12; n++) {                                                  \
                e[0] = (e[0] + e[1] + e[2] - e[3]) * t;                                     \
                e[1] = (e[0] + e[1] - e[2] + e[3]) * t;                                     \
                e[2] = (e[0] - e[1] + e[2] + e[3]) * t;                                     \
                e[3] = (-e[0] + e[1] + e[2] + e[3]) * t;                                    \
            }                                                                               \
            for (int n = 0; n < 14; n++) NAME##_pa(e, t, t2);                               \
            j = 1;                                                                          \
            for (int n = 0; n < 345; n++) {                                                 \
                j = (j == 1) ? 2 : 3;                                                       \
                j = (j > 2) ? 0 : 1;                                                        \
                j = (j < 1) ? 1 : 0;                                                        \
            }                                                                               \
            j = 1; k = 2; l = 3;                                                            \
            for (int n = 0; n < 210; n++) {                                                 \
                j = j * (k - j) * (l - k);                                                  \
                k = l * k - (l - j) * k;                                                    \
                l = (l - k) * (k + j);                                                      \
                e[(l - 2) & 3] = (T)(j + k + l);                                            \
                e[(k - 2) & 3] = (T)(j * k * l);                                            \
            }                                                                               \
            x = y = (T)0.5;                                                                 \
            for (int n = 0; n < 32; n++) {                                                  \
                x = t * ATAN(t2 * SIN(x) * COS(x) / (COS(x + y) + COS(x - y) - (T)1.0));    \
                y = t * ATAN(t2 * SIN(y) * COS(y) / (COS(x + y) + COS(x - y) - (T)1.0));    \
            }                                                                               \
            x = y = z = (T)1.0;                                                             \
            for (int n = 0; n < 899; n++) NAME##_p3(x, y, &z, t, t2);                       \
            j = 1; k = 2; l = 3;                                                            \
            e[0] = 1; e[1] = 2; e[2] = 3;                                                   \
            for (int n = 0; n < 616; n++) {                                                 \
                e[j & 3] = e[k & 3];                                                        \
                e[k & 3] = e[l & 3];                                                        \
                e[l & 3] = e[j & 3];                                                        \
            }                                                                               \
            x = (T)0.75;                                                                    \
            for (int n = 0; n < 93; n++) x = SQRT(EXP(LOG(x) / t1));                        \
            s_sink = (double)(x + y + z + e[0] + e[1] + e[2] + e[3]);                       \
        }                                                                                   \
        return passes;                                                                      \
    }

WHETSTONE(whet_f, float, sinf, cosf, atanf, expf, logf, sqrtf)
WHETSTONE(whet_d, double, sin, cos, atan, exp, log, sqrt)

/* One pass is 0.1 MWIPS worth of work in the classic benchmark. */
static double rate_whet_f(void)
{
    int64_t t0 = now_us(), el;
    long passes = 0;
    do {
        passes += (long)whet_f(10);
    } while ((el = now_us() - t0) < 800000);
    return (double)passes * 0.1 / ((double)el / 1e6);
}

static double rate_whet_d(void)
{
    int64_t t0 = now_us(), el;
    long passes = 0;
    do {
        passes += (long)whet_d(4);
    } while ((el = now_us() - t0) < 800000);
    return (double)passes * 0.1 / ((double)el / 1e6);
}

typedef struct {
    int a, b;
    char s[32];
} dstruct_t;

static dstruct_t s_g1, s_g2;
static int s_arr1[50], s_arr2[50][50];

static void dhry_proc(dstruct_t *p, int v)
{
    *p = s_g1;
    p->a = v + 5;
    p->b = p->a * 2 - v;
    s_arr1[v % 50] = p->b;
    s_arr2[v % 50][(v + 1) % 50] = s_arr1[v % 50] + 1;
}

/* Dhrystone-like mix (strings, struct copies, integer math); not the official source. */
static double rate_dhry(void)
{
    char s1[32] = "DHRYSTONE PROGRAM, 1'ST STRING";
    char s2[32];
    int64_t t0 = now_us(), el;
    long loops = 0;
    int v = 3;
    do {
        for (int n = 0; n < 5000; n++) {
            strcpy(s2, "DHRYSTONE PROGRAM, 2'ND STRING");
            int cmp = strcmp(s1, s2);
            dhry_proc(&s_g2, v);
            int i1 = 2, i2 = 3, i3;
            while (i1 < i2) {
                i3 = 5 * i1 - i2;
                i1 += 1;
                v = i3 + cmp;
            }
            v = (v * (i2 + 7) / 3 + s_g2.b) & 0xFFFF;
            s_isink = v;
        }
        loops += 5000;
    } while ((el = now_us() - t0) < 800000);
    return (double)loops / ((double)el / 1e6) / 1757.0;
}

typedef double (*kern_t)(void);

typedef struct {
    kern_t k;
    double res;
    SemaphoreHandle_t done;
} job_t;

static void job_task(void *arg)
{
    job_t *j = arg;
    j->res = j->k();
    xSemaphoreGive(j->done);
    vTaskDelete(NULL);
}

/* Runs the kernel on core 1 alone, or on both cores at once (summed). */
static double run_cpu(kern_t k, bool both)
{
    job_t jobs[2] = {{k, 0, NULL}, {k, 0, NULL}};
    int n = both ? 2 : 1;
    for (int i = 0; i < n; i++) {
        jobs[i].done = xSemaphoreCreateBinary();
        int core = both ? i : 1;
        xTaskCreatePinnedToCore(job_task, "bcpu", 6144, &jobs[i], 4, NULL, core);
    }
    double sum = 0;
    for (int i = 0; i < n; i++) {
        xSemaphoreTake(jobs[i].done, portMAX_DELAY);
        vSemaphoreDelete(jobs[i].done);
        sum += jobs[i].res;
    }
    return sum;
}

static void bench_cpu(void)
{
    static const struct {
        const char *n;
        kern_t k;
        const char *u;
        const char *note;
    } tests[] = {
        {"Whetstone float (FPU)", rate_whet_f, "MWIPS", "single-precision, hardware FPU"},
        {"Whetstone double", rate_whet_d, "MWIPS", "double precision, software on the S3"},
        {"Dhrystone-like", rate_dhry, "DMIPS", "approximate, DMIPS = loops/s / 1757"},
    };
    for (int i = 0; i < 3; i++) {
        char p[48];
        snprintf(p, sizeof(p), "CPU: %s", tests[i].n);
        set_prog(p);
        double one = run_cpu(tests[i].k, false);
        vTaskDelay(pdMS_TO_TICKS(20));
        double two = run_cpu(tests[i].k, true);
        add_row('c', "CPU", tests[i].n, one, two, tests[i].u, tests[i].note);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* --------------------------------------------------------------- memory */

static double bw_read(void *buf, size_t sz)
{
    const volatile uint32_t *p = buf;
    size_t n = sz / 4;
    int64_t t0 = now_us(), el;
    long reps = 0;
    uint32_t s = 0;
    do {
        for (size_t i = 0; i < n; i += 4) {
            s += p[i] + p[i + 1] + p[i + 2] + p[i + 3];
        }
        reps++;
    } while ((el = now_us() - t0) < 150000);
    s_isink = (int)s;
    return (double)sz * reps / el;
}

static double bw_write(void *buf, size_t sz)
{
    volatile uint32_t *p = buf;
    size_t n = sz / 4;
    int64_t t0 = now_us(), el;
    long reps = 0;
    do {
        for (size_t i = 0; i < n; i += 4) {
            p[i] = (uint32_t)i;
            p[i + 1] = (uint32_t)i;
            p[i + 2] = (uint32_t)i;
            p[i + 3] = (uint32_t)i;
        }
        reps++;
    } while ((el = now_us() - t0) < 150000);
    return (double)sz * reps / el;
}

static double bw_copy(void *buf, size_t sz)
{
    uint8_t *a = buf, *b = a + sz / 2;
    int64_t t0 = now_us(), el;
    long reps = 0;
    do {
        memcpy(a, b, sz / 2);
        reps++;
    } while ((el = now_us() - t0) < 150000);
    return (double)(sz / 2) * reps / el;
}

static void bw_set(const char *label, void *buf, size_t sz)
{
    char n[44];
    memset(buf, 0x5A, sz);
    snprintf(n, sizeof(n), "%s read", label);
    add_row('m', "Bandwidth", n, bw_read(buf, sz), -1, "MB/s", "32-bit loads");
    snprintf(n, sizeof(n), "%s write", label);
    add_row('m', "Bandwidth", n, bw_write(buf, sz), -1, "MB/s", "32-bit stores");
    snprintf(n, sizeof(n), "%s copy", label);
    add_row('m', "Bandwidth", n, bw_copy(buf, sz), -1, "MB/s", "memcpy");
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* Pointer chasing over a random single cycle, one node per 64 bytes. */
static double chase_ns(uint32_t *buf, size_t sz)
{
    size_t n = sz / 64;
    for (size_t i = 0; i < n; i++) {
        buf[i * 16] = (uint32_t)i;
    }
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = esp_random() % i;
        uint32_t t = buf[i * 16];
        buf[i * 16] = buf[j * 16];
        buf[j * 16] = t;
    }
    volatile uint32_t idx = 0;
    for (size_t i = 0; i < n; i++) {   /* warm the cache with one lap */
        idx = buf[idx * 16];
    }
    const long steps = 200000;
    uint32_t c0 = esp_cpu_get_cycle_count();
    uint32_t k = idx;
    for (long i = 0; i < steps; i++) {
        k = buf[k * 16];
    }
    uint32_t c1 = esp_cpu_get_cycle_count();
    s_isink = (int)k;
    return (double)(c1 - c0) * 1000.0 / CPU_MHZ / steps;
}

static void lat_set(const char *label, void *buf, size_t sz)
{
    char n[44];
    snprintf(n, sizeof(n), "%s %u KB", label, (unsigned)(sz / 1024));
    double ns = chase_ns(buf, sz);
    char note[64];
    snprintf(note, sizeof(note), "%.1f cycles per dependent load", ns * CPU_MHZ / 1000.0);
    add_row('m', "Latency", n, ns, -1, "ns", note);
    vTaskDelay(1);
}

static void bench_mem(void)
{
    uint32_t *dram = heap_caps_malloc(32 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    set_prog("Memory: DRAM bandwidth");
    if (dram) {
        bw_set("DRAM 32 KB", dram, 32 * 1024);
    }

    uint32_t *ps = heap_caps_malloc(2 * 1024 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ps) {
        set_prog("Memory: PSRAM bandwidth");
        bw_set("PSRAM 16 KB (cached)", ps, 16 * 1024);
        bw_set("PSRAM 2 MB (uncached)", ps, 2 * 1024 * 1024);
    } else {
        add_row('m', "Bandwidth", "PSRAM", -1, -1, "MB/s", "no PSRAM available");
    }

    set_prog("Memory: latency");
    if (dram) {
        static const size_t di[] = {4, 16, 28};
        for (int i = 0; i < 3; i++) {
            lat_set("DRAM", dram, di[i] * 1024);
        }
    }
    heap_caps_free(dram);
    heap_caps_free(ps);

    uint32_t *big = heap_caps_malloc(4 * 1024 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (big) {
        static const size_t pi[] = {4, 16, 32, 64, 256, 1024, 4096};
        for (int i = 0; i < 7; i++) {
            lat_set("PSRAM", big, pi[i] * 1024);
        }
        heap_caps_free(big);
    }
}

/* --------------------------------------------------------------- crypto */

#define BUFSZ 16384

static uint8_t s_sbox[256];
static uint32_t s_k256[64];

static void soft_init(void)
{
    uint8_t p = 1, q = 1;
    do {
        p = p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1B : 0);
        q ^= q << 1;
        q ^= q << 2;
        q ^= q << 4;
        if (q & 0x80) {
            q ^= 0x09;
        }
        uint8_t x = q ^ (uint8_t)((q << 1) | (q >> 7)) ^ (uint8_t)((q << 2) | (q >> 6)) ^
                    (uint8_t)((q << 3) | (q >> 5)) ^ (uint8_t)((q << 4) | (q >> 4));
        s_sbox[p] = x ^ 0x63;
    } while (p != 1);
    s_sbox[0] = 0x63;

    /* SHA-256 round constants: fractional part of the cube roots of the first 64 primes. */
    int cnt = 0;
    for (int c = 2; cnt < 64; c++) {
        bool prime = true;
        for (int d = 2; d * d <= c; d++) {
            if (c % d == 0) {
                prime = false;
                break;
            }
        }
        if (prime) {
            double r = cbrt((double)c);
            s_k256[cnt++] = (uint32_t)((r - floor(r)) * 4294967296.0);
        }
    }
}

static inline uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

typedef struct {
    uint8_t rk[240];
    int nr;
} saes_t;

static void saes_setkey(saes_t *c, const uint8_t *key, int bits)
{
    int nk = bits / 32;
    c->nr = nk + 6;
    memcpy(c->rk, key, nk * 4);
    uint8_t rcon = 1;
    for (int i = nk; i < 4 * (c->nr + 1); i++) {
        uint8_t t[4];
        memcpy(t, &c->rk[(i - 1) * 4], 4);
        if (i % nk == 0) {
            uint8_t u = t[0];
            t[0] = s_sbox[t[1]] ^ rcon;
            t[1] = s_sbox[t[2]];
            t[2] = s_sbox[t[3]];
            t[3] = s_sbox[u];
            rcon = xt(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int b = 0; b < 4; b++) {
                t[b] = s_sbox[t[b]];
            }
        }
        for (int b = 0; b < 4; b++) {
            c->rk[i * 4 + b] = c->rk[(i - nk) * 4 + b] ^ t[b];
        }
    }
}

static void saes_block(const saes_t *c, uint8_t *s)
{
    uint8_t t[16];
    for (int i = 0; i < 16; i++) {
        s[i] ^= c->rk[i];
    }
    for (int r = 1; r <= c->nr; r++) {
        for (int col = 0; col < 4; col++) {
            for (int row = 0; row < 4; row++) {
                t[row + 4 * col] = s_sbox[s[row + 4 * ((col + row) & 3)]];
            }
        }
        if (r < c->nr) {
            for (int col = 0; col < 4; col++) {
                uint8_t *a = &t[col * 4];
                uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                uint8_t b0 = xt(a0), b1 = xt(a1), b2 = xt(a2), b3 = xt(a3);
                s[col * 4 + 0] = b0 ^ b1 ^ a1 ^ a2 ^ a3;
                s[col * 4 + 1] = a0 ^ b1 ^ b2 ^ a2 ^ a3;
                s[col * 4 + 2] = a0 ^ a1 ^ b2 ^ b3 ^ a3;
                s[col * 4 + 3] = b0 ^ a0 ^ a1 ^ a2 ^ b3;
            }
        } else {
            memcpy(s, t, 16);
        }
        for (int i = 0; i < 16; i++) {
            s[i] ^= c->rk[r * 16 + i];
        }
    }
}

static void saes_cbc(const saes_t *c, const uint8_t *iv, size_t len, const uint8_t *in, uint8_t *out)
{
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    for (size_t o = 0; o < len; o += 16) {
        for (int i = 0; i < 16; i++) {
            out[o + i] = in[o + i] ^ prev[i];
        }
        saes_block(c, out + o);
        memcpy(prev, out + o, 16);
    }
}

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha1_soft(const uint8_t *d, size_t len, uint8_t *out)
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t blk[128];
    size_t total = len;
    size_t pos = 0;
    while (pos < len + 9) {
        const uint8_t *src;
        size_t rem = (pos < len) ? len - pos : 0;
        if (rem >= 64) {
            src = d + pos;
        } else {
            memset(blk, 0, sizeof(blk));
            memcpy(blk, d + pos, rem);
            blk[rem] = 0x80;
            size_t nb = (rem + 9 > 64) ? 128 : 64;
            uint64_t bits = (uint64_t)total * 8;
            for (int i = 0; i < 8; i++) {
                blk[nb - 1 - i] = (uint8_t)(bits >> (8 * i));
            }
            for (size_t b = 0; b < nb; b += 64) {
                uint32_t w[80];
                for (int i = 0; i < 16; i++) {
                    w[i] = (uint32_t)blk[b + i * 4] << 24 | blk[b + i * 4 + 1] << 16 | blk[b + i * 4 + 2] << 8 | blk[b + i * 4 + 3];
                }
                for (int i = 16; i < 80; i++) {
                    w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
                }
                uint32_t a = h[0], bb = h[1], c = h[2], dd = h[3], e = h[4];
                for (int i = 0; i < 80; i++) {
                    uint32_t f, k;
                    if (i < 20) { f = (bb & c) | (~bb & dd); k = 0x5A827999; }
                    else if (i < 40) { f = bb ^ c ^ dd; k = 0x6ED9EBA1; }
                    else if (i < 60) { f = (bb & c) | (bb & dd) | (c & dd); k = 0x8F1BBCDC; }
                    else { f = bb ^ c ^ dd; k = 0xCA62C1D6; }
                    uint32_t t = ROL(a, 5) + f + e + k + w[i];
                    e = dd; dd = c; c = ROL(bb, 30); bb = a; a = t;
                }
                h[0] += a; h[1] += bb; h[2] += c; h[3] += dd; h[4] += e;
            }
            break;
        }
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t)src[i * 4] << 24 | src[i * 4 + 1] << 16 | src[i * 4 + 2] << 8 | src[i * 4 + 3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], bb = h[1], c = h[2], dd = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (bb & c) | (~bb & dd); k = 0x5A827999; }
            else if (i < 40) { f = bb ^ c ^ dd; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (bb & c) | (bb & dd) | (c & dd); k = 0x8F1BBCDC; }
            else { f = bb ^ c ^ dd; k = 0xCA62C1D6; }
            uint32_t t = ROL(a, 5) + f + e + k + w[i];
            e = dd; dd = c; c = ROL(bb, 30); bb = a; a = t;
        }
        h[0] += a; h[1] += bb; h[2] += c; h[3] += dd; h[4] += e;
        pos += 64;
    }
    for (int i = 0; i < 5; i++) {
        out[i * 4] = h[i] >> 24; out[i * 4 + 1] = h[i] >> 16; out[i * 4 + 2] = h[i] >> 8; out[i * 4 + 3] = h[i];
    }
}

static void sha256_block(uint32_t *h, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + s_k256[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* len must be a multiple of 64 so the padding block is the only tail. */
static void sha256_soft(const uint8_t *d, size_t len, uint8_t *out)
{
    static const int primes[8] = {2, 3, 5, 7, 11, 13, 17, 19};
    uint32_t h[8];
    for (int i = 0; i < 8; i++) {
        double r = sqrt((double)primes[i]);
        h[i] = (uint32_t)((r - floor(r)) * 4294967296.0);
    }
    for (size_t o = 0; o < len; o += 64) {
        sha256_block(h, d + o);
    }
    uint8_t tail[64] = {0x80};
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) {
        tail[63 - i] = (uint8_t)(bits >> (8 * i));
    }
    sha256_block(h, tail);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = h[i] >> 24; out[i * 4 + 1] = h[i] >> 16; out[i * 4 + 2] = h[i] >> 8; out[i * 4 + 3] = h[i];
    }
}

#define TIMED(body, bytes, result)                              \
    do {                                                        \
        int64_t t0_ = now_us(), el_;                            \
        long it_ = 0;                                           \
        do {                                                    \
            body;                                               \
            it_++;                                              \
        } while ((el_ = now_us() - t0_) < WINDOW_US);           \
        (result) = (double)(bytes) * it_ / (double)el_;         \
    } while (0)

static int rng_cb(void *p, unsigned char *b, size_t n)
{
    (void)p;
    esp_fill_random(b, n);
    return 0;
}

static void rsa_row(int bits, bool with_soft)
{
    mbedtls_mpi n, a, e, x, y, rr1, rr2;
    mbedtls_mpi_init(&n); mbedtls_mpi_init(&a); mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&x); mbedtls_mpi_init(&y);
    mbedtls_mpi_init(&rr1); mbedtls_mpi_init(&rr2);
    char name[44], note[64] = "";
    snprintf(name, sizeof(name), "RSA-%d modexp (%d-bit exponent)", bits, bits);

    bool ok = mbedtls_mpi_fill_random(&n, bits / 8, rng_cb, NULL) == 0 &&
              mbedtls_mpi_set_bit(&n, bits - 1, 1) == 0 && mbedtls_mpi_set_bit(&n, 0, 1) == 0 &&
              mbedtls_mpi_fill_random(&a, bits / 8 - 1, rng_cb, NULL) == 0 &&
              mbedtls_mpi_fill_random(&e, bits / 8, rng_cb, NULL) == 0 &&
              mbedtls_mpi_set_bit(&e, 0, 1) == 0;
    double hw = -1, sw = -1;
    if (ok && mbedtls_mpi_exp_mod(&x, &a, &e, &n, &rr1) == 0) {   /* warm-up also caches RR */
        TIMED(mbedtls_mpi_exp_mod(&x, &a, &e, &n, &rr1), 1, hw);
        hw *= 1e6;
        if (with_soft) {
            int64_t t0 = now_us();
            if (mbedtls_mpi_exp_mod_soft(&y, &a, &e, &n, &rr2) == 0) {
                int64_t el = now_us() - t0;
                sw = 1e6 / (double)el;
                snprintf(note, sizeof(note), "%s", mbedtls_mpi_cmp_mpi(&x, &y) == 0 ? "results match" : "RESULT MISMATCH");
            }
        } else {
            snprintf(note, sizeof(note), "software reference skipped (too slow)");
        }
    } else {
        snprintf(note, sizeof(note), "MPI call failed");
    }
    add_row('x', "Asymmetric", name, sw, hw, "ops/s", note);
    mbedtls_mpi_free(&n); mbedtls_mpi_free(&a); mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&x); mbedtls_mpi_free(&y);
    mbedtls_mpi_free(&rr1); mbedtls_mpi_free(&rr2);
}

static void aes_rows(int bits, uint8_t *in, uint8_t *out, uint8_t *out2)
{
    uint8_t key[32], iv0[16], iv[16];
    esp_fill_random(key, sizeof(key));
    esp_fill_random(iv0, sizeof(iv0));
    char name[44], note[64];

    esp_aes_context hc;
    esp_aes_init(&hc);
    esp_aes_setkey(&hc, key, bits);
    saes_t sc;
    saes_setkey(&sc, key, bits);

    double hw, sw;
    snprintf(name, sizeof(name), "AES-%d-CBC encrypt", bits);
    TIMED((memcpy(iv, iv0, 16), esp_aes_crypt_cbc(&hc, 1, BUFSZ, iv, in, out)), BUFSZ, hw);
    vTaskDelay(1);
    TIMED(saes_cbc(&sc, iv0, BUFSZ, in, out2), BUFSZ, sw);
    memcpy(iv, iv0, 16);
    esp_aes_crypt_cbc(&hc, 1, BUFSZ, iv, in, out);
    snprintf(note, sizeof(note), "%s", memcmp(out, out2, BUFSZ) == 0 ? "software output verified against hardware" : "OUTPUT MISMATCH");
    add_row('x', "Symmetric", name, sw, hw, "MB/s", note);
    vTaskDelay(1);

    snprintf(name, sizeof(name), "AES-%d-CBC decrypt", bits);
    TIMED((memcpy(iv, iv0, 16), esp_aes_crypt_cbc(&hc, 0, BUFSZ, iv, out, out2)), BUFSZ, hw);
    add_row('x', "Symmetric", name, -1, hw, "MB/s", "hardware only (software decrypt not implemented)");
    esp_aes_free(&hc);
    vTaskDelay(1);

    esp_gcm_context gc;
    uint8_t tag[16], giv[12];
    esp_fill_random(giv, sizeof(giv));
    esp_aes_gcm_init(&gc);
    if (esp_aes_gcm_setkey(&gc, 2, key, bits) == 0) {   /* 2 = MBEDTLS_CIPHER_ID_AES */
        snprintf(name, sizeof(name), "AES-%d-GCM encrypt+tag", bits);
        TIMED(esp_aes_gcm_crypt_and_tag(&gc, 1, BUFSZ, giv, 12, NULL, 0, in, out, 16, tag), BUFSZ, hw);
        add_row('x', "Symmetric", name, -1, hw, "MB/s", "hardware only (software GHASH not implemented)");
    }
    esp_aes_gcm_free(&gc);
    vTaskDelay(1);
}

static void chacha_row(uint8_t *in, uint8_t *out)
{
    psa_key_attributes_t at = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t id = 0;
    uint8_t key[32], nonce[12] = {0};
    esp_fill_random(key, sizeof(key));
    psa_set_key_type(&at, PSA_KEY_TYPE_CHACHA20);
    psa_set_key_bits(&at, 256);
    psa_set_key_usage_flags(&at, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&at, PSA_ALG_CHACHA20_POLY1305);
    psa_status_t st = psa_import_key(&at, key, sizeof(key), &id);
    size_t ol = 0;
    double sw = -1;
    if (st == PSA_SUCCESS) {
        const size_t len = BUFSZ - 16;
        TIMED(psa_aead_encrypt(id, PSA_ALG_CHACHA20_POLY1305, nonce, 12, NULL, 0, in, len, out, BUFSZ, &ol), len, sw);
        psa_destroy_key(id);
    }
    add_row('x', "Symmetric", "ChaCha20-Poly1305 encrypt", sw, -1, "MB/s",
            st == PSA_SUCCESS ? "software only (no hardware engine)" : "not enabled in this mbedTLS build");
}

static void ecdsa_rows(void)
{
    psa_key_attributes_t at = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t id = 0;
    psa_algorithm_t alg = PSA_ALG_ECDSA(PSA_ALG_SHA_256);
    psa_set_key_type(&at, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&at, 256);
    psa_set_key_usage_flags(&at, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&at, alg);
    if (psa_generate_key(&at, &id) != PSA_SUCCESS) {
        add_row('x', "Asymmetric", "ECDSA P-256", -1, -1, "ops/s", "not available in this build");
        return;
    }
    uint8_t hash[32], sig[72];
    size_t sl = 0;
    esp_fill_random(hash, sizeof(hash));
    double sg = -1, vf = -1;
    int64_t t0 = now_us();
    long n = 0;
    while (now_us() - t0 < 1000000) {
        if (psa_sign_hash(id, alg, hash, sizeof(hash), sig, sizeof(sig), &sl) != PSA_SUCCESS) {
            break;
        }
        n++;
        vTaskDelay(1);
    }
    if (n) {
        sg = (double)n * 1e6 / (double)(now_us() - t0 - n * 10000);
    }
    n = 0;
    t0 = now_us();
    while (now_us() - t0 < 1000000) {
        if (psa_verify_hash(id, alg, hash, sizeof(hash), sig, sl) != PSA_SUCCESS) {
            break;
        }
        n++;
        vTaskDelay(1);
    }
    if (n) {
        vf = (double)n * 1e6 / (double)(now_us() - t0 - n * 10000);
    }
    add_row('x', "Asymmetric", "ECDSA P-256 sign", sg, -1, "ops/s", "software only (no ECC engine on the S3)");
    add_row('x', "Asymmetric", "ECDSA P-256 verify", vf, -1, "ops/s", "software only");
    psa_destroy_key(id);
}

static void bench_crypto(void)
{
    uint8_t *in = heap_caps_malloc(BUFSZ, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *out = heap_caps_malloc(BUFSZ, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *out2 = heap_caps_malloc(BUFSZ, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!in || !out || !out2) {
        add_row('x', "Symmetric", "buffers", -1, -1, "", "out of memory");
        goto done;
    }
    esp_fill_random(in, BUFSZ);
    soft_init();

    set_prog("Crypto: AES-128");
    aes_rows(128, in, out, out2);
    set_prog("Crypto: AES-256");
    aes_rows(256, in, out, out2);
    set_prog("Crypto: ChaCha20-Poly1305");
    chacha_row(in, out);

    set_prog("Crypto: hashes");
    uint8_t hh[64], hs[64];
    double hw, sw;
    char note[64];

    esp_sha(SHA1, in, BUFSZ, hh);
    sha1_soft(in, BUFSZ, hs);
    snprintf(note, sizeof(note), "%s", memcmp(hh, hs, 20) == 0 ? "software digest verified against hardware" : "DIGEST MISMATCH");
    TIMED(esp_sha(SHA1, in, BUFSZ, hh), BUFSZ, hw);
    vTaskDelay(1);
    TIMED(sha1_soft(in, BUFSZ, hs), BUFSZ, sw);
    add_row('x', "Hash", "SHA-1", sw, hw, "MB/s", note);
    vTaskDelay(1);

    esp_sha(SHA2_256, in, BUFSZ, hh);
    sha256_soft(in, BUFSZ, hs);
    snprintf(note, sizeof(note), "%s", memcmp(hh, hs, 32) == 0 ? "software digest verified against hardware" : "DIGEST MISMATCH");
    TIMED(esp_sha(SHA2_256, in, BUFSZ, hh), BUFSZ, hw);
    vTaskDelay(1);
    TIMED(sha256_soft(in, BUFSZ, hs), BUFSZ, sw);
    add_row('x', "Hash", "SHA-256", sw, hw, "MB/s", note);
    vTaskDelay(1);

    TIMED(esp_sha(SHA2_512, in, BUFSZ, hh), BUFSZ, hw);
    add_row('x', "Hash", "SHA-512", -1, hw, "MB/s", "hardware only (software SHA-512 not implemented)");
    vTaskDelay(1);

    md5_context_t mc;
    TIMED((esp_rom_md5_init(&mc), esp_rom_md5_update(&mc, in, BUFSZ), esp_rom_md5_final(hh, &mc)), BUFSZ, sw);
    add_row('x', "Hash", "MD5", sw, -1, "MB/s", "software only (ROM code, no engine)");
    TIMED(s_isink = (int)esp_rom_crc32_le(0, in, BUFSZ), BUFSZ, sw);
    add_row('x', "Hash", "CRC32", sw, -1, "MB/s", "software only (ROM table code)");
    TIMED(esp_fill_random(out, BUFSZ), BUFSZ, hw);
    add_row('x', "Hash", "Random generator", -1, hw, "MB/s", "hardware RNG, esp_fill_random");
    vTaskDelay(1);

    set_prog("Crypto: RSA 1024");
    rsa_row(1024, true);
    set_prog("Crypto: RSA 2048");
    rsa_row(2048, false);
    set_prog("Crypto: ECDSA P-256");
    ecdsa_rows();

done:
    heap_caps_free(in);
    heap_caps_free(out);
    heap_caps_free(out2);
}

/* ------------------------------------------------------------ plumbing */

static void bench_task(void *arg)
{
    char t = (char)(intptr_t)arg;
    if (t == 'c') {
        bench_cpu();
    } else if (t == 'm') {
        bench_mem();
    } else {
        if (psa_crypto_init() != PSA_SUCCESS) {
            ESP_LOGW(TAG, "psa_crypto_init failed");
        }
        bench_crypto();
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_running = false;
    s_prog[0] = 0;
    s_last_ms = now_us() / 1000;
    xSemaphoreGive(s_mux);
    vTaskDelete(NULL);
}

static esp_err_t post_handler(httpd_req_t *req)
{
    if (httpd_req_get_hdr_value_len(req, "X-Monitor") == 0) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "missing header");
    }
    char q[32], type[12] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "type", type, sizeof(type));
    }
    char t = 0;
    if (!strcmp(type, "cpu")) t = 'c';
    else if (!strcmp(type, "mem")) t = 'm';
    else if (!strcmp(type, "crypto")) t = 'x';
    if (!t) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "type must be cpu, mem or crypto");
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool busy = s_running;
    if (!busy) {
        s_running = true;
        s_type = t;
        snprintf(s_prog, sizeof(s_prog), "starting");
    }
    xSemaphoreGive(s_mux);
    httpd_resp_set_type(req, "application/json");
    if (busy) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"a benchmark is already running\"}");
    }
    drop_rows(t);
    if (xTaskCreatePinnedToCore(bench_task, "bench", 12288, (void *)(intptr_t)t, 3, NULL, 1) != pdPASS) {
        xSemaphoreTake(s_mux, portMAX_DELAY);
        s_running = false;
        xSemaphoreGive(s_mux);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"task create failed\"}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void num(char *b, size_t n, double v)
{
    if (v < 0) {
        snprintf(b, n, "null");
    } else {
        snprintf(b, n, "%.3f", v);
    }
}

static esp_err_t get_handler(httpd_req_t *req)
{
    size_t cap = 14000, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    len += snprintf(buf + len, cap - len, "{\"running\":%s,\"type\":\"%c\",\"prog\":\"%s\",\"cpu_mhz\":%d,\"rows\":[",
                    s_running ? "true" : "false", s_type ? s_type : '-', s_prog, CPU_MHZ);
    for (int i = 0; i < s_nrows && len + 260 < cap; i++) {
        const row_t *r = &s_rows[i];
        char a[24], b[24];
        num(a, sizeof(a), r->a);
        num(b, sizeof(b), r->b);
        len += snprintf(buf + len, cap - len, "%s{\"t\":\"%c\",\"g\":\"%s\",\"n\":\"%s\",\"a\":%s,\"b\":%s,\"u\":\"%s\",\"note\":\"%s\"}",
                        i ? "," : "", r->t, r->grp, r->name, a, b, r->unit, r->note);
    }
    len += snprintf(buf + len, cap - len, "]}");
    xSemaphoreGive(s_mux);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, buf, len);
    free(buf);
    return e;
}

void bench_start(void)
{
    s_mux = xSemaphoreCreateMutex();
}

void bench_register(httpd_handle_t srv)
{
    httpd_uri_t g = {.uri = "/api/bench", .method = HTTP_GET, .handler = get_handler};
    httpd_uri_t p = {.uri = "/api/bench", .method = HTTP_POST, .handler = post_handler};
    httpd_register_uri_handler(srv, &g);
    httpd_register_uri_handler(srv, &p);
}
