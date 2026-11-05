// A status screen for the commissioning probes: a 128x64 SSD1306 or SH1106 on its own I2C bus, five
// lines of 21 characters (Terminus 6x12). Optional: without a panel every call
// does nothing.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_LINES 5
#define OLED_COLS 21

// Finds the panel at 0x3C or 0x3D on I2C port 1 and turns it on; false when
// none answers.
bool oled_start(int sda, int scl);

// Sets line n (0 - 4), printf style, cut to OLED_COLS, and redraws the screen.
void oled_line(int n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Blanks the panel (before deep sleep).
void oled_off(void);

#ifdef __cplusplus
}
#endif
