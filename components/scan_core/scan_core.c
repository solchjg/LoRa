#include "scan_core.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *sc_verdict_name(sc_verdict_t v)
{
    switch (v) {
    case SC_GOOD:
        return "Good";
    case SC_MARGINAL:
        return "Marginal";
    case SC_NOT_USABLE:
        return "Not usable";
    default:
        return "Not tested";
    }
}

// ---- WiFi -------------------------------------------------------------------

// Pass marks (plan): Good at -67 dBm or better, Marginal to -80 dBm.
#define WIFI_GOOD_DBM (-67)
#define WIFI_USABLE_DBM (-80)

sc_verdict_t sc_wifi_verdict(const sc_wifi_t *w)
{
    if (!w->scanned) {
        return SC_NOT_TESTED;
    }
    if (w->target[0] != '\0' && w->attempts > 0) {
        if (w->connects == 0) {
            return SC_NOT_USABLE;
        }
        if (w->connects < w->attempts || (w->https_tried && !w->https_ok)) {
            return SC_MARGINAL;
        }
        if (w->target_rssi != 0 && w->target_rssi < WIFI_GOOD_DBM) {
            return SC_MARGINAL;
        }
        return SC_GOOD;
    }
    // Signal only: the strongest network heard.
    if (w->n_aps == 0) {
        return SC_NOT_USABLE;
    }
    int rssi = w->aps[0].rssi;
    return rssi >= WIFI_GOOD_DBM ? SC_GOOD : rssi >= WIFI_USABLE_DBM ? SC_MARGINAL : SC_NOT_USABLE;
}

// ---- LoRaWAN ----------------------------------------------------------------

#define LORAWAN_GOOD_MARGIN_DB 10

void sc_lorawan_clear(sc_lorawan_t *l)
{
    memset(l, 0, sizeof *l);
    for (int i = 0; i < SC_SFS; i++) {
        l->margin[i] = -1;
    }
}

void sc_lorawan_add(sc_lorawan_t *l, int i, bool answered, bool lc, uint8_t margin, uint8_t gw,
                    int16_t rssi, int16_t snr_x10)
{
    if (i < 0 || i >= SC_SFS) {
        return;
    }
    if (l->sent[i] < UINT8_MAX) {
        l->sent[i]++;
    }
    if (!answered) {
        return;
    }
    if (l->answered[i] < UINT8_MAX) {
        l->answered[i]++;
    }
    if (lc) {
        l->margin_known = true;
        if ((int)margin > l->margin[i]) {
            l->margin[i] = margin;
        }
        if (gw > l->gateways[i]) {
            l->gateways[i] = gw;
        }
    }
    if (l->rssi[i] == 0 || rssi > l->rssi[i]) {
        l->rssi[i] = rssi;
        l->snr_x10[i] = snr_x10;
    }
}

int sc_ladder_next(int i, bool answered)
{
    if (answered || i + 1 >= SC_SFS) {
        return -1;
    }
    return i + 1;
}

sc_verdict_t sc_lorawan_verdict(const sc_lorawan_t *l)
{
    if (!l->tried) {
        return SC_NOT_TESTED;
    }
    if (!l->joined) {
        return SC_NOT_USABLE;
    }
    int first = -1;
    for (int i = 0; i < SC_SFS; i++) {
        if (l->answered[i] > 0) {
            first = i;
            break;
        }
    }
    if (first < 0) {
        return SC_NOT_USABLE;
    }
    if (first > 0) {
        return SC_MARGINAL;
    }
    if (l->margin[0] >= LORAWAN_GOOD_MARGIN_DB) {
        return SC_GOOD;
    }
    // A network that never answered LinkCheckReq (an ACK only): SF7 is the
    // best this test can show.
    return l->margin_known ? SC_MARGINAL : SC_GOOD;
}

// ---- A spot -----------------------------------------------------------------

void sc_spot_init(sc_spot_t *s)
{
    memset(s, 0, sizeof *s);
    s->magic = SC_SPOT_MAGIC;
    s->enclosure = 2;
    sc_lorawan_clear(&s->ttn);
    sc_lorawan_clear(&s->helium);
}

// ---- Text helpers ---------------------------------------------------------------

typedef struct {
    char *out;
    size_t cap, len;
} buf_t;

static void put(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(buf_t *b, const char *fmt, ...)
{
    if (b->cap == 0 || b->len >= b->cap - 1) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->out + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    b->len += (size_t)n;
    if (b->len > b->cap - 1) {
        b->len = b->cap - 1;
    }
}

// Days from 1970-01-01 to y-m-d (proleptic Gregorian).
static long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static void civil_from_days(long z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (*m <= 2));
}

// The Unix time of 2:00 local standard time on the n-th Sunday (n >= 1) of
// month m, given the zone's standard offset in seconds.
static long nth_sunday_2am(int y, unsigned m, int n, long std_off)
{
    long first = days_from_civil(y, m, 1);
    int wday = (int)((first + 4) % 7);  // 1970-01-01 was a Thursday (4)
    if (wday < 0) {
        wday += 7;
    }
    long day = first + (7 - wday) % 7 + 7L * (n - 1);
    return day * 86400 + 2 * 3600 - std_off;
}

// Unix time as US Eastern local time, "2026-11-05 09:41 EST".
static void eastern(uint32_t t, char *out, size_t cap)
{
    const long est = -5 * 3600;
    int y;
    unsigned m, d;
    civil_from_days((long)((t + est) / 86400), &y, &m, &d);
    // EDT from 2:00 EST on the second Sunday of March to 2:00 EDT (1:00 EST)
    // on the first Sunday of November.
    long start = nth_sunday_2am(y, 3, 2, est);
    long end = nth_sunday_2am(y, 11, 1, est) - 3600;
    bool dst = (long)t >= start && (long)t < end;
    long local = (long)t + est + (dst ? 3600 : 0);
    long days = local / 86400;
    long secs = local % 86400;
    civil_from_days(days, &y, &m, &d);
    snprintf(out, cap, "%04d-%02u-%02u %02ld:%02ld %s", y, m, d, secs / 3600, (secs / 60) % 60,
             dst ? "EDT" : "EST");
}

static void coord(int32_t e6, char *out, size_t cap)
{
    long a = labs((long)e6);
    snprintf(out, cap, "%s%ld.%06ld", e6 < 0 ? "-" : "", a / 1000000, a % 1000000);
}

static const char *enclosure_text(uint8_t e)
{
    return e == 1 ? "yes" : e == 0 ? "no" : "not noted";
}

// A field of Markdown table text: '|' and line breaks would break the table.
static void md_cell(buf_t *b, const char *s)
{
    for (; *s; s++) {
        char c = *s;
        put(b, "%c", c == '|' ? '/' : (c == '\n' || c == '\r') ? ' ' : c);
    }
}

static void wifi_summary(buf_t *b, const sc_wifi_t *w)
{
    if (!w->scanned) {
        put(b, "not run");
        return;
    }
    if (w->n_aps == 0) {
        put(b, "no networks heard");
    } else {
        put(b, "%u networks heard, strongest \"", (unsigned)w->n_heard);
        md_cell(b, w->aps[0].ssid);
        put(b, "\" ch %u %d dBm", w->aps[0].channel, w->aps[0].rssi);
    }
    if (w->target[0] == '\0' || w->attempts == 0) {
        put(b, "; signal only, no connect test");
        return;
    }
    put(b, "; \"");
    md_cell(b, w->target);
    put(b, "\" ");
    if (w->target_rssi != 0) {
        put(b, "%d dBm, ", w->target_rssi);
    } else {
        put(b, "not in the scan, ");
    }
    put(b, "%u/%u connects", w->connects, w->attempts);
    if (w->connects > 0) {
        put(b, ", first in %.1f s", w->connect_ms / 1000.0);
    }
    if (w->https_tried) {
        if (w->https_status < 0) {
            put(b, "; HTTPS no answer");
        } else {
            put(b, "; HTTPS %d in %u ms", w->https_status, (unsigned)w->https_ms);
        }
    }
}

static void lorawan_summary(buf_t *b, const sc_lorawan_t *l)
{
    if (!l->tried) {
        put(b, "no keys set");
        return;
    }
    if (!l->joined) {
        put(b, "no join after %u tries", l->join_tries);
        return;
    }
    int first = -1;
    for (int i = 0; i < SC_SFS; i++) {
        if (l->answered[i] > 0) {
            first = i;
            break;
        }
    }
    if (first < 0) {
        put(b, "joined, but no answer at SF7 - SF10 in %u rounds", l->rounds);
        return;
    }
    put(b, "SF%d: %u/%u answered", sc_sf(first), l->answered[first], l->sent[first]);
    if (l->margin[first] >= 0) {
        put(b, ", margin %d dB, %u gateway%s", l->margin[first], l->gateways[first],
            l->gateways[first] == 1 ? "" : "s");
    } else {
        put(b, ", margin not reported");
    }
    if (l->rssi[first] != 0) {
        put(b, ", downlink %d dBm / %.1f dB", l->rssi[first], l->snr_x10[first] / 10.0);
    }
}

size_t sc_report_md(const sc_spot_t *s, char *out, size_t cap)
{
    buf_t b = {out, cap, 0};
    if (cap > 0) {
        out[0] = '\0';
    }
    put(&b, "# Site scan: ");
    md_cell(&b, s->site[0] ? s->site : "(no site)");
    put(&b, " / ");
    md_cell(&b, s->spot[0] ? s->spot : "(no spot)");
    put(&b, "\n\n");
    if (s->when) {
        char t[32];
        eastern(s->when, t, sizeof t);
        put(&b, "- When: %s\n", t);
    }
    if (s->has_pos) {
        char la[16], lo[16];
        coord(s->lat_e6, la, sizeof la);
        coord(s->lon_e6, lo, sizeof lo);
        put(&b, "- Position: %s, %s", la, lo);
        if (s->acc_m) {
            put(&b, " (within %u m)", s->acc_m);
        }
        put(&b, "\n");
    } else {
        put(&b, "- Position: not recorded\n");
    }
    if (s->height_cm) {
        put(&b, "- Mounting height: %.2f m\n", s->height_cm / 100.0);
    }
    put(&b, "- In the enclosure: %s\n", enclosure_text(s->enclosure));
    if (s->notes[0]) {
        put(&b, "- Notes: ");
        md_cell(&b, s->notes);
        put(&b, "\n");
    }

    put(&b, "\n| Connection | Verdict | Details |\n|---|---|---|\n");
    put(&b, "| WiFi | %s | ", sc_verdict_name(sc_wifi_verdict(&s->wifi)));
    wifi_summary(&b, &s->wifi);
    put(&b, " |\n| LoRaWAN (TTN) | %s | ", sc_verdict_name(sc_lorawan_verdict(&s->ttn)));
    lorawan_summary(&b, &s->ttn);
    put(&b, " |\n| LoRaWAN (Helium) | %s | ", sc_verdict_name(sc_lorawan_verdict(&s->helium)));
    lorawan_summary(&b, &s->helium);
    put(&b, " |\n");

    if (s->wifi.scanned && s->wifi.n_aps > 0) {
        put(&b, "\n## WiFi networks heard (strongest %u)\n\n| SSID | Channel | RSSI | Security |\n|---|---|---|---|\n",
            s->wifi.n_aps);
        for (int i = 0; i < s->wifi.n_aps && i < SC_APS; i++) {
            const sc_ap_t *a = &s->wifi.aps[i];
            put(&b, "| ");
            md_cell(&b, a->ssid[0] ? a->ssid : "(hidden)");
            put(&b, " | %u | %d dBm | %s |\n", a->channel, a->rssi, a->open ? "open" : "password");
        }
    }

    const sc_lorawan_t *nets[2] = {&s->ttn, &s->helium};
    const char *names[2] = {"TTN", "Helium"};
    bool any = s->ttn.joined || s->helium.joined;
    if (any) {
        put(&b, "\n## LoRaWAN by spreading factor\n\n| Network | SF | Answered | Margin | Gateways | Downlink |\n|---|---|---|---|---|---|\n");
        for (int n = 0; n < 2; n++) {
            const sc_lorawan_t *l = nets[n];
            if (!l->joined) {
                continue;
            }
            for (int i = 0; i < SC_SFS; i++) {
                if (l->sent[i] == 0) {
                    continue;
                }
                put(&b, "| %s | SF%d | %u/%u | ", names[n], sc_sf(i), l->answered[i], l->sent[i]);
                if (l->margin[i] >= 0) {
                    put(&b, "%d dB | %u |", l->margin[i], l->gateways[i]);
                } else {
                    put(&b, "- | - |");
                }
                if (l->rssi[i] != 0) {
                    put(&b, " %d dBm / %.1f dB |\n", l->rssi[i], l->snr_x10[i] / 10.0);
                } else {
                    put(&b, " - |\n");
                }
            }
        }
        put(&b, "\nA ladder round tries SF7 first and moves to a slower SF only when no answer comes.\n");
    }
    return b.len;
}

const char SC_CSV_HEADER[] =
    "index,site,spot,time,lat,lon,height_m,enclosure,connection,verdict,sf,sent,answered,"
    "margin_db,gateways,rssi_dbm,snr_db,ssid,channel,summary\n";

// A CSV field, quoted when it holds a comma, a quote or a line break.
static void csv_field(buf_t *b, const char *s)
{
    if (strpbrk(s, ",\"\r\n") == NULL) {
        put(b, "%s", s);
        return;
    }
    put(b, "\"");
    for (; *s; s++) {
        if (*s == '"') {
            put(b, "\"\"");
        } else if (*s == '\r' || *s == '\n') {
            put(b, " ");
        } else {
            put(b, "%c", *s);
        }
    }
    put(b, "\"");
}

static void csv_prefix(buf_t *b, const sc_spot_t *s, int index)
{
    char t[32] = "", la[16] = "", lo[16] = "", h[12] = "";
    if (s->when) {
        eastern(s->when, t, sizeof t);
    }
    if (s->has_pos) {
        coord(s->lat_e6, la, sizeof la);
        coord(s->lon_e6, lo, sizeof lo);
    }
    if (s->height_cm) {
        snprintf(h, sizeof h, "%.2f", s->height_cm / 100.0);
    }
    put(b, "%d,", index);
    csv_field(b, s->site);
    put(b, ",");
    csv_field(b, s->spot);
    put(b, ",%s,%s,%s,%s,%s,", t, la, lo, h, enclosure_text(s->enclosure));
}

size_t sc_report_csv(const sc_spot_t *s, int index, char *out, size_t cap)
{
    buf_t b = {out, cap, 0};
    if (cap > 0) {
        out[0] = '\0';
    }
    char sum[256];
    buf_t sb;

    // One summary line per connection.
    sb = (buf_t){sum, sizeof sum, 0};
    sum[0] = '\0';
    wifi_summary(&sb, &s->wifi);
    csv_prefix(&b, s, index);
    put(&b, "wifi,%s,,,,,,,,,,", sc_verdict_name(sc_wifi_verdict(&s->wifi)));
    csv_field(&b, sum);
    put(&b, "\n");
    for (int i = 0; s->wifi.scanned && i < s->wifi.n_aps && i < SC_APS; i++) {
        const sc_ap_t *a = &s->wifi.aps[i];
        csv_prefix(&b, s, index);
        put(&b, "wifi_ap,,,,,,,%d,,", a->rssi);
        csv_field(&b, a->ssid);
        put(&b, ",%u,%s\n", a->channel, a->open ? "open" : "password");
    }

    const sc_lorawan_t *nets[2] = {&s->ttn, &s->helium};
    const char *names[2] = {"ttn", "helium"};
    for (int n = 0; n < 2; n++) {
        const sc_lorawan_t *l = nets[n];
        sb = (buf_t){sum, sizeof sum, 0};
        sum[0] = '\0';
        lorawan_summary(&sb, l);
        csv_prefix(&b, s, index);
        put(&b, "%s,%s,,,,,,,,,,", names[n], sc_verdict_name(sc_lorawan_verdict(l)));
        csv_field(&b, sum);
        put(&b, "\n");
        for (int i = 0; l->joined && i < SC_SFS; i++) {
            if (l->sent[i] == 0) {
                continue;
            }
            csv_prefix(&b, s, index);
            put(&b, "%s_sf,,%d,%u,%u,", names[n], sc_sf(i), l->sent[i], l->answered[i]);
            if (l->margin[i] >= 0) {
                put(&b, "%d,%u,", l->margin[i], l->gateways[i]);
            } else {
                put(&b, ",,");
            }
            if (l->rssi[i] != 0) {
                put(&b, "%d,%.1f,,,\n", l->rssi[i], l->snr_x10[i] / 10.0);
            } else {
                put(&b, ",,,,\n");
            }
        }
    }
    return b.len;
}

size_t sc_list_line(const sc_spot_t *s, int index, char *out, size_t cap)
{
    buf_t b = {out, cap, 0};
    if (cap > 0) {
        out[0] = '\0';
    }
    char t[32] = "";
    if (s->when) {
        eastern(s->when, t, sizeof t);
    }
    put(&b, "%d|", index);
    for (const char *p = s->site; *p; p++) {
        put(&b, "%c", *p == '|' || *p == '\n' ? '/' : *p);
    }
    put(&b, "|");
    for (const char *p = s->spot; *p; p++) {
        put(&b, "%c", *p == '|' || *p == '\n' ? '/' : *p);
    }
    put(&b, "|%s|%s|%s|%s\n", t, sc_verdict_name(sc_wifi_verdict(&s->wifi)),
        sc_verdict_name(sc_lorawan_verdict(&s->ttn)), sc_verdict_name(sc_lorawan_verdict(&s->helium)));
    return b.len;
}

// ---- Form bodies --------------------------------------------------------------

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

int sc_form_get(const char *body, size_t len, const char *name, char *out, size_t cap)
{
    size_t nlen = strlen(name);
    size_t i = 0;
    while (i < len) {
        size_t end = i;
        while (end < len && body[end] != '&') {
            end++;
        }
        if (end - i > nlen && body[i + nlen] == '=' && memcmp(body + i, name, nlen) == 0) {
            size_t o = 0;
            for (size_t k = i + nlen + 1; k < end; k++) {
                char c = body[k];
                if (c == '+') {
                    c = ' ';
                } else if (c == '%' && k + 2 < end) {
                    int h = hexval(body[k + 1]);
                    int l = hexval(body[k + 2]);
                    if (h >= 0 && l >= 0) {
                        c = (char)(h * 16 + l);
                        k += 2;
                    }
                }
                if (o + 1 >= cap) {
                    return -1;
                }
                out[o++] = c;
            }
            if (cap > 0) {
                out[o] = '\0';
            }
            return (int)o;
        }
        if (end - i == nlen && memcmp(body + i, name, nlen) == 0) {  // "name" alone
            if (cap > 0) {
                out[0] = '\0';
            }
            return 0;
        }
        i = end + 1;
    }
    return -1;
}

bool sc_hex(const char *s, uint8_t *out, size_t n)
{
    if (strlen(s) != 2 * n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        int h = hexval(s[2 * i]), l = hexval(s[2 * i + 1]);
        if (h < 0 || l < 0) {
            return false;
        }
        out[i] = (uint8_t)(h * 16 + l);
    }
    return true;
}

// A decimal number with up to 6 decimals ("-75.123456") as value x 1e6.
static bool parse_e6(const char *s, long min_e6, long max_e6, int32_t *out)
{
    bool neg = false;
    if (*s == '-' || *s == '+') {
        neg = *s == '-';
        s++;
    }
    if (!isdigit((unsigned char)*s)) {
        return false;
    }
    long whole = 0;
    while (isdigit((unsigned char)*s)) {
        whole = whole * 10 + (*s++ - '0');
        if (whole > 1000) {
            return false;
        }
    }
    long frac = 0;
    int digits = 0;
    if (*s == '.') {
        s++;
        while (isdigit((unsigned char)*s)) {
            if (digits < 6) {
                frac = frac * 10 + (*s - '0');
                digits++;
            }
            s++;
        }
    }
    if (*s != '\0') {
        return false;
    }
    while (digits++ < 6) {
        frac *= 10;
    }
    long v = whole * 1000000 + frac;
    if (neg) {
        v = -v;
    }
    if (v < min_e6 || v > max_e6) {
        return false;
    }
    *out = (int32_t)v;
    return true;
}

static bool parse_uint(const char *s, unsigned long max, unsigned long *out)
{
    if (!isdigit((unsigned char)*s)) {
        return false;
    }
    unsigned long v = 0;
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s)) {
            return false;
        }
        v = v * 10 + (unsigned long)(*s - '0');
        if (v > max) {
            return false;
        }
    }
    *out = v;
    return true;
}

static bool printable(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20) {
            return false;
        }
    }
    return true;
}

const char *sc_parse_spot(const char *body, size_t len, sc_spot_t *s)
{
    char v[96];
    sc_spot_t t = *s;
    int n;
    if ((n = sc_form_get(body, len, "site", v, sizeof v)) >= 0) {
        if (n >= (int)sizeof t.site || !printable(v)) {
            return "site";
        }
        strcpy(t.site, v);
    }
    if ((n = sc_form_get(body, len, "spot", v, sizeof v)) >= 0) {
        if (n >= (int)sizeof t.spot || !printable(v)) {
            return "spot";
        }
        strcpy(t.spot, v);
    }
    if ((n = sc_form_get(body, len, "notes", v, sizeof v)) >= 0) {
        if (n >= (int)sizeof t.notes || !printable(v)) {
            return "notes";
        }
        strcpy(t.notes, v);
    }
    bool has_lat = sc_form_get(body, len, "lat", v, sizeof v) > 0;
    if (has_lat && !parse_e6(v, -90000000L, 90000000L, &t.lat_e6)) {
        return "lat";
    }
    bool has_lon = sc_form_get(body, len, "lon", v, sizeof v) > 0;
    if (has_lon && !parse_e6(v, -180000000L, 180000000L, &t.lon_e6)) {
        return "lon";
    }
    if (has_lat != has_lon) {
        return has_lat ? "lon" : "lat";
    }
    if (has_lat) {
        t.has_pos = true;
    }
    unsigned long u;
    if (sc_form_get(body, len, "acc", v, sizeof v) > 0) {
        if (!parse_uint(v, 65535, &u)) {
            return "acc";
        }
        t.acc_m = (uint16_t)u;
    }
    if (sc_form_get(body, len, "height_cm", v, sizeof v) > 0) {
        if (!parse_uint(v, 10000, &u)) {
            return "height_cm";
        }
        t.height_cm = (uint16_t)u;
    }
    if (sc_form_get(body, len, "enclosure", v, sizeof v) > 0) {
        if (strcmp(v, "yes") == 0) {
            t.enclosure = 1;
        } else if (strcmp(v, "no") == 0) {
            t.enclosure = 0;
        } else {
            return "enclosure";
        }
    }
    if (sc_form_get(body, len, "when", v, sizeof v) > 0) {
        if (!parse_uint(v, 4294967295UL, &u)) {
            return "when";
        }
        t.when = (uint32_t)u;
    }
    *s = t;
    return NULL;
}

unsigned sc_parse_tests(const char *body, size_t len)
{
    char v[64];
    if (sc_form_get(body, len, "tests", v, sizeof v) <= 0) {
        return 0;
    }
    unsigned mask = 0;
    char *save = NULL;
    for (char *tok = strtok_r(v, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (strcmp(tok, "all") == 0) {
            mask |= SC_T_ALL;
        } else if (strcmp(tok, "wifi") == 0) {
            mask |= SC_T_WIFI;
        } else if (strcmp(tok, "ttn") == 0) {
            mask |= SC_T_TTN;
        } else if (strcmp(tok, "helium") == 0) {
            mask |= SC_T_HELIUM;
        } else {
            return 0;
        }
    }
    return mask;
}
