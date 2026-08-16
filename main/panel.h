// Panel bring-up, backlight and touch — implemented per display type
// (panel_waveshare.c / panel_rpi.c, picked by Kconfig in CMakeLists.txt).
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

// Brings the DSI panel up and hands back its frame buffer: LCD_H_RES *
// LCD_V_RES pixels of type px_t (see board_config.h).
esp_err_t panel_start(esp_lcd_panel_handle_t *out_panel, void **out_fb);

// Backlight brightness, 0-100 %.
void panel_set_backlight(uint8_t percent);

// Returns NULL when no touch controller answers — the panel is still usable.
esp_lcd_touch_handle_t panel_touch_init(void);
