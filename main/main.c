#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "board_config.h"
#include "panel.h"
#include "font.h"

static const char *TAG = "widescreen";

#define FB_SIZE      (LCD_H_RES * LCD_V_RES * sizeof(px_t))

#define COLOR_BLACK  PX(0,   0,   0)
#define COLOR_WHITE  PX(255, 255, 255)
#define COLOR_RED    PX(255, 0,   0)
#define COLOR_GREEN  PX(0,   255, 0)
#define COLOR_BLUE   PX(0,   0,   255)
#define COLOR_YELLOW PX(255, 255, 0)
#define COLOR_CYAN   PX(0,   255, 255)

static void fb_fill_rect(px_t *fb, int x, int y, int w, int h, px_t color)
{
    if (x < 0)             { w += x; x = 0; }
    if (y < 0)             { h += y; y = 0; }
    if (x + w > LCD_H_RES) { w = LCD_H_RES - x; }
    if (y + h > LCD_V_RES) { h = LCD_V_RES - y; }
    if (w <= 0 || h <= 0)  return;
    for (int row = y; row < y + h; row++) {
        px_t *p = fb + row * LCD_H_RES + x;
        for (int col = 0; col < w; col++) {
            p[col] = color;
        }
    }
}

// Draw a single character at (x,y) scaled by `scale`, clipped to framebuffer.
static void fb_draw_char(px_t *fb, int x, int y, char ch, int scale,
                         px_t fg, px_t bg)
{
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8_t *glyph = font8x8[(uint8_t)(ch - 0x20)];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            px_t color = (bits >> col) & 1 ? fg : bg;
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

static void fb_draw_string(px_t *fb, int x, int y, const char *s, int scale,
                            px_t fg, px_t bg)
{
    for (; *s; s++, x += 8 * scale)
        fb_draw_char(fb, x, y, *s, scale, fg, bg);
}

#define TOUCH_MARKER_R  24  // half-size of touch crosshair

static void fb_draw_touch_marker(px_t *fb, int cx, int cy, px_t color)
{
    fb_fill_rect(fb, cx - TOUCH_MARKER_R, cy - 2, TOUCH_MARKER_R * 2, 4, color);
    fb_fill_rect(fb, cx - 2, cy - TOUCH_MARKER_R, 4, TOUCH_MARKER_R * 2, color);
}

void app_main(void)
{
    ESP_LOGI(TAG, "%s + %s (%dx%d)", BOARD_NAME, DISPLAY_NAME, LCD_H_RES, LCD_V_RES);
    esp_lcd_panel_handle_t panel;
    px_t *fb = NULL;
    ESP_ERROR_CHECK(panel_start(&panel, (void **)&fb));

    panel_set_backlight(100);
    esp_lcd_touch_handle_t tp = panel_touch_init();
    ESP_LOGI(TAG, "Starting animation");

    // Bouncing rectangle: 300x150, cycling through colours on each bounce.
    const int rw = 300, rh = 150;
    int rx = (LCD_H_RES - rw) / 2;
    int ry = (LCD_V_RES - rh) / 2;
    int vx = 4, vy = 6;

    const px_t colors[] = { COLOR_WHITE, COLOR_RED, COLOR_GREEN,
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
        if (tp) {
            esp_lcd_touch_read_data(tp);
            esp_lcd_touch_get_coordinates(tp, tx, ty, NULL, &ntouch, 5);
        }

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
