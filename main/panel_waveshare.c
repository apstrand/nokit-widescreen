// Waveshare DSI panels (6.25inch DSI LCD and friends): the esp_lcd_dsi
// component sets the panel up over I2C_NUM_1 and the board drives backlight
// and reset directly; touch is a GT911.
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_dsi.h"
#include "esp_lcd_touch_gt911.h"

#include "board_config.h"
#include "panel.h"

static const char *TAG = "panel_ws";

static uint16_t s_touch_x_native;
static uint16_t s_touch_y_native;

esp_err_t panel_start(esp_lcd_panel_handle_t *out_panel, void **out_fb)
{
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy), TAG, "LDO");

    esp_lcd_dsi_bus_handle_t dsi_bus;
    esp_lcd_dsi_bus_config_t bus_cfg = LCD_DSI_BUS_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus), TAG, "dsi bus");

    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_dbi_io_config_t io_cfg = DSI_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &io_cfg, &io_handle), TAG, "dbi io");

    esp_lcd_dpi_panel_config_t dpi_cfg = LCD_DPI_CONFIG();

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
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dsi(io_handle, &panel_cfg, &panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "disp on");

    void *fb = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb), TAG, "fb");

    *out_panel = panel;
    *out_fb = fb;
    return ESP_OK;
}

void panel_set_backlight(uint8_t percent)
{
    static bool inited;
    if (!inited) {
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
        inited = true;
    }
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, percent * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void touch_scale_coords(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                                uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    for (int i = 0; i < *point_num; i++) {
        uint16_t xs = (uint32_t)x[i] * LCD_H_RES / s_touch_x_native;
        uint16_t ys = (uint32_t)y[i] * LCD_V_RES / s_touch_y_native;
#if TOUCH_MIRROR_X
        xs = (LCD_H_RES - 1) - xs;
#endif
#if TOUCH_MIRROR_Y
        ys = (LCD_V_RES - 1) - ys;
#endif
        x[i] = xs;
        y[i] = ys;
    }
}

esp_lcd_touch_handle_t panel_touch_init(void)
{
    // The esp_lcd_dsi component already owns I2C_NUM_1; reuse its bus handle.
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_master_get_bus_handle(I2C_NUM_1, &i2c_bus));

    if (TOUCH_RST_GPIO != GPIO_NUM_NC) {
        // Pulse RST and wait for GT911 to boot (driver only waits 10 ms; chip needs ≥200 ms).
        gpio_set_direction(TOUCH_RST_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(TOUCH_RST_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(TOUCH_RST_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // Probe both GT911 addresses; INT is NC so we can't drive it during reset.
    uint32_t gt911_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;  // 0x5D
    if (i2c_master_probe(i2c_bus, gt911_addr, 50) != ESP_OK) {
        gt911_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP; // 0x14
        ESP_LOGI(TAG, "GT911 not at 0x5D, trying 0x14");
        if (i2c_master_probe(i2c_bus, gt911_addr, 50) != ESP_OK) {
            ESP_LOGW(TAG, "No GT911 found, running without touch");
            return NULL;
        }
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
        esp_err_t err = i2c_master_transmit_receive(dev, reg, 2, buf, 4, 100);
        i2c_master_bus_rm_device(dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GT911 config read failed (%s), running without touch",
                     esp_err_to_name(err));
            return NULL;
        }

        s_touch_x_native = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        s_touch_y_native = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
        ESP_LOGI(TAG, "GT911 native res: %"PRIu16"x%"PRIu16, s_touch_x_native, s_touch_y_native);
    }
    if (s_touch_x_native == 0 || s_touch_y_native == 0) {
        ESP_LOGW(TAG, "GT911 reported a zero resolution, running without touch");
        return NULL;
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
