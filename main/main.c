#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_dsi.h"

static const char *TAG = "widescreen";

#define LCD_H_RES   720
#define LCD_V_RES   1560

#define LCD_BL_GPIO     GPIO_NUM_26
#define LCD_RST_GPIO    GPIO_NUM_27

#define MIPI_DSI_PHY_LDO_CHAN   3
#define MIPI_DSI_PHY_LDO_MV     2500

#define BYTES_PER_PIXEL 2
#define FB_SIZE         (LCD_H_RES * LCD_V_RES * BYTES_PER_PIXEL)

#define RGB565(r, g, b) ((uint16_t)(((r) & 0x1F) << 11 | ((g) & 0x3F) << 5 | ((b) & 0x1F)))
#define COLOR_BLACK  RGB565(0,  0,  0)
#define COLOR_WHITE  RGB565(31, 63, 31)
#define COLOR_RED    RGB565(31, 0,  0)
#define COLOR_GREEN  RGB565(0,  63, 0)
#define COLOR_BLUE   RGB565(0,  0,  31)
#define COLOR_YELLOW RGB565(31, 63, 0)
#define COLOR_CYAN   RGB565(0,  63, 31)

static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t chan = {
        .gpio_num   = LCD_BL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_1,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&chan));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void fb_fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{
    for (int row = y; row < y + h; row++) {
        uint16_t *p = fb + row * LCD_H_RES + x;
        for (int col = 0; col < w; col++) {
            p[col] = color;
        }
    }
}

void app_main(void)
{
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy));

    esp_lcd_dsi_bus_handle_t dsi_bus;
    esp_lcd_dsi_bus_config_t bus_cfg = DSI_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus));

    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_dbi_io_config_t io_cfg = DSI_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &io_cfg, &io_handle));

    esp_lcd_dpi_panel_config_t dpi_cfg = DSI_PANEL_DPI_6_25_INCH_CONFIG(LCD_COLOR_FMT_RGB565);

    dsi_vendor_config_t vendor_cfg = {
        .mipi_config = { .dsi_bus = dsi_bus, .dpi_config = &dpi_cfg },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST_GPIO,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_cfg,
    };

    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(esp_lcd_new_panel_dsi(io_handle, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    uint16_t *fb = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, (void **)&fb));

    backlight_init();
    ESP_LOGI(TAG, "Starting animation");

    // Bouncing rectangle: 300x150, cycling through colours on each bounce.
    const int rw = 300, rh = 150;
    int rx = (LCD_H_RES - rw) / 2;
    int ry = (LCD_V_RES - rh) / 2;
    int vx = 4, vy = 6;

    const uint16_t colors[] = { COLOR_WHITE, COLOR_RED, COLOR_GREEN,
                                 COLOR_BLUE, COLOR_YELLOW, COLOR_CYAN };
    const int ncolors = sizeof(colors) / sizeof(colors[0]);
    int color_idx = 0;

    uint32_t frame_count = 0;
    int64_t fps_window_start = esp_timer_get_time();

    while (1) {
        // Erase previous rect, move, draw new rect.
        fb_fill_rect(fb, rx, ry, rw, rh, COLOR_BLACK);

        rx += vx;
        ry += vy;

        bool bounced = false;
        if (rx <= 0)             { rx = 0;              vx = -vx; bounced = true; }
        if (rx + rw >= LCD_H_RES){ rx = LCD_H_RES - rw; vx = -vx; bounced = true; }
        if (ry <= 0)             { ry = 0;              vy = -vy; bounced = true; }
        if (ry + rh >= LCD_V_RES){ ry = LCD_V_RES - rh; vy = -vy; bounced = true; }

        if (bounced) {
            color_idx = (color_idx + 1) % ncolors;
        }

        fb_fill_rect(fb, rx, ry, rw, rh, colors[color_idx]);
        ESP_ERROR_CHECK(esp_cache_msync(fb, FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M));

        frame_count++;
        int64_t now = esp_timer_get_time();
        if (now - fps_window_start >= 1000000) {
            float fps = frame_count * 1e6f / (now - fps_window_start);
            ESP_LOGI(TAG, "FPS: %.1f", fps);
            frame_count = 0;
            fps_window_start = now;
        }
    }
}
