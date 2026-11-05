// Site scanner, pure core: what one spot's tests measured, the verdict each
// connection earns (docs/site-scanner-plan.md, pass marks), the spot report as
// Markdown and CSV, and the form bodies the Bluetooth page writes. No ESP-IDF
// headers, so tests/host compiles it with the host cc.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SC_NOT_TESTED = 0,
    SC_GOOD,
    SC_MARGINAL,
    SC_NOT_USABLE,
} sc_verdict_t;

const char *sc_verdict_name(sc_verdict_t v);  // "Good", "Marginal", ...

// ---- WiFi -------------------------------------------------------------------

#define SC_APS 8  // the strongest networks kept per spot

typedef struct {
    char ssid[33];
    int8_t rssi;      // dBm
    uint8_t channel;
    bool open;        // no password
} sc_ap_t;

typedef struct {
    bool scanned;
    uint8_t n_aps;          // networks heard (at most SC_APS kept, strongest first)
    uint16_t n_heard;       // all networks heard, before the cut
    sc_ap_t aps[SC_APS];
    // Connect test, only with credentials.
    char target[33];        // SSID tried; empty: no connect test
    int8_t target_rssi;     // from the scan; 0 when the scan did not hear it
    uint8_t attempts;
    uint8_t connects;       // attempts that got an IP address
    uint32_t connect_ms;    // the first successful connect, to an IP address
    bool https_tried;       // a URL was given
    bool https_ok;          // a 2xx or 3xx answer
    int16_t https_status;   // HTTP status, or -1 for no answer
    uint32_t https_ms;
} sc_wifi_t;

sc_verdict_t sc_wifi_verdict(const sc_wifi_t *w);

// ---- LoRaWAN ----------------------------------------------------------------

// US915 125 kHz uplink data rates: index 0 = SF7 (DR3) ... 3 = SF10 (DR0).
#define SC_SFS 4
static inline int sc_sf(int i) { return 7 + i; }

typedef struct {
    bool tried;             // keys were set and the test ran
    bool joined;
    uint8_t join_tries;
    uint8_t rounds;
    uint8_t sent[SC_SFS];
    uint8_t answered[SC_SFS];   // a LinkCheckAns, or with confirmed uplinks an ACK
    bool margin_known;          // the network answered LinkCheckReq at least once
    int16_t margin[SC_SFS];     // best LinkCheck margin (dB), -1 none
    uint8_t gateways[SC_SFS];   // most gateways in one LinkCheckAns
    int16_t rssi[SC_SFS];       // best downlink RSSI (dBm), 0 none
    int16_t snr_x10[SC_SFS];    // that downlink's SNR in 0.1 dB
} sc_lorawan_t;

void sc_lorawan_clear(sc_lorawan_t *l);

// Record one uplink at SF index i: answered, and when a LinkCheckAns came,
// its margin and gateway count (lc false: an ACK only).
void sc_lorawan_add(sc_lorawan_t *l, int i, bool answered, bool lc, uint8_t margin, uint8_t gw,
                    int16_t rssi, int16_t snr_x10);

// The SF index a ladder round starts at the next uplink after one at i:
// i + 1, or -1 when the round is over (answered, or SF10 sent).
int sc_ladder_next(int i, bool answered);

sc_verdict_t sc_lorawan_verdict(const sc_lorawan_t *l);

// ---- A spot -----------------------------------------------------------------

typedef struct {
    uint32_t magic;          // SC_SPOT_MAGIC: a stored blob of this layout
    uint32_t when;           // Unix time from the phone, 0 unknown
    char site[32];
    char spot[32];
    bool has_pos;
    int32_t lat_e6, lon_e6;  // degrees x 1e6
    uint16_t acc_m;          // position accuracy, 0 unknown
    uint16_t height_cm;
    uint8_t enclosure;       // 0 no, 1 yes, 2 unknown
    char notes[64];
    sc_wifi_t wifi;
    sc_lorawan_t ttn, helium;
} sc_spot_t;

#define SC_SPOT_MAGIC 0x53435031u  // "SCP1"

void sc_spot_init(sc_spot_t *s);

// ---- Reports ----------------------------------------------------------------

// The spot as a Markdown report (terminated, cut at cap). Returns the length.
size_t sc_report_md(const sc_spot_t *s, char *out, size_t cap);

// CSV: one line per spot and connection, after the header line.
extern const char SC_CSV_HEADER[];
size_t sc_report_csv(const sc_spot_t *s, int index, char *out, size_t cap);

// One line for the page's list of spots:
// index|site|spot|when|wifi verdict|ttn verdict|helium verdict
size_t sc_list_line(const sc_spot_t *s, int index, char *out, size_t cap);

// ---- Form bodies --------------------------------------------------------------

// The value of field name in a URL-encoded body, decoded into out (cap bytes,
// terminated). Returns its length, or -1 when the field is absent or too long.
int sc_form_get(const char *body, size_t len, const char *name, char *out, size_t cap);

// Hex text to bytes: exactly 2n hex digits. False otherwise.
bool sc_hex(const char *s, uint8_t *out, size_t n);

// The spot details of a `cmd=spot` body into s (tests untouched). Returns NULL,
// or the name of the first invalid field.
const char *sc_parse_spot(const char *body, size_t len, sc_spot_t *s);

// The tests a `cmd=run` body asks for: tests=wifi,ttn,helium (any of them, or
// "all"). Bits SC_T_*; 0 when the field is absent or names something else.
#define SC_T_WIFI 1u
#define SC_T_TTN 2u
#define SC_T_HELIUM 4u
#define SC_T_ALL (SC_T_WIFI | SC_T_TTN | SC_T_HELIUM)
unsigned sc_parse_tests(const char *body, size_t len);

#ifdef __cplusplus
}
#endif
