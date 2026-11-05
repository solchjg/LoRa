// Host tests of components/scan_core: built and run by test_scan_core.py.
// Each CHECK prints the failing line; the exit status is the failure count.
#include <stdio.h>
#include <string.h>

#include "scan_core.h"

static int failures;
#define CHECK(c)                                                  \
    do {                                                          \
        if (!(c)) {                                               \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
            failures++;                                           \
        }                                                         \
    } while (0)

static void wifi_verdicts(void)
{
    sc_wifi_t w = {0};
    CHECK(sc_wifi_verdict(&w) == SC_NOT_TESTED);
    w.scanned = true;
    CHECK(sc_wifi_verdict(&w) == SC_NOT_USABLE);  // nothing heard
    w.n_aps = 1;
    w.aps[0].rssi = -60;
    CHECK(sc_wifi_verdict(&w) == SC_GOOD);
    w.aps[0].rssi = -67;
    CHECK(sc_wifi_verdict(&w) == SC_GOOD);
    w.aps[0].rssi = -68;
    CHECK(sc_wifi_verdict(&w) == SC_MARGINAL);
    w.aps[0].rssi = -80;
    CHECK(sc_wifi_verdict(&w) == SC_MARGINAL);
    w.aps[0].rssi = -81;
    CHECK(sc_wifi_verdict(&w) == SC_NOT_USABLE);
    // Connect test.
    strcpy(w.target, "Shop");
    w.target_rssi = -60;
    w.attempts = 3;
    w.connects = 0;
    CHECK(sc_wifi_verdict(&w) == SC_NOT_USABLE);
    w.connects = 2;
    CHECK(sc_wifi_verdict(&w) == SC_MARGINAL);
    w.connects = 3;
    CHECK(sc_wifi_verdict(&w) == SC_GOOD);
    w.https_tried = true;
    w.https_ok = false;
    CHECK(sc_wifi_verdict(&w) == SC_MARGINAL);
    w.https_ok = true;
    CHECK(sc_wifi_verdict(&w) == SC_GOOD);
    w.target_rssi = -72;
    CHECK(sc_wifi_verdict(&w) == SC_MARGINAL);
}

static void lorawan_verdicts(void)
{
    sc_lorawan_t l;
    sc_lorawan_clear(&l);
    CHECK(sc_lorawan_verdict(&l) == SC_NOT_TESTED);
    l.tried = true;
    CHECK(sc_lorawan_verdict(&l) == SC_NOT_USABLE);  // no join
    l.joined = true;
    sc_lorawan_add(&l, 0, false, false, 0, 0, 0, 0);
    CHECK(sc_lorawan_verdict(&l) == SC_NOT_USABLE);
    sc_lorawan_add(&l, 1, true, true, 20, 2, -100, 55);
    CHECK(sc_lorawan_verdict(&l) == SC_MARGINAL);  // only SF8
    sc_lorawan_add(&l, 0, true, true, 6, 1, -110, -20);
    CHECK(sc_lorawan_verdict(&l) == SC_MARGINAL);  // SF7, 6 dB
    sc_lorawan_add(&l, 0, true, true, 12, 3, -90, 70);
    CHECK(sc_lorawan_verdict(&l) == SC_GOOD);
    CHECK(l.margin[0] == 12 && l.gateways[0] == 3 && l.rssi[0] == -90 && l.snr_x10[0] == 70);
    CHECK(l.sent[0] == 3 && l.answered[0] == 2);

    // ACK only (no LinkCheckAns ever): SF7 is Good.
    sc_lorawan_t h;
    sc_lorawan_clear(&h);
    h.tried = h.joined = true;
    sc_lorawan_add(&h, 0, true, false, 0, 0, -95, 40);
    CHECK(!h.margin_known && h.margin[0] == -1);
    CHECK(sc_lorawan_verdict(&h) == SC_GOOD);

    CHECK(sc_ladder_next(0, false) == 1);
    CHECK(sc_ladder_next(0, true) == -1);
    CHECK(sc_ladder_next(3, false) == -1);
}

static void forms(void)
{
    char v[32];
    const char *b = "cmd=spot&site=North+Field&spot=Pump%20house&x&lat=40.5";
    CHECK(sc_form_get(b, strlen(b), "site", v, sizeof v) == 11 && strcmp(v, "North Field") == 0);
    CHECK(sc_form_get(b, strlen(b), "spot", v, sizeof v) == 10 && strcmp(v, "Pump house") == 0);
    CHECK(sc_form_get(b, strlen(b), "x", v, sizeof v) == 0);
    CHECK(sc_form_get(b, strlen(b), "nope", v, sizeof v) == -1);
    CHECK(sc_form_get(b, strlen(b), "site", v, 5) == -1);  // too long
    CHECK(sc_form_get("a=%4", 4, "a", v, sizeof v) == 2 && strcmp(v, "%4") == 0);

    uint8_t k[2];
    CHECK(sc_hex("aB09", k, 2) && k[0] == 0xab && k[1] == 0x09);
    CHECK(!sc_hex("aB0", k, 2));
    CHECK(!sc_hex("zz09", k, 2));

    sc_spot_t s;
    sc_spot_init(&s);
    const char *ok = "cmd=spot&site=Farm&spot=Tank+2&lat=40.712776&lon=-74.005974&acc=8"
                     "&height_cm=150&enclosure=yes&when=1793860000&notes=north+wall";
    CHECK(sc_parse_spot(ok, strlen(ok), &s) == NULL);
    CHECK(strcmp(s.site, "Farm") == 0 && strcmp(s.spot, "Tank 2") == 0);
    CHECK(s.has_pos && s.lat_e6 == 40712776 && s.lon_e6 == -74005974 && s.acc_m == 8);
    CHECK(s.height_cm == 150 && s.enclosure == 1 && s.when == 1793860000u);
    const char *bad = "lat=91&lon=0";
    CHECK(strcmp(sc_parse_spot(bad, strlen(bad), &s), "lat") == 0);
    CHECK(s.lat_e6 == 40712776);  // unchanged on refusal
    const char *half = "lat=10";
    CHECK(strcmp(sc_parse_spot(half, strlen(half), &s), "lon") == 0);
    const char *enc = "enclosure=maybe";
    CHECK(strcmp(sc_parse_spot(enc, strlen(enc), &s), "enclosure") == 0);

    const char *t1 = "cmd=run&tests=wifi,helium";
    CHECK(sc_parse_tests(t1, strlen(t1)) == (SC_T_WIFI | SC_T_HELIUM));
    const char *t2 = "tests=all";
    CHECK(sc_parse_tests(t2, strlen(t2)) == SC_T_ALL);
    const char *t3 = "tests=wifi,cell";
    CHECK(sc_parse_tests(t3, strlen(t3)) == 0);
}

static void reports(void)
{
    sc_spot_t s;
    sc_spot_init(&s);
    strcpy(s.site, "Farm");
    strcpy(s.spot, "Tank|2");
    s.when = 1793860000u;  // 2026-11-05 06:26:40 UTC = 01:26 EST
    s.has_pos = true;
    s.lat_e6 = 40712776;
    s.lon_e6 = -74005974;
    s.wifi.scanned = true;
    s.wifi.n_aps = 1;
    s.wifi.n_heard = 4;
    strcpy(s.wifi.aps[0].ssid, "Shop,1");
    s.wifi.aps[0].rssi = -61;
    s.wifi.aps[0].channel = 6;
    s.ttn.tried = s.ttn.joined = true;
    sc_lorawan_add(&s.ttn, 0, false, false, 0, 0, 0, 0);
    sc_lorawan_add(&s.ttn, 1, true, true, 14, 2, -97, 65);

    char out[4096];
    size_t n = sc_report_md(&s, out, sizeof out);
    CHECK(n == strlen(out));
    CHECK(strstr(out, "# Site scan: Farm / Tank/2") != NULL);
    CHECK(strstr(out, "- When: 2026-11-05 01:26 EST") != NULL);
    CHECK(strstr(out, "- Position: 40.712776, -74.005974") != NULL);
    CHECK(strstr(out, "| WiFi | Good | 4 networks heard") != NULL);
    CHECK(strstr(out, "| LoRaWAN (TTN) | Marginal | SF8: 1/1 answered, margin 14 dB, 2 gateways") != NULL);
    CHECK(strstr(out, "| LoRaWAN (Helium) | Not tested | no keys set |") != NULL);
    CHECK(strstr(out, "| TTN | SF7 | 0/1 | - | - | - |") != NULL);

    n = sc_report_csv(&s, 3, out, sizeof out);
    CHECK(strncmp(out, "3,Farm,Tank|2,2026-11-05 01:26 EST,40.712776,-74.005974,,not noted,wifi,Good,", 76) == 0);
    CHECK(strstr(out, "wifi_ap,,,,,,,-61,,\"Shop,1\",6,password\n") != NULL);
    CHECK(strstr(out, "ttn_sf,,8,1,1,14,2,-97,6.5,,,\n") != NULL);
    CHECK(strstr(out, ",helium,Not tested,,,,,,,,,,no keys set\n") != NULL);
    // Every CSV line has the header's 20 fields (none of these have quoted commas but one).
    int cols = 1;
    for (const char *p = SC_CSV_HEADER; *p != '\n'; p++) {
        cols += *p == ',';
    }
    CHECK(cols == 20);

    sc_list_line(&s, 3, out, sizeof out);
    CHECK(strcmp(out, "3|Farm|Tank/2|2026-11-05 01:26 EST|Good|Marginal|Not tested\n") == 0);

    // Daylight time: 2026-07-01 16:00 UTC = 12:00 EDT.
    s.when = 1782921600u;
    sc_list_line(&s, 0, out, sizeof out);
    CHECK(strstr(out, "2026-07-01 12:00 EDT") != NULL);

    // The switches: 2026-03-08 and 2026-11-01.
    const struct { uint32_t t; const char *want; } sw[] = {
        {1772953199u, "2026-03-08 01:59 EST"}, {1772953200u, "2026-03-08 03:00 EDT"},
        {1793512799u, "2026-11-01 01:59 EDT"}, {1793512800u, "2026-11-01 01:00 EST"},
    };
    for (size_t i = 0; i < sizeof sw / sizeof sw[0]; i++) {
        s.when = sw[i].t;
        sc_list_line(&s, 0, out, sizeof out);
        CHECK(strstr(out, sw[i].want) != NULL);
    }

    // A small buffer is cut, never overrun.
    char tiny[16];
    memset(tiny, 'x', sizeof tiny);
    n = sc_report_md(&s, tiny, 10);
    CHECK(n == 9 && tiny[9] == '\0' && tiny[10] == 'x');
}

int main(void)
{
    wifi_verdicts();
    lorawan_verdicts();
    forms();
    reports();
    printf("%d failures\n", failures);
    return failures;
}
