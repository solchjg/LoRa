// Site connectivity scanner on a Heltec WiFi LoRa 32 V3 (docs/site-scanner-plan.md,
// phase 1). At one spot it tests WiFi (scan, then with credentials connect and
// one HTTPS request) and LoRaWAN on TTN and on Helium (join, then ladder rounds
// of LinkCheckReq uplinks SF7 -> SF10), gives each a verdict (scan_core), keeps
// the last 20 spots in NVS, and is run and read from a Web Bluetooth page
// (web/index.html) over the shared ble_setup service, or with the PRG key.
//
// PRG: short press = run every test on the current spot; long press (1 s) = new spot.

#include <RadioLib.h>

#include <cinttypes>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>

extern "C" {
#include "ble_setup.h"
}
#include "driver/gpio.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "oled.h"
#include "scan_core.h"
#include "sdkconfig.h"

#define FW_VERSION "0.1.0"

// ---- Heltec WiFi LoRa 32 V3 pins (Heltec's V3 schematic) ------------------------

enum {
    PIN_LORA_NSS = 8,
    PIN_LORA_SCK = 9,
    PIN_LORA_MOSI = 10,
    PIN_LORA_MISO = 11,
    PIN_LORA_RST = 12,
    PIN_LORA_BUSY = 13,
    PIN_LORA_DIO1 = 14,
    PIN_OLED_SDA = 17,
    PIN_OLED_SCL = 18,
    PIN_OLED_RST = 21,
    PIN_LED = 35,
    PIN_VEXT = 36,  // low powers the OLED
    PIN_KEY = 0,    // PRG, low when pressed
};
static const float TCXO_V = 1.8f;  // the V3's SX1262 runs from a TCXO on DIO3

// ---- State shared between the scan task and the Bluetooth callbacks --------------

#define SPOTS 20

static SemaphoreHandle_t lock;
static sc_spot_t cur;          // the current spot, also in NVS slot cur_slot
static int cur_slot = -1;      // -1: no spot yet
static int n_spots;            // stored spots (at most SPOTS)
static int next_slot;          // where the next new spot goes
static char step[48] = "idle";
static bool running;
static char dev_name[16];

typedef struct {
    char ttn_deveui[17], ttn_joineui[17], ttn_appkey[33], ttn_nwkkey[33];
    char hel_deveui[17], hel_joineui[17], hel_appkey[33];
    char wifi_ssid[33], wifi_pass[65], wifi_url[201];
    uint32_t keys_gen;  // bumped when a network's keys change: join again
} cfg_t;
static cfg_t cfg;

enum cmd_type { CMD_RUN, CMD_NEW_SPOT };
typedef struct {
    cmd_type type;
    unsigned tests;
} cmd_t;
static QueueHandle_t cmdq;

static void lock_take(void) { xSemaphoreTake(lock, portMAX_DELAY); }
static void lock_give(void) { xSemaphoreGive(lock); }

// ---- NVS --------------------------------------------------------------------------

static void nvs_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
}

static void cfg_load(void)
{
    nvs_handle_t h;
    memset(&cfg, 0, sizeof cfg);
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    nvs_str(h, "ttn_dev", cfg.ttn_deveui, sizeof cfg.ttn_deveui);
    nvs_str(h, "ttn_join", cfg.ttn_joineui, sizeof cfg.ttn_joineui);
    nvs_str(h, "ttn_app", cfg.ttn_appkey, sizeof cfg.ttn_appkey);
    nvs_str(h, "ttn_nwk", cfg.ttn_nwkkey, sizeof cfg.ttn_nwkkey);
    nvs_str(h, "hel_dev", cfg.hel_deveui, sizeof cfg.hel_deveui);
    nvs_str(h, "hel_join", cfg.hel_joineui, sizeof cfg.hel_joineui);
    nvs_str(h, "hel_app", cfg.hel_appkey, sizeof cfg.hel_appkey);
    nvs_str(h, "wifi_ssid", cfg.wifi_ssid, sizeof cfg.wifi_ssid);
    nvs_str(h, "wifi_pass", cfg.wifi_pass, sizeof cfg.wifi_pass);
    nvs_str(h, "wifi_url", cfg.wifi_url, sizeof cfg.wifi_url);
    nvs_close(h);
}

static void cfg_save(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, "ttn_dev", cfg.ttn_deveui);
    nvs_set_str(h, "ttn_join", cfg.ttn_joineui);
    nvs_set_str(h, "ttn_app", cfg.ttn_appkey);
    nvs_set_str(h, "ttn_nwk", cfg.ttn_nwkkey);
    nvs_set_str(h, "hel_dev", cfg.hel_deveui);
    nvs_set_str(h, "hel_join", cfg.hel_joineui);
    nvs_set_str(h, "hel_app", cfg.hel_appkey);
    nvs_set_str(h, "wifi_ssid", cfg.wifi_ssid);
    nvs_set_str(h, "wifi_pass", cfg.wifi_pass);
    nvs_set_str(h, "wifi_url", cfg.wifi_url);
    nvs_commit(h);
    nvs_close(h);
}

static bool spot_load(int slot, sc_spot_t *s)
{
    nvs_handle_t h;
    if (nvs_open("spots", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char key[8];
    snprintf(key, sizeof key, "s%02d", slot);
    size_t len = sizeof *s;
    bool ok = nvs_get_blob(h, key, s, &len) == ESP_OK && len == sizeof *s && s->magic == SC_SPOT_MAGIC;
    nvs_close(h);
    return ok;
}

static void spot_save(int slot, const sc_spot_t *s)
{
    nvs_handle_t h;
    if (nvs_open("spots", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char key[8];
    snprintf(key, sizeof key, "s%02d", slot);
    nvs_set_blob(h, key, s, sizeof *s);
    nvs_set_u8(h, "cur", (uint8_t)slot);
    nvs_set_u8(h, "next", (uint8_t)next_slot);
    nvs_set_u8(h, "count", (uint8_t)n_spots);
    nvs_commit(h);
    nvs_close(h);
}

static void spots_load(void)
{
    nvs_handle_t h;
    if (nvs_open("spots", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t c = 0, n = 0, k = 0;
    nvs_get_u8(h, "cur", &c);
    nvs_get_u8(h, "next", &n);
    nvs_get_u8(h, "count", &k);
    nvs_close(h);
    n_spots = k <= SPOTS ? k : SPOTS;
    next_slot = n < SPOTS ? n : 0;
    if (n_spots > 0 && c < SPOTS && spot_load(c, &cur)) {
        cur_slot = c;
    }
}

static void spots_clear(void)
{
    nvs_handle_t h;
    if (nvs_open("spots", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    n_spots = 0;
    next_slot = 0;
    cur_slot = -1;
}

// A new, empty spot in the next slot (the oldest goes when all 20 are used).
// Called with the lock held.
static void spot_new(void)
{
    sc_spot_init(&cur);
    cur_slot = next_slot;
    next_slot = (next_slot + 1) % SPOTS;
    if (n_spots < SPOTS) {
        n_spots++;
    }
    snprintf(cur.spot, sizeof cur.spot, "spot %d", cur_slot);
    spot_save(cur_slot, &cur);
}

// ---- Screen -----------------------------------------------------------------------

static void show(void)
{
    lock_take();
    sc_spot_t s = cur;
    int slot = cur_slot;
    char st[48];
    strcpy(st, step);
    lock_give();
    if (slot < 0) {
        oled_line(0, "%s  no spot", dev_name);
    } else {
        oled_line(0, "%d %s", slot, s.spot);
    }
    oled_line(1, "%s", st);
    oled_line(2, "WiFi   %s", slot < 0 ? "-" : sc_verdict_name(sc_wifi_verdict(&s.wifi)));
    oled_line(3, "TTN    %s", slot < 0 ? "-" : sc_verdict_name(sc_lorawan_verdict(&s.ttn)));
    oled_line(4, "Helium %s", slot < 0 ? "-" : sc_verdict_name(sc_lorawan_verdict(&s.helium)));
}

static void set_step(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_step(const char *fmt, ...)
{
    char t[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    lock_take();
    strcpy(step, t);
    lock_give();
    printf("scan: %s\n", t);
    show();
}

// ---- WiFi test ------------------------------------------------------------------------

#define WIFI_GOT_IP BIT0
#define WIFI_FAILED BIT1
static EventGroupHandle_t wifi_ev;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(wifi_ev, WIFI_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_ev, WIFI_GOT_IP);
    }
}

static void wifi_init(void)
{
    wifi_ev = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wc);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void https_check(const char *url, sc_wifi_t *w)
{
    esp_http_client_config_t hc = {};
    hc.url = url;
    hc.timeout_ms = 10000;
    hc.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    w->https_tried = true;
    w->https_status = -1;
    if (c == NULL) {
        return;
    }
    int64_t t0 = now_ms();
    if (esp_http_client_perform(c) == ESP_OK) {
        w->https_status = (int16_t)esp_http_client_get_status_code(c);
        w->https_ok = w->https_status >= 200 && w->https_status < 400;
    }
    w->https_ms = (uint32_t)(now_ms() - t0);
    esp_http_client_cleanup(c);
    printf("scan: https %s -> %d in %" PRIu32 " ms\n", url, w->https_status, w->https_ms);
}

static void test_wifi(sc_wifi_t *w)
{
    memset(w, 0, sizeof *w);
    set_step("WiFi: scanning");
    wifi_scan_config_t sc = {};
    sc.show_hidden = true;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        set_step("WiFi: scan failed");
        return;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    static wifi_ap_record_t recs[30];
    uint16_t got = sizeof recs / sizeof recs[0];
    esp_wifi_scan_get_ap_records(&got, recs);
    // Strongest first.
    for (int i = 1; i < got; i++) {
        for (int j = i; j > 0 && recs[j].rssi > recs[j - 1].rssi; j--) {
            wifi_ap_record_t t = recs[j];
            recs[j] = recs[j - 1];
            recs[j - 1] = t;
        }
    }
    w->scanned = true;
    w->n_heard = n;
    for (int i = 0; i < got; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        printf("scan: ap,%s,%d,%u,%s\n", ssid, recs[i].rssi, recs[i].primary,
               recs[i].authmode == WIFI_AUTH_OPEN ? "open" : "password");
        if (i < SC_APS) {
            sc_ap_t *a = &w->aps[w->n_aps++];
            snprintf(a->ssid, sizeof a->ssid, "%s", ssid);
            a->rssi = recs[i].rssi;
            a->channel = recs[i].primary;
            a->open = recs[i].authmode == WIFI_AUTH_OPEN;
        }
    }

    lock_take();
    cfg_t c = cfg;
    lock_give();
    if (c.wifi_ssid[0] == '\0') {
        return;
    }
    strcpy(w->target, c.wifi_ssid);
    for (int i = 0; i < got; i++) {
        if (strcmp((const char *)recs[i].ssid, c.wifi_ssid) == 0) {
            w->target_rssi = recs[i].rssi;
            break;
        }
    }
    wifi_config_t wcfg = {};
    // The driver's fields hold 32 and 64 bytes, unterminated when full.
    memcpy(wcfg.sta.ssid, c.wifi_ssid, strnlen(c.wifi_ssid, sizeof wcfg.sta.ssid));
    memcpy(wcfg.sta.password, c.wifi_pass, strnlen(c.wifi_pass, sizeof wcfg.sta.password));
    wcfg.sta.threshold.authmode = c.wifi_pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    for (int a = 1; a <= CONFIG_SCAN_WIFI_ATTEMPTS; a++) {
        set_step("WiFi: connect %d/%d", a, CONFIG_SCAN_WIFI_ATTEMPTS);
        w->attempts++;
        xEventGroupClearBits(wifi_ev, WIFI_GOT_IP | WIFI_FAILED);
        int64_t t0 = now_ms();
        esp_wifi_connect();
        EventBits_t b = xEventGroupWaitBits(wifi_ev, WIFI_GOT_IP | WIFI_FAILED, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(20000));
        if (b & WIFI_GOT_IP) {
            if (w->connects == 0) {
                w->connect_ms = (uint32_t)(now_ms() - t0);
            }
            w->connects++;
            if (c.wifi_url[0] && !w->https_tried) {
                set_step("WiFi: HTTPS check");
                https_check(c.wifi_url, w);
            }
        }
        printf("scan: wifi connect %d %s in %" PRId64 " ms\n", a, (b & WIFI_GOT_IP) ? "ok" : "failed",
               now_ms() - t0);
        xEventGroupClearBits(wifi_ev, WIFI_FAILED);
        esp_wifi_disconnect();
        xEventGroupWaitBits(wifi_ev, WIFI_FAILED, pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ---- LoRaWAN test ------------------------------------------------------------------------

static EspHal *hal = new EspHal(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI);
static SX1262 radio = new Module(hal, PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);
static bool radio_ok;

typedef struct {
    const char *name;
    const char *nonces_key;
    LoRaWANNode *node;
    bool confirmed;      // Helium: confirmed uplinks, so an ACK shows the uplink was heard
    bool joined;
    uint32_t joined_gen;  // cfg.keys_gen when it joined
} net_t;

static LoRaWANNode ttn_node(&radio, &US915, CONFIG_SCAN_SUBBAND);
static LoRaWANNode hel_node(&radio, &US915, CONFIG_SCAN_SUBBAND);
static net_t nets[2] = {
    {"TTN", "ttn_nonces", &ttn_node, false, false, 0},
    {"Helium", "hel_nonces", &hel_node, true, false, 0},
};

// US915 125 kHz data rates: SF index i (0 = SF7) is DR 3 - i.
static uint8_t dr_of(int i) { return (uint8_t)(3 - i); }

static void nonces_load(net_t *n)
{
    nvs_handle_t h;
    if (nvs_open("lorawan", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t buf[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
    size_t len = sizeof buf;
    if (nvs_get_blob(h, n->nonces_key, buf, &len) == ESP_OK && len == sizeof buf) {
        n->node->setBufferNonces(buf);  // refused (keys changed): a fresh count
    }
    nvs_close(h);
}

static void nonces_save(net_t *n)
{
    nvs_handle_t h;
    if (nvs_open("lorawan", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, n->nonces_key, n->node->getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
    nvs_commit(h);
    nvs_close(h);
}

static void test_lorawan(net_t *n, sc_lorawan_t *l)
{
    sc_lorawan_clear(l);
    lock_take();
    cfg_t c = cfg;
    lock_give();
    bool ttn = n == &nets[0];
    const char *dev = ttn ? c.ttn_deveui : c.hel_deveui;
    const char *join = ttn ? c.ttn_joineui : c.hel_joineui;
    const char *app = ttn ? c.ttn_appkey : c.hel_appkey;
    const char *nwk = ttn ? c.ttn_nwkkey : "";
    uint8_t dev_b[8], join_b[8], app_k[16], nwk_k[16];
    if (!radio_ok || !sc_hex(dev, dev_b, 8) || !sc_hex(app, app_k, 16)) {
        set_step("%s: %s", n->name, radio_ok ? "no keys" : "no radio");
        return;  // Not tested
    }
    if (!sc_hex(join, join_b, 8)) {
        memset(join_b, 0, sizeof join_b);
    }
    bool has_nwk = sc_hex(nwk, nwk_k, 16);
    l->tried = true;

    if (!n->joined || n->joined_gen != c.keys_gen) {
        uint64_t dev_eui = 0, join_eui = 0;
        for (int i = 0; i < 8; i++) {
            dev_eui = (dev_eui << 8) | dev_b[i];
            join_eui = (join_eui << 8) | join_b[i];
        }
        n->joined = false;
        if (n->node->beginOTAA(join_eui, dev_eui, has_nwk ? nwk_k : NULL, app_k) != RADIOLIB_ERR_NONE) {
            set_step("%s: bad keys", n->name);
            return;
        }
        nonces_load(n);
        for (int t = 1; t <= CONFIG_SCAN_JOIN_TRIES && !n->joined; t++) {
            set_step("%s: join %d/%d", n->name, t, CONFIG_SCAN_JOIN_TRIES);
            l->join_tries = (uint8_t)t;
            gpio_set_level((gpio_num_t)PIN_LED, 1);
            int16_t st = n->node->activateOTAA();
            gpio_set_level((gpio_num_t)PIN_LED, 0);
            nonces_save(n);
            printf("scan: %s join %d -> %d\n", n->name, t, st);
            if (st == RADIOLIB_LORAWAN_NEW_SESSION || st == RADIOLIB_LORAWAN_SESSION_RESTORED) {
                n->joined = true;
                n->joined_gen = c.keys_gen;
            } else if (t < CONFIG_SCAN_JOIN_TRIES) {
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
        }
        if (!n->joined) {
            return;
        }
        n->node->setADR(false);
        n->node->setTxPower(CONFIG_SCAN_TX_DBM);
    }
    l->joined = true;
    if (l->join_tries == 0) {
        l->join_tries = 1;  // joined earlier this power-on
    }

    for (int r = 1; r <= CONFIG_SCAN_ROUNDS; r++) {
        l->rounds = (uint8_t)r;
        for (int i = 0; i >= 0;) {
            set_step("%s: round %d SF%d", n->name, r, sc_sf(i));
            bool answered = false, lc = false;
            uint8_t margin = 0, gw = 0;
            int16_t rssi = 0, snr = 0;
            if (n->node->setDatarate(dr_of(i)) == RADIOLIB_ERR_NONE) {
                n->node->sendMacCommandReq(RADIOLIB_LORAWAN_MAC_LINK_CHECK);
                uint8_t up[3] = {(uint8_t)cur_slot, (uint8_t)r, dr_of(i)};
                gpio_set_level((gpio_num_t)PIN_LED, 1);
                int16_t st = n->node->sendReceive(up, sizeof up, 1, n->confirmed);
                gpio_set_level((gpio_num_t)PIN_LED, 0);
                if (st > 0) {
                    answered = true;
                    lc = n->node->getMacLinkCheckAns(&margin, &gw) == RADIOLIB_ERR_NONE;
                    rssi = (int16_t)radio.getRSSI();
                    snr = (int16_t)(radio.getSNR() * 10);
                }
                printf("scan: %s,round %d,SF%d,%s,%d,%d,%d,%.1f\n", n->name, r, sc_sf(i),
                       st > 0 ? (lc ? "lc" : "ack") : st == 0 ? "none" : "error", lc ? margin : -1,
                       lc ? gw : -1, rssi, snr / 10.0);
            }
            sc_lorawan_add(l, i, answered, lc, margin, gw, rssi, snr);
            i = sc_ladder_next(i, answered);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

// ---- The scan task ---------------------------------------------------------------------------

static void commit(void (*apply)(sc_spot_t *, const void *), const void *part)
{
    lock_take();
    apply(&cur, part);
    spot_save(cur_slot, &cur);
    lock_give();
    show();
}

static void put_wifi(sc_spot_t *s, const void *p) { s->wifi = *(const sc_wifi_t *)p; }
static void put_ttn(sc_spot_t *s, const void *p) { s->ttn = *(const sc_lorawan_t *)p; }
static void put_hel(sc_spot_t *s, const void *p) { s->helium = *(const sc_lorawan_t *)p; }

static void run_tests(unsigned tests)
{
    lock_take();
    if (cur_slot < 0) {
        spot_new();
    }
    running = true;
    lock_give();
    if (tests & SC_T_WIFI) {
        static sc_wifi_t w;
        test_wifi(&w);
        commit(put_wifi, &w);
    }
    if (tests & SC_T_TTN) {
        static sc_lorawan_t l;
        test_lorawan(&nets[0], &l);
        commit(put_ttn, &l);
    }
    if (tests & SC_T_HELIUM) {
        static sc_lorawan_t l;
        test_lorawan(&nets[1], &l);
        commit(put_hel, &l);
    }
    lock_take();
    running = false;
    lock_give();
    set_step("done");
}

static void scan_task(void *arg)
{
    cmd_t c;
    for (;;) {
        if (xQueueReceive(cmdq, &c, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (c.type == CMD_NEW_SPOT) {
            lock_take();
            spot_new();
            lock_give();
            set_step("new spot");
        } else {
            run_tests(c.tests);
        }
    }
}

// ---- Bluetooth --------------------------------------------------------------------------------

// field=value&, the value percent-encoded.
static void form_put(char *out, size_t cap, size_t *len, const char *name, const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = *len;
    int w = snprintf(out + n, n < cap ? cap - n : 0, "%s%s=", n ? "&" : "", name);
    if (w < 0) {
        return;
    }
    n += (size_t)w;
    for (const unsigned char *p = (const unsigned char *)value; *p && n + 3 < cap; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[n++] = (char)*p;
        } else {
            out[n++] = '%';
            out[n++] = hex[*p >> 4];
            out[n++] = hex[*p & 15];
        }
    }
    if (n >= cap) {
        n = cap - 1;
    }
    out[n] = '\0';
    *len = n;
}

static size_t ble_read_info(char *out, size_t cap)
{
    char num[12];
    size_t n = 0;
    out[0] = '\0';
    lock_take();
    form_put(out, cap, &n, "name", dev_name);
    form_put(out, cap, &n, "fw", FW_VERSION);
    form_put(out, cap, &n, "state", running ? "running" : "idle");
    form_put(out, cap, &n, "step", step);
    snprintf(num, sizeof num, "%d", cur_slot);
    form_put(out, cap, &n, "spot", num);
    snprintf(num, sizeof num, "%d", n_spots);
    form_put(out, cap, &n, "spots", num);
    form_put(out, cap, &n, "history", "spots");
    lock_give();
    return n;
}

static size_t ble_read_settings(char *out, size_t cap)
{
    char t[24];
    size_t n = 0;
    out[0] = '\0';
    lock_take();
    if (cur_slot >= 0) {
        form_put(out, cap, &n, "site", cur.site);
        form_put(out, cap, &n, "spot", cur.spot);
        form_put(out, cap, &n, "notes", cur.notes);
        snprintf(t, sizeof t, "%u", cur.height_cm);
        form_put(out, cap, &n, "height_cm", cur.height_cm ? t : "");
        form_put(out, cap, &n, "enclosure", cur.enclosure == 1 ? "yes" : cur.enclosure == 0 ? "no" : "");
        form_put(out, cap, &n, "wifi", sc_verdict_name(sc_wifi_verdict(&cur.wifi)));
        form_put(out, cap, &n, "ttn", sc_verdict_name(sc_lorawan_verdict(&cur.ttn)));
        form_put(out, cap, &n, "helium", sc_verdict_name(sc_lorawan_verdict(&cur.helium)));
    }
    // Keys and passwords are never read back: "<name>_set=1" says one is stored.
    form_put(out, cap, &n, "ttn_deveui", cfg.ttn_deveui);
    form_put(out, cap, &n, "ttn_joineui", cfg.ttn_joineui);
    form_put(out, cap, &n, "ttn_appkey_set", cfg.ttn_appkey[0] ? "1" : "0");
    form_put(out, cap, &n, "ttn_nwkkey_set", cfg.ttn_nwkkey[0] ? "1" : "0");
    form_put(out, cap, &n, "hel_deveui", cfg.hel_deveui);
    form_put(out, cap, &n, "hel_joineui", cfg.hel_joineui);
    form_put(out, cap, &n, "hel_appkey_set", cfg.hel_appkey[0] ? "1" : "0");
    form_put(out, cap, &n, "wifi_ssid", cfg.wifi_ssid);
    form_put(out, cap, &n, "wifi_pass_set", cfg.wifi_pass[0] ? "1" : "0");
    form_put(out, cap, &n, "wifi_url", cfg.wifi_url);
    lock_give();
    return n;
}

// A hex key field: absent keeps the stored value, empty clears it, otherwise
// exactly `bytes` bytes of hex. False when invalid.
static bool key_field(const char *body, size_t len, const char *name, char *dst, size_t bytes, bool *changed)
{
    char v[80];
    int n = sc_form_get(body, len, name, v, sizeof v);
    if (n < 0) {
        return true;
    }
    uint8_t tmp[16];
    if (n > 0 && !sc_hex(v, tmp, bytes)) {
        return false;
    }
    if (strcmp(dst, v) != 0) {
        *changed = true;
    }
    strcpy(dst, v);
    return true;
}

static bool text_field(const char *body, size_t len, const char *name, char *dst, size_t cap)
{
    char v[256];
    int n = sc_form_get(body, len, name, v, sizeof v);
    if (n < 0) {
        return true;
    }
    if ((size_t)n >= cap) {
        return false;
    }
    strcpy(dst, v);
    return true;
}

static void ble_on_apply(const char *body, size_t len, char *result, size_t cap)
{
    char cmd[16];
    if (sc_form_get(body, len, "cmd", cmd, sizeof cmd) < 0) {
        snprintf(result, cap, "invalid=cmd");
        return;
    }
    lock_take();
    bool busy = running;
    if (strcmp(cmd, "spot") == 0) {
        char nw[4];
        bool fresh = sc_form_get(body, len, "new", nw, sizeof nw) >= 0 && strcmp(nw, "1") == 0;
        if (fresh && busy) {
            snprintf(result, cap, "busy");
        } else {
            sc_spot_t t = cur;
            if (fresh || cur_slot < 0) {
                sc_spot_init(&t);
            }
            const char *bad = sc_parse_spot(body, len, &t);
            if (bad) {
                snprintf(result, cap, "invalid=%s", bad);
            } else {
                if (fresh || cur_slot < 0) {
                    spot_new();
                }
                // Keep the test results the scan task may be writing.
                t.wifi = cur.wifi;
                t.ttn = cur.ttn;
                t.helium = cur.helium;
                cur = t;
                spot_save(cur_slot, &cur);
                snprintf(result, cap, "ok");
            }
        }
    } else if (strcmp(cmd, "run") == 0) {
        unsigned tests = sc_parse_tests(body, len);
        if (tests == 0) {
            snprintf(result, cap, "invalid=tests");
        } else if (busy || uxQueueMessagesWaiting(cmdq) > 0) {
            snprintf(result, cap, "busy");
        } else {
            cmd_t c = {CMD_RUN, tests};
            xQueueSend(cmdq, &c, 0);
            running = true;  // until the task picks it up and finishes
            snprintf(result, cap, "ok");
        }
    } else if (strcmp(cmd, "keys") == 0) {
        cfg_t t = cfg;
        bool ttn_changed = false, hel_changed = false;
        const char *bad = !key_field(body, len, "ttn_deveui", t.ttn_deveui, 8, &ttn_changed) ? "ttn_deveui"
                          : !key_field(body, len, "ttn_joineui", t.ttn_joineui, 8, &ttn_changed) ? "ttn_joineui"
                          : !key_field(body, len, "ttn_appkey", t.ttn_appkey, 16, &ttn_changed) ? "ttn_appkey"
                          : !key_field(body, len, "ttn_nwkkey", t.ttn_nwkkey, 16, &ttn_changed) ? "ttn_nwkkey"
                          : !key_field(body, len, "hel_deveui", t.hel_deveui, 8, &hel_changed) ? "hel_deveui"
                          : !key_field(body, len, "hel_joineui", t.hel_joineui, 8, &hel_changed) ? "hel_joineui"
                          : !key_field(body, len, "hel_appkey", t.hel_appkey, 16, &hel_changed) ? "hel_appkey"
                                                                                                 : NULL;
        if (bad) {
            snprintf(result, cap, "invalid=%s", bad);
        } else if (busy && (ttn_changed || hel_changed)) {
            snprintf(result, cap, "busy");
        } else {
            if (ttn_changed || hel_changed) {
                t.keys_gen++;
            }
            cfg = t;
            cfg_save();
            snprintf(result, cap, "ok");
        }
    } else if (strcmp(cmd, "wifi") == 0) {
        cfg_t t = cfg;
        const char *bad = !text_field(body, len, "wifi_ssid", t.wifi_ssid, sizeof t.wifi_ssid) ? "wifi_ssid"
                          : !text_field(body, len, "wifi_pass", t.wifi_pass, sizeof t.wifi_pass) ? "wifi_pass"
                          : !text_field(body, len, "wifi_url", t.wifi_url, sizeof t.wifi_url)   ? "wifi_url"
                                                                                                : NULL;
        if (!bad && t.wifi_url[0] && strncmp(t.wifi_url, "https://", 8) != 0 &&
            strncmp(t.wifi_url, "http://", 7) != 0) {
            bad = "wifi_url";
        }
        if (!bad && t.wifi_pass[0] && strlen(t.wifi_pass) < 8) {
            bad = "wifi_pass";
        }
        if (bad) {
            snprintf(result, cap, "invalid=%s", bad);
        } else {
            cfg = t;
            cfg_save();
            snprintf(result, cap, "ok");
        }
    } else if (strcmp(cmd, "select") == 0) {
        char v[8];
        int slot = sc_form_get(body, len, "slot", v, sizeof v) > 0 ? atoi(v) : -1;
        sc_spot_t t;
        if (busy) {
            snprintf(result, cap, "busy");
        } else if (slot < 0 || slot >= SPOTS || !spot_load(slot, &t)) {
            snprintf(result, cap, "invalid=slot");
        } else {
            cur = t;
            cur_slot = slot;
            spot_save(cur_slot, &cur);
            snprintf(result, cap, "ok");
        }
    } else if (strcmp(cmd, "clear") == 0) {
        if (busy) {
            snprintf(result, cap, "busy");
        } else {
            spots_clear();
            snprintf(result, cap, "ok");
        }
    } else {
        snprintf(result, cap, "invalid=cmd");
    }
    lock_give();
    show();
}

static void ble_on_passkey(uint32_t passkey, bool shown)
{
    if (shown) {
        oled_line(1, "pair code %06" PRIu32, passkey);
    } else {
        show();
    }
}

// History: what `range=` chose. "spots": one list line per stored spot,
// "csv": the CSV header and every spot's lines, "md=<slot>": one spot's report.
enum hist_mode { H_SPOTS, H_CSV, H_MD };
static hist_mode hmode = H_SPOTS;
static int hslot;

static bool ble_on_range(const char *text, size_t len)
{
    char t[16];
    if (len >= sizeof t) {
        return false;
    }
    memcpy(t, text, len);
    t[len] = '\0';
    if (strcmp(t, "spots") == 0 || len == 0) {
        hmode = H_SPOTS;
    } else if (strcmp(t, "csv") == 0) {
        hmode = H_CSV;
    } else if (strncmp(t, "md=", 3) == 0 && atoi(t + 3) >= 0 && atoi(t + 3) < SPOTS) {
        hmode = H_MD;
        hslot = atoi(t + 3);
    } else {
        return false;
    }
    return true;
}

// Copies whole lines of text from line `want` (counting from *line) into out;
// stops when the next line does not fit. Returns false once out is full.
static bool emit_lines(const char *text, uint32_t want, uint32_t *line, char *out, size_t cap, size_t *len)
{
    for (const char *p = text; *p;) {
        const char *nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) + 1 : strlen(p);
        if (*line >= want) {
            if (*len + l > cap) {
                return false;
            }
            memcpy(out + *len, p, l);
            *len += l;
        }
        (*line)++;
        p += l;
    }
    return true;
}

static size_t ble_read_history(uint32_t want, char *out, size_t cap)
{
    static char text[6144];
    static sc_spot_t s;
    size_t len = 0;
    uint32_t line = 0;
    lock_take();
    if (hmode == H_MD) {
        if (hslot == cur_slot ? (s = cur, true) : spot_load(hslot, &s)) {
            sc_report_md(&s, text, sizeof text);
            emit_lines(text, want, &line, out, cap, &len);
        }
    } else {
        if (hmode == H_CSV) {
            emit_lines(SC_CSV_HEADER, want, &line, out, cap, &len);
        }
        // Oldest first.
        int first = n_spots < SPOTS ? 0 : next_slot;
        for (int k = 0; k < n_spots; k++) {
            int slot = (first + k) % SPOTS;
            if (!(slot == cur_slot ? (s = cur, true) : spot_load(slot, &s))) {
                continue;
            }
            if (hmode == H_CSV) {
                sc_report_csv(&s, slot, text, sizeof text);
            } else {
                sc_list_line(&s, slot, text, sizeof text);
            }
            if (!emit_lines(text, want, &line, out, cap, &len)) {
                break;
            }
        }
    }
    lock_give();
    return len;
}

static ble_setup_config_t ble_cfg;

// ---- Key ------------------------------------------------------------------------------------------

static void key_task(void *arg)
{
    int64_t down_at = -1;
    bool long_sent = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        bool down = gpio_get_level((gpio_num_t)PIN_KEY) == 0;
        int64_t t = now_ms();
        if (down && down_at < 0) {
            down_at = t;
            long_sent = false;
        } else if (down && !long_sent && t - down_at >= 1000) {
            long_sent = true;
            cmd_t c = {CMD_NEW_SPOT, 0};
            xQueueSend(cmdq, &c, 0);
        } else if (!down && down_at >= 0) {
            if (!long_sent && t - down_at >= 30) {
                lock_take();
                bool busy = running;
                if (!busy) {
                    running = true;
                }
                lock_give();
                if (!busy) {
                    cmd_t c = {CMD_RUN, SC_T_ALL};
                    xQueueSend(cmdq, &c, 0);
                }
            }
            down_at = -1;
        }
    }
}

// ---- Start ----------------------------------------------------------------------------------------

extern "C" void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    lock = xSemaphoreCreateMutex();
    cmdq = xQueueCreate(2, sizeof(cmd_t));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(dev_name, sizeof dev_name, "SCAN-%02X%02X%02X", mac[3], mac[4], mac[5]);

    gpio_config_t out = {};
    out.pin_bit_mask = (1ULL << PIN_LED) | (1ULL << PIN_VEXT) | (1ULL << PIN_OLED_RST);
    out.mode = GPIO_MODE_OUTPUT;
    gpio_config(&out);
    gpio_config_t in = {};
    in.pin_bit_mask = 1ULL << PIN_KEY;
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&in);

    // OLED: power on through Vext, then pulse its reset.
    gpio_set_level((gpio_num_t)PIN_VEXT, 0);
    gpio_set_level((gpio_num_t)PIN_OLED_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)PIN_OLED_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    oled_start(PIN_OLED_SDA, PIN_OLED_SCL);

    cfg_load();
    spots_load();
    printf("scan: %s fw %s, %d spots stored\n", dev_name, FW_VERSION, n_spots);

    int16_t st = radio.begin(915.0, 125.0, 9, 7, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 14, 8, TCXO_V, false);
    radio_ok = st == RADIOLIB_ERR_NONE;
    if (radio_ok) {
        radio.setDio2AsRfSwitch(true);
    }
    printf("scan: sx1262 %s (%d)\n", radio_ok ? "ok" : "CHECK", st);
    if (radio_ok) set_step("idle");
    else set_step("SX1262 error %d", st);

    wifi_init();

    ble_cfg.name = dev_name;
    ble_cfg.read_info = ble_read_info;
    ble_cfg.read_settings = ble_read_settings;
    ble_cfg.on_apply = ble_on_apply;
    ble_cfg.on_passkey = ble_on_passkey;
    ble_cfg.read_history = ble_read_history;
    ble_cfg.on_history_range = ble_on_range;
    esp_err_t be = ble_setup_start(&ble_cfg);
    printf("scan: bluetooth %s\n", esp_err_to_name(be));

    xTaskCreate(scan_task, "scan", 12288, NULL, 5, NULL);
    xTaskCreate(key_task, "key", 3072, NULL, 4, NULL);
    show();
}
