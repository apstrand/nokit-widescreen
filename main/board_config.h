// Board and panel wiring / timing, selected through menuconfig
// ("Nokit widescreen hardware", see NOTES.md).
#pragma once

#include <stdint.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_lcd_mipi_dsi.h"

// ---------------------------------------------------------------- board ----
//
// Both boards drive the panel through the Raspberry-Pi-style DSI FPC, which
// also carries the panel's I2C bus (SDA GPIO7 / SCL GPIO8 on both boards) and
// the panel's power.

#define PANEL_I2C_SDA_GPIO  GPIO_NUM_7
#define PANEL_I2C_SCL_GPIO  GPIO_NUM_8

#if CONFIG_NOKIT_BOARD_OLIMEX_P4_DEVKIT

#define BOARD_NAME      "Olimex ESP32-P4-DevKit"
#define LCD_BL_GPIO     GPIO_NUM_26
#define LCD_RST_GPIO    GPIO_NUM_27
#define TOUCH_RST_GPIO  GPIO_NUM_23

#elif CONFIG_NOKIT_BOARD_WAVESHARE_P4_WIFI6

#define BOARD_NAME      "Waveshare ESP32-P4-WIFI6"
#define LCD_BL_GPIO     GPIO_NUM_26
#define LCD_RST_GPIO    GPIO_NUM_27
// The touch controller on the Pi-style panels is reset by the panel itself.
#define TOUCH_RST_GPIO  GPIO_NUM_NC

#else
#error "No board selected -- run idf.py menuconfig"
#endif

// -------------------------------------------------------------- display ----

#define MIPI_DSI_PHY_LDO_CHAN   3
#define MIPI_DSI_PHY_LDO_MV     2500

#if CONFIG_NOKIT_DISPLAY_WS_6_25

#define DISPLAY_NAME    "Waveshare 6.25inch DSI LCD"
#define LCD_H_RES       720
#define LCD_V_RES       1560

// Timings from the esp_lcd_dsi component (DSI_PANEL_DPI_6_25_INCH_CONFIG).
#define LCD_DPI_CLOCK_MHZ   48
#define LCD_HSW             50
#define LCD_HBP             50
#define LCD_HFP             50
#define LCD_VSW             20
#define LCD_VBP             20
#define LCD_VFP             20

#define LCD_DSI_LANES       2
#define LCD_LANE_RATE_MBPS  1250
#define LCD_COLOR_FMT       LCD_COLOR_FMT_RGB565

// Panel is mounted upside down relative to the touch controller's origin.
#define TOUCH_SWAP_XY   0
#define TOUCH_MIRROR_X  1
#define TOUCH_MIRROR_Y  1

#elif CONFIG_NOKIT_DISPLAY_WS_QLED_4_3

#define DISPLAY_NAME    "Waveshare 4.3inch DSI QLED"
#define LCD_H_RES       800
#define LCD_V_RES       480

// Raspberry Pi 7" architecture (TC358762 bridge, see panel_rpi.c). The Linux
// modeline is 25.9794 MHz with HFP=1; dpi_clock_freq_mhz is whole MHz here and
// the long front porch is what has been observed to run stable on ESP32-P4.
// These are the first knobs to revisit if the image is shifted or tears.
#define LCD_DPI_CLOCK_MHZ   26
#define LCD_HSW             2
#define LCD_HBP             46
#define LCD_HFP             210
#define LCD_VSW             20
#define LCD_VBP             4
#define LCD_VFP             22

// The bridge is single-lane, and both it and the panel are 24-bit. The frame
// buffer has to be RGB888 too: before chip revision 3 the ESP32-P4 DSI bridge
// keeps input and output pixel format in the same register field, so asking
// for RGB565-in/RGB888-out just makes it read the buffer as RGB888.
#define LCD_DSI_LANES       1
#define LCD_LANE_RATE_MBPS  600
#define LCD_COLOR_FMT       LCD_COLOR_FMT_RGB888

// The FT5x06 already reports landscape coordinates (0..799, 0..479), but the
// panel scans the other way round (PA_LCD_LR in panel_rpi.c), so both axes are
// mirrored: touching the top-left corner reads as raw (744, 437).
#define TOUCH_SWAP_XY   0
#define TOUCH_MIRROR_X  1
#define TOUCH_MIRROR_Y  1

#else
#error "No display selected -- run idf.py menuconfig"
#endif

// ----------------------------------------------------------- pixel type ----
//
// The frame buffer layout follows LCD_COLOR_FMT, so drawing code works in
// `px_t` and builds colours with PX(r, g, b) using 8-bit components.

#if LCD_COLOR_FMT == LCD_COLOR_FMT_RGB888
typedef struct __attribute__((packed)) { uint8_t b, g, r; } px_t;
#define PX(r8, g8, b8)  ((px_t){ .b = (b8), .g = (g8), .r = (r8) })
#else
typedef uint16_t px_t;
#define PX(r8, g8, b8)  ((px_t)((((r8) & 0xF8) << 8) | (((g8) & 0xFC) << 3) | ((b8) >> 3)))
#endif

#define LCD_DSI_BUS_CONFIG() {                       \
        .bus_id = 0,                                 \
        .num_data_lanes = LCD_DSI_LANES,             \
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT, \
        .lane_bit_rate_mbps = LCD_LANE_RATE_MBPS,    \
    }

#define LCD_DPI_CONFIG() {                           \
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT, \
        .dpi_clock_freq_mhz = LCD_DPI_CLOCK_MHZ,     \
        .virtual_channel = 0,                        \
        .in_color_format = LCD_COLOR_FMT,            \
        .out_color_format = LCD_COLOR_FMT,           \
        .num_fbs = 1,                                \
        .video_timing = {                            \
            .h_size = LCD_H_RES,                     \
            .v_size = LCD_V_RES,                     \
            .hsync_pulse_width = LCD_HSW,            \
            .hsync_back_porch = LCD_HBP,             \
            .hsync_front_porch = LCD_HFP,            \
            .vsync_pulse_width = LCD_VSW,            \
            .vsync_back_porch = LCD_VBP,             \
            .vsync_front_porch = LCD_VFP,            \
        },                                           \
    }
