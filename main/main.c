#include <string.h>
#include <stdio.h>
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
#include "driver/i2c_master.h"
#include "esp_lcd_touch_gt911.h"
#include "font.h"

static const char *TAG = "widescreen";

#define LCD_H_RES   720
#define LCD_V_RES   1560

#define LCD_BL_GPIO     GPIO_NUM_26
#define LCD_RST_GPIO    GPIO_NUM_27

#define TOUCH_RST_GPIO  GPIO_NUM_23

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
    if (x < 0)             { w += x; x = 0; }
    if (y < 0)             { h += y; y = 0; }
    if (x + w > LCD_H_RES) { w = LCD_H_RES - x; }
    if (y + h > LCD_V_RES) { h = LCD_V_RES - y; }
    if (w <= 0 || h <= 0)  return;
    for (int row = y; row < y + h; row++) {
        uint16_t *p = fb + row * LCD_H_RES + x;
        for (int col = 0; col < w; col++) {
            p[col] = color;
        }
    }
}

// Draw a single character at (x,y) scaled by `scale`, clipped to framebuffer.
static void fb_draw_char(uint16_t *fb, int x, int y, char ch, int scale,
                         uint16_t fg, uint16_t bg)
{
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8_t *glyph = font8x8[(uint8_t)(ch - 0x20)];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            uint16_t color = (bits >> col) & 1 ? fg : bg;
            for (int sy = 0; sy < scale; sy++) {
                int py = y + row * scale + sy;
                if (py < 0 || py >= LCD_V_RES) continue;
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + col * scale + sx;
                    if (px < 0 || px >= LCD_H_RES) continue;
                    fb[py * LCD_H_RES + px] = color;
                }
            }
        }
    }
}

static void fb_draw_string(uint16_t *fb, int x, int y, const char *s, int scale,
                            uint16_t fg, uint16_t bg)
{
    for (; *s; s++, x += 8 * scale)
        fb_draw_char(fb, x, y, *s, scale, fg, bg);
}

#define TOUCH_MARKER_R  24  // half-size of touch crosshair

static void fb_draw_touch_marker(uint16_t *fb, int cx, int cy, uint16_t color)
{
    fb_fill_rect(fb, cx - TOUCH_MARKER_R, cy - 2, TOUCH_MARKER_R * 2, 4, color);
    fb_fill_rect(fb, cx - 2, cy - TOUCH_MARKER_R, 4, TOUCH_MARKER_R * 2, color);
}

static uint16_t s_touch_x_native;
static uint16_t s_touch_y_native;

static void touch_scale_coords(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                                uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    for (int i = 0; i < *point_num; i++) {
        uint16_t xs = (uint32_t)x[i] * LCD_H_RES / s_touch_x_native;
        x[i] = (LCD_H_RES - 1) - xs;
        uint16_t ys = (uint32_t)y[i] * LCD_V_RES / s_touch_y_native;
        y[i] = (LCD_V_RES - 1) - ys;
    }
}

static esp_lcd_touch_handle_t touch_init(void)
{
    // Waveshare DSI component already owns I2C_NUM_1; reuse its bus handle.
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_master_get_bus_handle(I2C_NUM_1, &i2c_bus));

    // Pulse RST and wait for GT911 to boot (driver only waits 10 ms; chip needs ≥200 ms).
    gpio_set_direction(TOUCH_RST_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(TOUCH_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TOUCH_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    // Probe both GT911 addresses; INT is NC so we can't drive it during reset.
    uint32_t gt911_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;  // 0x5D
    if (i2c_master_probe(i2c_bus, gt911_addr, 50) != ESP_OK) {
        gt911_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP; // 0x14
        ESP_LOGI(TAG, "GT911 not at 0x5D, trying 0x14");
    }

    // Read GT911 native touch resolution from config registers 0x8048–0x804B.
    // We need this to scale raw coordinates to panel pixels.
    {
        i2c_master_dev_handle_t dev;
        i2c_device_config_t dev_cfg = {
            .device_address = gt911_addr,
            .scl_speed_hz   = 400000,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &dev));

        uint8_t reg[2] = {0x80, 0x48};
        uint8_t buf[4] = {0};
        ESP_ERROR_CHECK(i2c_master_transmit_receive(dev, reg, 2, buf, 4, 100));
        i2c_master_bus_rm_device(dev);

        s_touch_x_native = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        s_touch_y_native = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
        ESP_LOGI(TAG, "GT911 native res: %"PRIu16"x%"PRIu16, s_touch_x_native, s_touch_y_native);
    }

    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.dev_addr = gt911_addr;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max               = LCD_H_RES,
        .y_max               = LCD_V_RES,
        .rst_gpio_num        = GPIO_NUM_NC,
        .int_gpio_num        = GPIO_NUM_NC,
        .process_coordinates = touch_scale_coords,
    };
    esp_lcd_touch_handle_t tp;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp));
    return tp;
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
    esp_lcd_touch_handle_t tp = touch_init();
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

    // FPS overlay: 2× scaled, top-left corner, max "FPS: 999.9" = 10 chars × 16px
    const int OVL_X = 4, OVL_Y = 4, OVL_SCALE = 2;
    const int OVL_W = 10 * 8 * OVL_SCALE, OVL_H = 8 * OVL_SCALE;
    char fps_str[32] = "FPS: ---";

    // Previous touch positions so we can erase them next frame
    uint16_t prev_tx[5] = {0}, prev_ty[5] = {0};
    uint8_t  prev_ntouch = 0;

    while (1) {
        // Read touch before moving the box so position is current this frame.
        uint16_t tx[5], ty[5];
        uint8_t ntouch = 0;
        esp_lcd_touch_read_data(tp);
        esp_lcd_touch_get_coordinates(tp, tx, ty, NULL, &ntouch, 5);

        if (prev_ntouch == 0 && ntouch > 0)
            ESP_LOGI(TAG, "Touch down  (%"PRIu16", %"PRIu16")", tx[0], ty[0]);
        else if (prev_ntouch > 0 && ntouch == 0)
            ESP_LOGI(TAG, "Touch up");
        else if (ntouch > 0)
            ESP_LOGI(TAG, "Touch move  (%"PRIu16", %"PRIu16")", tx[0], ty[0]);

        // Erase previous rect.
        fb_fill_rect(fb, rx, ry, rw, rh, COLOR_BLACK);

        if (ntouch > 0) {
            // Center box on first touch point.
            rx = (int)tx[0] - rw / 2;
            ry = (int)ty[0] - rh / 2;
        } else {
            rx += vx;
            ry += vy;
        }

        bool bounced = false;
        if (rx <= 0)             { rx = 0;              vx = -vx; bounced = true; }
        if (rx + rw >= LCD_H_RES){ rx = LCD_H_RES - rw; vx = -vx; bounced = true; }
        if (ry <= 0)             { ry = 0;              vy = -vy; bounced = true; }
        if (ry + rh >= LCD_V_RES){ ry = LCD_V_RES - rh; vy = -vy; bounced = true; }

        // Only cycle colour on a real free-running bounce, not an edge clamp from touch.
        if (bounced && ntouch == 0) {
            color_idx = (color_idx + 1) % ncolors;
        }

        fb_fill_rect(fb, rx, ry, rw, rh, colors[color_idx]);

        // Redraw FPS overlay every frame.
        fb_fill_rect(fb, OVL_X, OVL_Y, OVL_W, OVL_H, COLOR_BLACK);
        fb_draw_string(fb, OVL_X, OVL_Y, fps_str, OVL_SCALE, COLOR_WHITE, COLOR_BLACK);

        // Erase previous touch markers, draw current ones.
        for (int i = 0; i < prev_ntouch; i++)
            fb_draw_touch_marker(fb, prev_tx[i], prev_ty[i], COLOR_BLACK);
        for (int i = 0; i < ntouch; i++) {
            fb_draw_touch_marker(fb, tx[i], ty[i], COLOR_YELLOW);
            prev_tx[i] = tx[i];
            prev_ty[i] = ty[i];
        }
        prev_ntouch = ntouch;

        ESP_ERROR_CHECK(esp_cache_msync(fb, FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M));

        // esp_cache_msync holds a critical section for ~2ms, so taskYIELD() isn't
        // enough — app_main is always ready and gets immediately rescheduled.
        // Block for one tick every 100 frames so IDLE0 can run and reset the WDT.
        if (++frame_count % 100 == 0) {
            vTaskDelay(1);
        }
        int64_t now = esp_timer_get_time();
        if (now - fps_window_start >= 1000000) {
            float fps = frame_count * 1e6f / (now - fps_window_start);
            snprintf(fps_str, sizeof(fps_str), "FPS: %.1f", fps);
            ESP_LOGI(TAG, "%s", fps_str);
            frame_count = 0;
            fps_window_start = now;
        }
    }
}
