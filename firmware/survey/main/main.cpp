// LoRaWAN coverage survey on a Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262,
// SSD1306 OLED). Joins a public or private LoRaWAN network (US915) and, on each
// round, sends tiny uplinks with a LinkCheckReq. The network's LinkCheckAns says
// how many gateways heard the uplink and the link margin (dB above the
// demodulation floor of the best gateway); the downlink's RSSI and SNR come from
// the board's own radio. Results go to the OLED and, as CSV, to the serial log.
//
// Keys:  PRG short press = run a round now, long press (1 s) = next spot.
// Setup and use: docs/Method/project/lora-survey.md. Never released.

#include <RadioLib.h>

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "oled.h"
#include "sdkconfig.h"

// Heltec WiFi LoRa 32 V3 pins (Heltec's V3 schematic).
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
static const uint8_t FPORT = 1;
static const int64_t LONG_PRESS_US = 1000000;
#ifdef CONFIG_SURVEY_MODE_LADDER
static const bool LADDER = true;
#else
static const bool LADDER = false;
#endif

static EspHal *hal = new EspHal(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI);
static SX1262 radio = new Module(hal, PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);
static LoRaWANNode node(&radio, &US915, CONFIG_SURVEY_SUBBAND);

// US915 uplink data rates at 125 kHz: DR3 = SF7 ... DR0 = SF10.
static const uint8_t DRS[] = {3, 2, 1, 0};
static const int N_DR = sizeof DRS;

static unsigned spot = 1;
static unsigned round_no = 0;

static int64_t now_us(void) { return esp_timer_get_time(); }

// ---- keys -----------------------------------------------------------------

static bool hex_bytes(const char *s, uint8_t *out, size_t n)
{
    if (strlen(s) != 2 * n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (!isxdigit((unsigned char)s[2 * i]) || !isxdigit((unsigned char)s[2 * i + 1]) ||
            sscanf(s + 2 * i, "%2x", &v) != 1) {
            return false;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

static bool hex_u64(const char *s, uint64_t *out)
{
    uint8_t b[8];
    if (!hex_bytes(s, b, 8)) {
        return false;
    }
    *out = 0;
    for (int i = 0; i < 8; i++) {
        *out = (*out << 8) | b[i];
    }
    return true;
}

static bool all_zero(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0) {
            return false;
        }
    }
    return true;
}

// ---- NVS: the DevNonce counter must survive a reset, or the network rejects
// every join after the first ("DevNonce already used").

static void nonces_load(void)
{
    nvs_handle_t h;
    if (nvs_open("survey", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t buf[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
    size_t len = sizeof buf;
    if (nvs_get_blob(h, "nonces", buf, &len) == ESP_OK && len == sizeof buf) {
        int16_t st = node.setBufferNonces(buf);
        printf("survey: nonces restored %s (%d)\n", st == RADIOLIB_ERR_NONE ? "ok" : "CHECK", st);
    }
    nvs_close(h);
}

static void nonces_save(void)
{
    nvs_handle_t h;
    if (nvs_open("survey", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, "nonces", node.getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
    nvs_commit(h);
    nvs_close(h);
}

// ---- PRG key ----------------------------------------------------------------

enum key_event { KEY_NONE, KEY_SHORT, KEY_LONG };

// Polled; a long press is reported when it reaches 1 s, a short one on release.
static key_event key_poll(void)
{
    static int64_t down_at = -1;
    static bool long_sent = false;
    bool down = gpio_get_level((gpio_num_t)PIN_KEY) == 0;
    if (down && down_at < 0) {
        down_at = now_us();
        long_sent = false;
    } else if (down && !long_sent && now_us() - down_at >= LONG_PRESS_US) {
        long_sent = true;
        return KEY_LONG;
    } else if (!down && down_at >= 0) {
        bool was_long = long_sent;
        int64_t held = now_us() - down_at;
        down_at = -1;
        if (!was_long && held >= 30000) {  // 30 ms debounce
            return KEY_SHORT;
        }
    }
    return KEY_NONE;
}

// Waits up to ms for a key event; KEY_NONE on timeout.
static key_event key_wait(uint32_t ms)
{
    int64_t until = now_us() + (int64_t)ms * 1000;
    while (ms == 0 || now_us() < until) {
        key_event e = key_poll();
        if (e != KEY_NONE) {
            return e;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return KEY_NONE;
}

// ---- display ------------------------------------------------------------------

static void show_header(void)
{
    oled_line(0, "spot %u  round %u", spot, round_no);
}

static int sf_of(uint8_t dr) { return 10 - dr; }

// ---- join ---------------------------------------------------------------------

static void join(void)
{
    uint32_t backoff_s = 15;
    for (int attempt = 1;; attempt++) {
        oled_line(1, "joining, try %d", attempt);
        oled_line(2, "US915 sub-band %d", CONFIG_SURVEY_SUBBAND);
        oled_line(3, " ");
        oled_line(4, " ");
        gpio_set_level((gpio_num_t)PIN_LED, 1);
        int64_t t0 = now_us();
        LoRaWANJoinEvent_t ev;
        int16_t st = node.activateOTAA(&ev);
        gpio_set_level((gpio_num_t)PIN_LED, 0);
        int64_t ms = (now_us() - t0) / 1000;
        nonces_save();
        bool ok = st == RADIOLIB_LORAWAN_NEW_SESSION || st == RADIOLIB_LORAWAN_SESSION_RESTORED;
        printf("join,%d,%s,%d,%" PRId64 ",%u\n", attempt, ok ? "ok" : "fail", st, ms, ev.devNonce);
        if (ok) {
            oled_line(1, "joined after %d try", attempt);
            oled_line(2, "DevAddr %08" PRIx32, node.getDevAddr());
            return;
        }
        oled_line(1, "join try %d: no", attempt);
        oled_line(2, "retry in %" PRIu32 " s (key: now)", backoff_s);
        if (st != RADIOLIB_ERR_NO_JOIN_ACCEPT) {
            oled_line(3, "error %d", st);
        }
        key_wait(backoff_s * 1000);
        backoff_s = backoff_s < 240 ? backoff_s * 2 : 300;
    }
}

// ---- one survey round ------------------------------------------------------------

static void run_round(void)
{
    round_no++;
    show_header();
    for (int i = 0; i < N_DR; i++) {
        oled_line(1 + i, "SF%-2d -", sf_of(DRS[i]));
    }
    gpio_set_level((gpio_num_t)PIN_LED, 1);
    for (int i = 0; i < N_DR; i++) {
        uint8_t dr = DRS[i];
        int sf = sf_of(dr);
        oled_line(1 + i, "SF%-2d sending", sf);
        int16_t st = node.setDatarate(dr);
        if (st == RADIOLIB_ERR_NONE) {
            node.sendMacCommandReq(RADIOLIB_LORAWAN_MAC_LINK_CHECK);
            // Payload: spot, round (16 bit), data rate; it shows up with each
            // gateway's RSSI and SNR in the network console's live data.
            uint8_t up[4] = {(uint8_t)spot, (uint8_t)(round_no >> 8), (uint8_t)round_no, dr};
            LoRaWANEvent_t evUp, evDown;
            st = node.sendReceive(up, sizeof up, FPORT, false, &evUp, &evDown);
            if (st > 0) {
                uint8_t margin = 0, gw = 0;
                bool lc = node.getMacLinkCheckAns(&margin, &gw) == RADIOLIB_ERR_NONE;
                int rssi = (int)radio.getRSSI();
                float snr = radio.getSNR();
                if (lc) {
                    oled_line(1 + i, "SF%-2d m%udB %ugw %d", sf, margin, gw, rssi);
                } else {
                    oled_line(1 + i, "SF%-2d down %d, no LC", sf, rssi);
                }
                printf("survey,%u,%u,%d,%s,%d,%d,%d,%.1f,%" PRIu32 ",%" PRIu32 "\n", spot, round_no, sf,
                       lc ? "ok" : "down", lc ? margin : -1, lc ? gw : -1, rssi, snr,
                       node.getFCntUp(), (uint32_t)node.getLastToA());
                if (lc && LADDER) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }
        if (st == 0) {
            oled_line(1 + i, "SF%-2d no answer", sf);
            printf("survey,%u,%u,%d,none,,,,,%" PRIu32 ",%" PRIu32 "\n", spot, round_no, sf,
                   node.getFCntUp(), (uint32_t)node.getLastToA());
        } else {
            oled_line(1 + i, "SF%-2d error %d", sf, st);
            printf("survey,%u,%u,%d,err%d,,,,,,\n", spot, round_no, sf, st);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    gpio_set_level((gpio_num_t)PIN_LED, 0);
}

// ---- main ---------------------------------------------------------------------------

static void halt(const char *l1, const char *l2)
{
    printf("survey: %s %s\n", l1, l2);
    oled_line(1, "%s", l1);
    oled_line(2, "%s", l2);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

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
    show_header();

    printf("survey: heltec v3 lora survey, US915 sub-band %d, %d dBm, %s, every %d s\n",
           CONFIG_SURVEY_SUBBAND, CONFIG_SURVEY_TX_DBM,
           LADDER ? "ladder" : "all", CONFIG_SURVEY_EVERY_S);
    printf("join,try,result,code,ms,devnonce\n");
    printf("survey,spot,round,sf,result,margin_db,gateways,rssi_dbm,snr_db,fcnt_up,toa_ms\n");

    uint64_t dev_eui, join_eui;
    uint8_t app_key[16], nwk_key[16];
    bool has_nwk = CONFIG_SURVEY_NWK_KEY[0] != '\0';
    if (!hex_u64(CONFIG_SURVEY_DEV_EUI, &dev_eui) || !hex_u64(CONFIG_SURVEY_JOIN_EUI, &join_eui) ||
        !hex_bytes(CONFIG_SURVEY_APP_KEY, app_key, 16) || all_zero(app_key, 16) ||
        (has_nwk && !hex_bytes(CONFIG_SURVEY_NWK_KEY, nwk_key, 16))) {
        halt("keys not set", "see lora-survey.md");
    }

    int16_t st = radio.begin(915.0, 125.0, 9, 7, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 14, 8, TCXO_V, false);
    if (st != RADIOLIB_ERR_NONE) {
        char msg[22];
        snprintf(msg, sizeof msg, "error %d", st);
        halt("SX1262 not found", msg);
    }
    radio.setDio2AsRfSwitch(true);
    printf("survey: sx1262 ok\n");

    st = node.beginOTAA(join_eui, dev_eui, has_nwk ? nwk_key : NULL, app_key);
    if (st != RADIOLIB_ERR_NONE) {
        char msg[22];
        snprintf(msg, sizeof msg, "beginOTAA %d", st);
        halt("LoRaWAN setup", msg);
    }
    nonces_load();
    join();

    node.setADR(false);
    node.setTxPower(CONFIG_SURVEY_TX_DBM);
    oled_line(3, "short key: round");
    oled_line(4, "long key: next spot");

    for (;;) {
        key_event k = key_wait(CONFIG_SURVEY_EVERY_S * 1000);
        if (k == KEY_LONG) {
            spot++;
            printf("spot,%u\n", spot);
            show_header();
            while (gpio_get_level((gpio_num_t)PIN_KEY) == 0) {  // wait for release
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            key_poll();  // clears the press without a short event
            continue;
        }
        run_round();  // a short press, or the timer
    }
}
