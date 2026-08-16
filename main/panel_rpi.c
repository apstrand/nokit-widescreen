// Raspberry Pi 7"-architecture DSI panels — the Waveshare 4.3inch DSI QLED is
// one: an ATTINY88 at I2C 0x45 doing power/backlight/reset, a Toshiba TC358762
// DSI-to-DPI bridge, and an FT5x06 touch controller at 0x38. Everything
// (DSI lanes, panel power, I2C) runs through the 15-pin Pi FPC.
//
// The Waveshare esp_lcd_dsi component cannot drive these: it writes a register
// set that only Waveshare's own panel MCU understands, and it never configures
// the bridge, so the panel stays dark. This file follows the Linux drivers
// instead:
//   drivers/regulator/rpi-panel-attiny-regulator.c  (power-on, firmware 0xC3)
//   drivers/gpu/drm/bridge/tc358762.c               (bridge register writes)
//   drivers/gpu/drm/panel/panel-raspberrypi-touchscreen.c (modeline)
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "driver/i2c_master.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_ft5x06.h"
#include "hal/mipi_dsi_hal.h"
#include "hal/mipi_dsi_host_ll.h"
#include "hal/mipi_dsi_types.h"

#include "board_config.h"
#include "panel.h"

static const char *TAG = "panel_rpi";

#define PANEL_MCU_ADDR      0x45
#define TOUCH_ADDR          0x38
#define PANEL_I2C_PORT      I2C_NUM_0
// 100 kHz: the ATTINY is bit-banging its I2C slave and does not keep up with
// 400 kHz — reads return NACK. The board has external pull-ups.
#define PANEL_I2C_FREQ_HZ   100000

// ATTINY88 registers (rpi-panel-attiny-regulator.c).
enum {
    REG_ID        = 0x80,   // 0xC3 = v1.1 firmware, 0xDE = older
    REG_PORTA     = 0x81,
    REG_PORTB     = 0x82,
    REG_PORTC     = 0x83,
    REG_PWM       = 0x86,   // backlight duty, 0..255
    REG_ADDR_L    = 0x8c,   // bridge register address, low byte  \  SPI proxy to
    REG_ADDR_H    = 0x8d,   //   ... high byte                     > the TC358762
    REG_WR_DATA_H = 0x90,   // bridge write data, high byte       /  before DSI is
    REG_WR_DATA_L = 0x91,   //   ... low byte                     /  up
};

#define PA_LCD_LR       (1u << 2)   // horizontal scan direction
#define PB_LCD_MAIN     (1u << 7)   // main regulator enable
#define PC_LED_EN       (1u << 0)
#define PC_RST_TP_N     (1u << 1)
#define PC_RST_LCD_N    (1u << 2)
#define PC_RST_BRIDGE_N (1u << 3)

// TC358762 registers (tc358762.c). Written as DSI generic long writes with a
// 6-byte payload: [reg_lo, reg_hi, val_b0..val_b3].
#define TC_PPI_STARTPPI         0x0104
#define TC_PPI_LPTXTIMECNT      0x0114
#define TC_PPI_D0S_ATMR         0x0144
#define TC_PPI_D1S_ATMR         0x0148
#define TC_PPI_D0S_CLRSIPOCOUNT 0x0164
#define TC_PPI_D1S_CLRSIPOCOUNT 0x0168
#define TC_DSI_STARTDSI         0x0204
#define TC_DSI_LANEENABLE       0x0210
#define TC_LCDCTRL              0x0420
#define TC_LCD_HS_HBP           0x0424
#define TC_LCD_HDISP_HFP        0x0428
#define TC_LCD_VS_VBP           0x042c
#define TC_LCD_VDISP_VFP        0x0430
#define TC_SPICMR               0x0450
#define TC_SYSCTRL              0x0464
#define TC_SYSPMCTRL            0x047c

// Mirror of the private esp_lcd_dsi_bus_t (esp-idf components/esp_lcd/dsi/
// mipi_dsi_priv.h). ESP-IDF's DBI IO only emits DCS packets, but the bridge
// only accepts generic ones, so we need the HAL context behind the handle.
typedef struct {
    int                    bus_id;
    mipi_dsi_hal_context_t hal;
} dsi_bus_shadow_t;

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_mcu;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;

// ------------------------------------------------------------ ATTINY88 ----

static esp_err_t mcu_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_mcu, buf, sizeof(buf), 50);
}

// The ATTINY wants the register address and the data phase as two separate
// transactions with a short gap, like the Linux driver does.
static esp_err_t mcu_read(uint8_t reg, uint8_t *out)
{
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_mcu, &reg, 1, 50), TAG, "addr");
    esp_rom_delay_us(200);
    return i2c_master_receive(s_mcu, out, 1, 50);
}

static esp_err_t mcu_attach(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = PANEL_I2C_PORT,
        .sda_io_num        = PANEL_I2C_SDA_GPIO,
        .scl_io_num        = PANEL_I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "i2c bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PANEL_MCU_ADDR,
        .scl_speed_hz    = PANEL_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_mcu),
                        TAG, "i2c dev 0x45");

    // Straight after a reset the ATTINY needs a moment before it answers, and
    // it NACKs rather than stretching the clock, so retry the ID read.
    uint8_t id = 0;
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 20 && err != ESP_OK; i++) {
        if (i) vTaskDelay(pdMS_TO_TICKS(20));
        err = mcu_read(REG_ID, &id);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "panel MCU @0x45 unreachable — check the FPC");
    if (id != 0xC3 && id != 0xDE) {
        ESP_LOGW(TAG, "unexpected panel MCU firmware id 0x%02x", id);
    } else {
        ESP_LOGI(TAG, "panel MCU firmware id 0x%02x", id);
    }
    return ESP_OK;
}

// Power up the panel, leaving the bridge in reset until the DSI HS clock runs.
static esp_err_t panel_power_on(void)
{
    ESP_RETURN_ON_ERROR(mcu_write(REG_PORTC, 0x00), TAG, "PORTC");   // all resets asserted
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(mcu_write(REG_PORTA, PA_LCD_LR), TAG, "PORTA");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(mcu_write(REG_PORTB, PB_LCD_MAIN), TAG, "PORTB");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(mcu_write(REG_PORTC, PC_LED_EN), TAG, "PORTC LED");
    vTaskDelay(pdMS_TO_TICKS(80));
    ESP_RETURN_ON_ERROR(mcu_write(REG_PWM, 0x00), TAG, "PWM");
    return ESP_OK;
}

// Release the bridge reset and take it out of low-power mode. SYSPMCTRL is
// written through the ATTINY's SPI proxy because the bridge does not listen on
// DSI yet.
static esp_err_t bridge_release_and_wake(void)
{
    ESP_RETURN_ON_ERROR(mcu_write(REG_PORTC, PC_LED_EN | PC_RST_LCD_N | PC_RST_BRIDGE_N),
                        TAG, "PORTC bridge release");
    vTaskDelay(pdMS_TO_TICKS(10));

    const struct { uint8_t reg, val; } proxy[] = {
        { REG_ADDR_H,    (TC_SYSPMCTRL >> 8) & 0xff },
        { REG_ADDR_L,    TC_SYSPMCTRL & 0xff },
        { REG_WR_DATA_H, 0x00 },
        { REG_WR_DATA_L, 0x00 },
    };
    for (int i = 0; i < 4; i++) {
        ESP_RETURN_ON_ERROR(mcu_write(proxy[i].reg, proxy[i].val), TAG, "SPI proxy");
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

// ------------------------------------------------------------ TC358762 ----

static void bridge_write(uint16_t reg, uint32_t val)
{
    dsi_bus_shadow_t *bus = (dsi_bus_shadow_t *)s_dsi_bus;
    uint8_t payload[6] = {
        (uint8_t)(reg & 0xff),         (uint8_t)(reg >> 8),
        (uint8_t)(val & 0xff),         (uint8_t)((val >> 8) & 0xff),
        (uint8_t)((val >> 16) & 0xff), (uint8_t)((val >> 24) & 0xff),
    };
    mipi_dsi_hal_host_gen_write_long_packet(&bus->hal, 0,
        MIPI_DSI_DT_GENERIC_LONG_WRITE, payload, sizeof(payload));
}

static void bridge_init(void)
{
    bridge_write(TC_DSI_LANEENABLE,       (1u << 0) | (1u << 1));  // clock + D0
    bridge_write(TC_PPI_D0S_CLRSIPOCOUNT, 0x05);
    bridge_write(TC_PPI_D1S_CLRSIPOCOUNT, 0x05);
    bridge_write(TC_PPI_D0S_ATMR,         0x00);
    bridge_write(TC_PPI_D1S_ATMR,         0x00);
    bridge_write(TC_PPI_LPTXTIMECNT,      0x03);
    bridge_write(TC_SPICMR,               0x00);
    bridge_write(TC_LCDCTRL,              0x00100150);  // VSDELAY=1 | RGB888 | VTGEN
    bridge_write(TC_SYSCTRL,              0x040f);
    bridge_write(TC_LCD_HS_HBP,    ((uint32_t)LCD_HBP << 16) | LCD_HSW);
    bridge_write(TC_LCD_HDISP_HFP, ((uint32_t)LCD_HFP << 16) | LCD_H_RES);
    bridge_write(TC_LCD_VS_VBP,    ((uint32_t)LCD_VBP << 16) | LCD_VSW);
    bridge_write(TC_LCD_VDISP_VFP, ((uint32_t)LCD_VFP << 16) | LCD_V_RES);
    vTaskDelay(pdMS_TO_TICKS(100));
    bridge_write(TC_PPI_STARTPPI, 0x01);
    bridge_write(TC_DSI_STARTDSI, 0x01);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "TC358762 bridge configured");
}

// ----------------------------------------------------------------- API ----

esp_err_t panel_start(esp_lcd_panel_handle_t *out_panel, void **out_fb)
{
    ESP_RETURN_ON_ERROR(mcu_attach(), TAG, "panel MCU");
    ESP_RETURN_ON_ERROR(panel_power_on(), TAG, "panel power");

    esp_ldo_channel_handle_t phy_pwr = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr), TAG, "LDO");

    esp_lcd_dsi_bus_config_t bus_cfg = LCD_DSI_BUS_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus), TAG, "dsi bus");

    // Creating (and dropping) a DBI IO latches LP speed mode for generic
    // writes; the bridge only accepts commands during low-power intervals.
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &dbi_io), TAG, "dbi io");
    esp_lcd_panel_io_del(dbi_io);

    // RGB565 frame buffer, RGB888 on the link — the bridge and the panel
    // behind it are 24-bit, the DSI bridge does the conversion for us.
    esp_lcd_dpi_panel_config_t dpi_cfg = LCD_DPI_CONFIG();
    esp_lcd_panel_handle_t panel;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(s_dsi_bus, &dpi_cfg, &panel), TAG, "dpi panel");

    // Two ESP-IDF defaults the TC358762 disagrees with: it needs non-burst
    // video (its DPI clock is locked to the link) and it never acks frames.
    dsi_bus_shadow_t *bus = (dsi_bus_shadow_t *)s_dsi_bus;
    mipi_dsi_host_ll_dpi_set_video_burst_type(bus->hal.host,
        MIPI_DSI_LL_VIDEO_NON_BURST_WITH_SYNC_PULSES);
    mipi_dsi_host_ll_dpi_enable_frame_ack(bus->hal.host, false);

    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");

    void *fb = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb), TAG, "fb");

    // Continuous HS clock: the bridge's FLL needs a stable reference. LP
    // blanking stays enabled so command packets fit between frames.
    mipi_dsi_host_ll_set_clock_lane_state(bus->hal.host, MIPI_DSI_LL_CLOCK_LANE_STATE_HS);
    mipi_dsi_host_ll_enable_cmd_ack(bus->hal.host, false);

    // Only now, with the HS clock running, will the bridge see LP commands.
    ESP_RETURN_ON_ERROR(bridge_release_and_wake(), TAG, "bridge wake");
    bridge_init();

    // Release the touch controller's reset now that the panel is scanning.
    mcu_write(REG_PORTC, PC_LED_EN | PC_RST_TP_N | PC_RST_LCD_N | PC_RST_BRIDGE_N);

    ESP_LOGI(TAG, "DSI up: %dx%d, %d MHz, %d lane(s) @ %d Mbps",
             LCD_H_RES, LCD_V_RES, LCD_DPI_CLOCK_MHZ, LCD_DSI_LANES, LCD_LANE_RATE_MBPS);

    *out_panel = panel;
    *out_fb = fb;
    return ESP_OK;
}

void panel_set_backlight(uint8_t percent)
{
    if (percent > 100) percent = 100;
    esp_err_t err = mcu_write(REG_PWM, (uint8_t)(percent * 255 / 100));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight write failed: %s", esp_err_to_name(err));
    }
}

esp_lcd_touch_handle_t panel_touch_init(void)
{
    if (i2c_master_probe(s_i2c_bus, TOUCH_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "No FT5x06 at 0x%02x, running without touch", TOUCH_ADDR);
        return NULL;
    }

    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io));

    // x_max/y_max are the controller's own ranges: mirroring happens before the
    // swap, so with swapped axes they are the panel's dimensions the other way
    // round.
    esp_lcd_touch_config_t tp_cfg = {
#if TOUCH_SWAP_XY
        .x_max        = LCD_V_RES,
        .y_max        = LCD_H_RES,
#else
        .x_max        = LCD_H_RES,
        .y_max        = LCD_V_RES,
#endif
        .rst_gpio_num = GPIO_NUM_NC,   // driven by the panel MCU
        .int_gpio_num = GPIO_NUM_NC,
        .flags = {
            .swap_xy  = TOUCH_SWAP_XY,
            .mirror_x = TOUCH_MIRROR_X,
            .mirror_y = TOUCH_MIRROR_Y,
        },
    };
    esp_lcd_touch_handle_t tp;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &tp));
    ESP_LOGI(TAG, "FT5x06 touch ready");
    return tp;
}
