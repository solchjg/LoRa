#include "oled.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "sdkconfig.h"
#include "text.h"

#define W 128
#define H 64
#define GLYPH_W 6
#define GLYPH_H 12

extern const uint16_t font6x12[FONT_GLYPHS][GLYPH_H];  // font6x12.c, generated
static const text_font_t font = {GLYPH_W, GLYPH_H, &font6x12[0][0]};

static esp_lcd_panel_handle_t panel;
#ifdef CONFIG_PROBE_OLED_SH1106
static esp_lcd_panel_io_handle_t sh_io;  // the SH1106 is driven by hand: see sh1106_*
#endif
static char lines[OLED_LINES][OLED_COLS + 1];
static uint8_t frame[W * H / 8];  // SSD1306 pages: byte x of page p holds rows 8p - 8p+7
static uint16_t row[W * GLYPH_H];

#ifdef CONFIG_PROBE_OLED_SH1106
// The SH1106 has 132 columns, the 128 visible ones starting at column 2, and
// only page addressing, so the SSD1306 driver's single write shows garbage.
static bool sh1106_cmds(const uint8_t *c, int n)
{
    for (int i = 0; i < n; i++) {
        if (esp_lcd_panel_io_tx_param(sh_io, c[i], NULL, 0) != ESP_OK) {
            return false;
        }
    }
    return true;
}

static bool sh1106_init(void)
{
    static const uint8_t init[] = {
        0xAE,        // display off
        0xD5, 0x80,  // clock
        0xA8, 0x3F,  // 64 rows
        0xD3, 0x00,  // no display offset
        0x40,        // start line 0
        0xAD, 0x8B,  // DC-DC on
        0xA1,        // column 127 at the left, as the SSD1306 driver sets it
        0xC8,        // rows scanned bottom up
        0xDA, 0x12,  // COM pins
        0x81, 0x80,  // contrast
        0xD9, 0x22,  // precharge
        0xDB, 0x35,  // VCOM deselect
        0xA4, 0xA6,  // show RAM, not inverted
        0xAF,        // display on
    };
    return sh1106_cmds(init, sizeof init);
}

static void sh1106_draw(void)
{
    for (int p = 0; p < H / 8; p++) {
        const uint8_t at[] = {(uint8_t)(0xB0 + p), 0x02, 0x10};
        if (!sh1106_cmds(at, sizeof at) ||
            esp_lcd_panel_io_tx_color(sh_io, -1, &frame[p * W], W) != ESP_OK) {
            return;
        }
    }
}
#endif

bool oled_start(int sda, int scl)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bc = {
        .i2c_port = 1,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) {
        return false;
    }
    int addr = i2c_master_probe(bus, 0x3C, 50) == ESP_OK   ? 0x3C
               : i2c_master_probe(bus, 0x3D, 50) == ESP_OK ? 0x3D
                                                            : 0;
    if (addr == 0) {
        printf("oled: none at 0x3c or 0x3d (SDA %d, SCL %d)\n", sda, scl);
        return false;
    }
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t ioc = {
        .dev_addr = addr,
        .scl_speed_hz = 400000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
#ifdef CONFIG_PROBE_OLED_SH1106
    bool ok = esp_lcd_new_panel_io_i2c(bus, &ioc, &io) == ESP_OK;
    sh_io = io;
    ok = ok && sh1106_init();
    printf("oled: sh1106 at 0x%02x %s\n", addr, ok ? "ok" : "CHECK");
    if (ok) {
        panel = (esp_lcd_panel_handle_t)io;  // marks the screen present
    }
    return ok;
#else
    esp_lcd_panel_ssd1306_config_t ssd = {.height = H};
    esp_lcd_panel_dev_config_t pc = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd,
    };
    bool ok = esp_lcd_new_panel_io_i2c(bus, &ioc, &io) == ESP_OK &&
              esp_lcd_new_panel_ssd1306(io, &pc, &panel) == ESP_OK &&
              esp_lcd_panel_reset(panel) == ESP_OK && esp_lcd_panel_init(panel) == ESP_OK &&
              esp_lcd_panel_disp_on_off(panel, true) == ESP_OK;
    printf("oled: ssd1306 at 0x%02x %s\n", addr, ok ? "ok" : "CHECK");
    if (!ok) {
        panel = NULL;
    }
    return ok;
#endif
}

static void draw(void)
{
    memset(frame, 0, sizeof frame);
    for (int n = 0; n < OLED_LINES; n++) {
        text_font_render(&font, lines[n], 1, 0, row, W);
        for (int y = 0; y < GLYPH_H; y++) {
            int py = n * GLYPH_H + y;
            for (int x = 0; x < W; x++) {
                if (row[y * W + x]) {
                    frame[(py / 8) * W + x] |= (uint8_t)(1u << (py % 8));
                }
            }
        }
    }
#ifdef CONFIG_PROBE_OLED_SH1106
    sh1106_draw();
#else
    esp_lcd_panel_draw_bitmap(panel, 0, 0, W, H, frame);
#endif
}

void oled_line(int n, const char *fmt, ...)
{
    if (panel == NULL || n < 0 || n >= OLED_LINES) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lines[n], sizeof lines[n], fmt, ap);
    va_end(ap);
    draw();
}

void oled_off(void)
{
    if (panel != NULL) {
#ifdef CONFIG_PROBE_OLED_SH1106
        const uint8_t off = 0xAE;
        sh1106_cmds(&off, 1);
#else
        esp_lcd_panel_disp_on_off(panel, false);
#endif
    }
}
