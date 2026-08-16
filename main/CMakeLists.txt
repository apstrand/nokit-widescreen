set(srcs "main.c")

# The two panel families need entirely different bring-up: Waveshare's own DSI
# panels go through the esp_lcd_dsi component, the Raspberry-Pi-architecture
# ones (4.3inch QLED) through the ATTINY/TC358762 sequence.
if(CONFIG_NOKIT_DISPLAY_WS_QLED_4_3)
    list(APPEND srcs "panel_rpi.c")
else()
    list(APPEND srcs "panel_waveshare.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_driver_ledc esp_lcd esp_hw_support esp_mm esp_timer esp_hal_lcd
)
