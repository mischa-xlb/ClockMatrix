#include <string.h>
#include <stdbool.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "config.h"
#include "max7219.h"

static const char *TAG = "max7219";

// ---------------------------------------------------------------------------
// MAX7219 register addresses
// ---------------------------------------------------------------------------
#define REG_NOOP        0x00
#define REG_DIGIT_BASE  0x01  // row 1..8 = registers 0x01..0x08
#define REG_DECODE_MODE 0x09
#define REG_INTENSITY   0x0A
#define REG_SCAN_LIMIT  0x0B
#define REG_SHUTDOWN    0x0C
#define REG_DISP_TEST   0x0F

// ---------------------------------------------------------------------------
// 5-wide x 6-tall digit font (rows 0-5 map to display rows 1-6).
// Top row (reg 1) and bottom row (reg 8) are always blank.
//
// Each digit: 6 bytes.  Pixel columns occupy bits [6:2] of each byte,
// giving one blank column on the left and two on the right:
//
//   bit: 7  6  5  4  3  2  1  0
//        0 [c0 c1 c2 c3 c4] 0  0
//
// So the 5-bit column pattern is simply shifted left by 2.
// ---------------------------------------------------------------------------
static const uint8_t FONT[10][6] = {
    // 0: 01110 / 10001 / 10001 / 10001 / 10001 / 01110
    { 0x38, 0x44, 0x44, 0x44, 0x44, 0x38 },
    // 1:  00100 / 01100 / 00100 / 00100 / 00100 / 01110
    { 0x10, 0x30, 0x10, 0x10, 0x10, 0x38 },
    // 2:  01110 / 10001 / 00010 / 00100 / 01000 / 11111
    { 0x38, 0x44, 0x08, 0x10, 0x20, 0x7C },
    // 3:  01110 / 10001 / 00110 / 00001 / 10001 / 01110
    { 0x38, 0x44, 0x18, 0x04, 0x44, 0x38 },
    // 4:  00010 / 00110 / 01010 / 11111 / 00010 / 00010
    { 0x08, 0x18, 0x28, 0x7C, 0x08, 0x08 },
    // 5:  11111 / 10000 / 11110 / 00001 / 10001 / 01110
    { 0x7C, 0x40, 0x78, 0x04, 0x44, 0x38 },
    // 6:  01110 / 10000 / 11110 / 10001 / 10001 / 01110
    { 0x38, 0x40, 0x78, 0x44, 0x44, 0x38 },
    // 7:  11111 / 00001 / 00010 / 00100 / 00100 / 00100
    { 0x7C, 0x04, 0x08, 0x10, 0x10, 0x10 },
    // 8:  01110 / 10001 / 01110 / 10001 / 10001 / 01110
    { 0x38, 0x44, 0x38, 0x44, 0x44, 0x38 },
    // 9:  01110 / 10001 / 10001 / 01111 / 00001 / 01110
    { 0x38, 0x44, 0x44, 0x3C, 0x04, 0x38 },
};

// ---------------------------------------------------------------------------
// Frame buffer: fb[module][row], rows 0-7.
// Rows 0 and 7 are kept at 0x00 (blank top/bottom lines).
// Rows 1-6 hold the digit pixel data.
// ---------------------------------------------------------------------------
static uint8_t fb[MAX7219_NUM_MODULES][8];
static bool    s_colon = false;   // colon state applied during refresh_digits()

static spi_device_handle_t spi_dev;

// ---------------------------------------------------------------------------
// Low-level: send one 16-bit word per module in a single CS-low transaction.
// tx[0] is sent first and ends up in the LAST module in the chain.
// tx[NUM_MODULES-1] is sent last and ends up in the FIRST module (nearest MCU).
// ---------------------------------------------------------------------------
static void spi_send(uint8_t *tx, int len_bytes)
{
    spi_transaction_t t = {
        .length    = len_bytes * 8,
        .tx_buffer = tx,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_dev, &t));
}

// ---------------------------------------------------------------------------
// Send one register command to every module (all get the same value).
// ---------------------------------------------------------------------------
static void max7219_write_all(uint8_t reg, uint8_t data)
{
    uint8_t buf[MAX7219_NUM_MODULES * 2];
    for (int i = 0; i < MAX7219_NUM_MODULES; i++) {
        buf[i * 2]     = reg;
        buf[i * 2 + 1] = data;
    }
    spi_send(buf, sizeof(buf));
}

// ---------------------------------------------------------------------------
// Push one display row (register row_reg, 1-indexed) from the frame buffer
// out to all modules in a single transaction.
//
// Module index 0 = leftmost panel on the display.
// In the SPI buffer, the farthest module's data goes first.
// ---------------------------------------------------------------------------
static void flush_row(uint8_t row_reg)
{
    uint8_t buf[MAX7219_NUM_MODULES * 2];
    int row = row_reg - 1; // 0-indexed into fb

    for (int i = 0; i < MAX7219_NUM_MODULES; i++) {
#if MAX7219_REVERSE_CHAIN
        int mod = i;                           // leftmost module first in buf
#else
        int mod = (MAX7219_NUM_MODULES - 1) - i; // rightmost module first in buf
#endif
        buf[i * 2]     = row_reg;
        buf[i * 2 + 1] = fb[mod][row];
    }
    spi_send(buf, sizeof(buf));
}

// ---------------------------------------------------------------------------
// Flush the entire frame buffer to the display (all 8 rows).
// ---------------------------------------------------------------------------
static void flush_all(void)
{
    for (uint8_t r = 1; r <= 8; r++) {
        flush_row(r);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void max7219_init(void)
{
    memset(fb, 0, sizeof(fb));

    ESP_LOGI(TAG, "--- max7219_init begin ---");
    ESP_LOGI(TAG, "Pins: MOSI=GPIO%d  CLK=GPIO%d  CS=GPIO%d",
             MAX7219_PIN_MOSI, MAX7219_PIN_CLK, MAX7219_PIN_CS);
    ESP_LOGI(TAG, "SPI host=%d  clock=%d Hz  modules=%d  reverse_chain=%d",
             MAX7219_SPI_HOST, MAX7219_SPI_CLOCK,
             MAX7219_NUM_MODULES, MAX7219_REVERSE_CHAIN);

    // -- SPI bus --
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = MAX7219_PIN_MOSI,
        .miso_io_num     = -1,             // not used
        .sclk_io_num     = MAX7219_PIN_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = MAX7219_NUM_MODULES * 2,
    };
    ESP_LOGI(TAG, "Calling spi_bus_initialize...");
    ESP_ERROR_CHECK(spi_bus_initialize(MAX7219_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "spi_bus_initialize OK");

    // -- SPI device (MAX7219 uses Mode 0, MSB first) --
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = MAX7219_SPI_CLOCK,
        .mode           = 0,
        .spics_io_num   = MAX7219_PIN_CS,
        .queue_size     = 1,
    };
    ESP_LOGI(TAG, "Calling spi_bus_add_device...");
    ESP_ERROR_CHECK(spi_bus_add_device(MAX7219_SPI_HOST, &dev_cfg, &spi_dev));
    ESP_LOGI(TAG, "spi_bus_add_device OK");

    // -- Wake up all modules --
    ESP_LOGI(TAG, "Writing REG_DISP_TEST=0x00 (test OFF)...");
    max7219_write_all(REG_DISP_TEST,   0x00); // display test OFF
    ESP_LOGI(TAG, "Writing REG_DECODE_MODE=0x00 (raw pixel)...");
    max7219_write_all(REG_DECODE_MODE, 0x00); // raw pixel mode (no BCD decode)
    ESP_LOGI(TAG, "Writing REG_SCAN_LIMIT=0x07 (all 8 rows)...");
    max7219_write_all(REG_SCAN_LIMIT,  0x07); // scan all 8 rows
    ESP_LOGI(TAG, "Writing REG_INTENSITY=0x%02X...", MAX7219_INTENSITY);
    max7219_write_all(REG_INTENSITY,   MAX7219_INTENSITY);
    ESP_LOGI(TAG, "Writing REG_SHUTDOWN=0x01 (normal op)...");
    max7219_write_all(REG_SHUTDOWN,    0x01); // normal operation
    ESP_LOGI(TAG, "Shutdown register written — modules should now be active");

    // Blank every row to start
    ESP_LOGI(TAG, "Blanking all rows...");
    for (uint8_t r = 1; r <= 8; r++) {
        ESP_LOGI(TAG, "  Blanking row %d...", r);
        max7219_write_all(r, 0x00);
    }
    ESP_LOGI(TAG, "All rows blanked");

    ESP_LOGI(TAG, "Initialised %d module(s)", MAX7219_NUM_MODULES);
}

void max7219_put_digit(uint8_t module, uint8_t digit)
{
    if (module >= MAX7219_NUM_MODULES || digit > 9) return;
    // Font rows 0-5 map to fb rows 1-6 (leaves row 0 and row 7 untouched).
    for (int i = 0; i < 6; i++) {
        fb[module][i + 1] = FONT[digit][i];
    }
}

void max7219_put_blank(uint8_t module)
{
    if (module >= MAX7219_NUM_MODULES) return;
    for (int i = 1; i <= 6; i++) {
        fb[module][i] = 0x00;
    }
}

void max7219_set_colon(bool visible)
{
    s_colon = visible;
}

void max7219_refresh_digits(void)
{
    // Apply colon state to module 1, rightmost two columns (bit 1 = 0x02).
    // Rows fb[1][2] and fb[1][5] give a nicely spaced upper/lower dot pair.
    // Clear first so toggling off actually removes the dots.
    //fb[1][2] = (fb[1][2] & ~0x02u) | (s_colon ? 0x02u : 0x00u);
    //fb[1][5] = (fb[1][5] & ~0x02u) | (s_colon ? 0x02u : 0x00u);

    fb[1][2] = (fb[1][2] & ~0x01u) | (s_colon ? 0x01u : 0x00u);
    fb[1][5] = (fb[1][5] & ~0x01u) | (s_colon ? 0x01u : 0x00u);

    // Flush only the digit rows (registers 2-7) in one pass.
    for (uint8_t r = 2; r <= 7; r++) {
        flush_row(r);
    }
}

void max7219_set_digit(uint8_t module, uint8_t digit)
{
    max7219_put_digit(module, digit);
    max7219_refresh_digits();
}

// ---------------------------------------------------------------------------
// Seconds progress bar on the bottom row (register 8 = fb[][7]).
//
// 32 LEDs span the bottom row of all 4 modules.  We use LEDs 1-30.
//
// LED numbering (1 = leftmost):
//   LED 1-8   -> module 0, bits 7-0
//   LED 9-16  -> module 1, bits 7-0
//   LED 17-24 -> module 2, bits 7-0
//   LED 25-32 -> module 3, bits 7-0
// ---------------------------------------------------------------------------
void max7219_set_seconds_bar(uint8_t second)
{
    uint8_t first_led, last_led;

    if (second == 0) {
        first_led = 1; last_led = 0;       // nothing lit
    } else if (second <= 30) {
        first_led = 1; last_led = second;  // fill left→right
    } else {
        // right edge fixed at 30, left edge retreats right
        // 31→[2,30]  32→[3,30] … 59→[30,30]
        first_led = second - 29; last_led = 30;
    }

    for (int m = 0; m < MAX7219_NUM_MODULES; m++) {
        uint8_t row_byte = 0;
        for (int bit = 7; bit >= 0; bit--) {
            uint8_t led = (uint8_t)(m * 8 + (7 - bit) + 1); // 1-indexed
            if (led >= first_led && led <= last_led) {
                row_byte |= (uint8_t)(1 << bit);
            }
        }
        fb[m][7] = row_byte;
    }
    flush_row(8); // register 8 = bottom row
}

void max7219_clear_all(void)
{
    memset(fb, 0, sizeof(fb));
    flush_all();
}

void max7219_set_intensity(uint8_t intensity)
{
    if (intensity > 15) intensity = 15;
    max7219_write_all(REG_INTENSITY, intensity);
}

// ---------------------------------------------------------------------------
// Animation helpers — write into the frame buffer for one module.
// Caller must call max7219_refresh_digits() after updating all modules.
// ---------------------------------------------------------------------------

// Scroll: old digit slides down off the bottom, new digit enters from the top.
//
// Font rows 0-5 normally map to fb rows 1-6.
// At frame f (1..ANIM_FRAMES_SCROLL):
//   old digit row i -> fb row (1 + f + i)   [slides down by f]
//   new digit row i -> fb row (f - 5 + i)   [enters from above]
//
// old_digit / new_digit may be MAX7219_BLANK (10) to suppress rendering.
void max7219_anim_scroll(uint8_t module, uint8_t old_digit, uint8_t new_digit, int frame)
{
    if (module >= MAX7219_NUM_MODULES) return;

    // Clear digit rows
    for (int r = 1; r <= 6; r++) fb[module][r] = 0;

    // Old digit sliding down
    if (old_digit <= 9) {
        for (int i = 0; i < 6; i++) {
            int fb_row = 1 + frame + i;
            if (fb_row >= 1 && fb_row <= 6) {
                fb[module][fb_row] = FONT[old_digit][i];
            }
        }
    }

    // New digit entering from above
    if (new_digit <= 9) {
        for (int i = 0; i < 6; i++) {
            int fb_row = frame - 5 + i;
            if (fb_row >= 1 && fb_row <= 6) {
                fb[module][fb_row] |= FONT[new_digit][i];
            }
        }
    }
}

// Explode: old digit shrinks to a dot then bursts outward in concentric rings.
//
// Pixel columns use bits [6:2]: bit6=left, bit5, bit4=centre(0x10), bit3, bit2=right.
// Digit occupies fb rows 1-6 (6 tall); vertical centre is between rows 3-4.
//
// Frame 1: shrink to 4 rows  — font rows 1-4 at fb rows 2-5
// Frame 2: shrink to 2 rows  — font rows 2-3 at fb rows 3-4
// Frame 3: dot               — centre column (0x10) at rows 3,4
// Frame 4: ring 1            — rows 2,5 = 0x10 ; rows 3,4 = 0x28 (±1 col)
// Frame 5: ring 2            — rows 1,6 = 0x10 ; rows 2,5 = 0x28 ; rows 3,4 = 0x44 (±2 col)
// Frame 6: ring 3 (corners)  — rows 1,6 = 0x44
// Frame 7: blank             — new digit shown by caller on next tick
void max7219_anim_explode(uint8_t module, uint8_t old_digit, uint8_t new_digit, int frame)
{
    if (module >= MAX7219_NUM_MODULES) return;
    (void)new_digit; // new digit is shown by the caller after the animation ends

    // Clear digit rows
    for (int r = 1; r <= 6; r++) fb[module][r] = 0;

    switch (frame) {
        case 1: // shrink to 4 rows (font rows 1-4 → fb rows 2-5)
            if (old_digit <= 9) {
                for (int i = 1; i <= 4; i++) {
                    fb[module][i + 1] = FONT[old_digit][i];
                }
            }
            break;

        case 2: // shrink to 2 rows (font rows 2-3 → fb rows 3-4)
            if (old_digit <= 9) {
                fb[module][3] = FONT[old_digit][2];
                fb[module][4] = FONT[old_digit][3];
            }
            break;

        case 3: // dot — centre column only
            fb[module][3] = 0x10;
            fb[module][4] = 0x10;
            break;

        case 4: // ring 1
            fb[module][2] = 0x10;
            fb[module][3] = 0x28;
            fb[module][4] = 0x28;
            fb[module][5] = 0x10;
            break;

        case 5: // ring 2
            fb[module][1] = 0x10;
            fb[module][2] = 0x28;
            fb[module][3] = 0x44;
            fb[module][4] = 0x44;
            fb[module][5] = 0x28;
            fb[module][6] = 0x10;
            break;

        case 6: // ring 3 — corner pixels only
            fb[module][1] = 0x44;
            fb[module][6] = 0x44;
            break;

        case 7: // blank — next tick will show the new digit
            break;

        default:
            break;
    }
}
