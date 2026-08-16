# Instant on computer

Based on OLIMEX esp32p4 with a 6.25inch 720x1560 IPS capacitive touch display



Display: https://www.waveshare.com/6.25inch-dsi-lcd.htm?srsltid=AfmBOoodj1cXJ192xlun9L4I3CCfr3atRyFUubcsOoZufZWbGNzGg-6K
   wiki: https://www.waveshare.com/wiki/6.25inch_DSI_LCD
Alt display: [Waveshare 4.3" DSI                                
QLED](https://www.waveshare.com/4.3inch-dsi-qled.htm) display

Board: https://www.olimex.com/Products/IoT/ESP32-P4/ESP32-P4-DevKit/open-source-hardware
Alt board: https://docs.waveshare.com/ESP32-P4-WIFI6


## Selecting board + display

Board and panel are menuconfig options ("Nokit widescreen hardware"), with
ready-made fragments in `config/`. Combine them through `SDKCONFIG_DEFAULTS`
(delete `sdkconfig` first — defaults only apply to options it doesn't have yet):

```
rm -f sdkconfig
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;config/board-waveshare.defaults;config/display-4.3-qled.defaults" build
idf.py -p /dev/ttyACM0 flash monitor
```

Combinations:

| board | display |
|---|---|
| `config/board-olimex.defaults` (Olimex ESP32-P4-DevKit) | `config/display-6.25.defaults` (Waveshare 6.25" DSI LCD) |
| `config/board-waveshare.defaults` (Waveshare ESP32-P4-WIFI6) | `config/display-4.3-qled.defaults` (Waveshare 4.3" DSI QLED) |

Both boards wire the panel the same way: everything (DSI lanes, panel power,
I2C at SDA GPIO7 / SCL GPIO8) goes through the Pi-style DSI FPC. The Waveshare
board also has an ES8311 audio codec at 0x18 on that same I2C bus.

Status: Waveshare board + 4.3" QLED verified working on hardware (image and
touch). The Olimex + 6.25" combination is build-checked only since the change.

## The two panels need different drivers

They only look like the same product family:

* **6.25" DSI LCD** — Waveshare's own architecture. The `esp_lcd_dsi`
  component pokes the panel MCU (I2C 0x45, registers 0xc0/0xc2/0xac/0xab/
  0xaa/0xad) and drives the panel directly over 2 DSI lanes. `panel_waveshare.c`.
* **4.3" DSI QLED** — a Raspberry Pi 7" panel clone: ATTINY88 at I2C 0x45
  (firmware id 0xC3 in `REG_ID` 0x80) doing power/backlight/resets, a Toshiba
  TC358762 DSI-to-DPI bridge, and an FT5x06 touch controller at 0x38 — no
  GT911. `panel_rpi.c`, ported from the Linux drivers
  `rpi-panel-attiny-regulator.c`, `tc358762.c` and
  `panel-raspberrypi-touchscreen.c`.

The Waveshare component cannot drive the QLED at all: its register writes mean
nothing to the ATTINY and it never configures the bridge, so the panel stays
completely dark. That is the earlier failure.

### QLED bring-up gotchas (all of these bit us)

* **Bridge config needs DSI *generic* long writes** (DT 0x29). ESP-IDF's DBI IO
  only emits DCS packets, so `panel_rpi.c` reaches the HAL through a shadow
  copy of the private `esp_lcd_dsi_bus_t` and calls
  `mipi_dsi_hal_host_gen_write_long_packet()` directly. Check that struct
  (`components/esp_lcd/dsi/mipi_dsi_priv.h`) after an IDF upgrade.
* **Frame buffer must be RGB888.** Before chip revision 3 the ESP32-P4 DSI
  bridge keeps input and output pixel format in the *same* register field
  (`mipi_dsi_brg_ll_set_output_color_format` overwrites `raw_type`), so
  RGB565-in/RGB888-out silently makes it read the buffer as RGB888 — the
  symptom is a uniformly blue screen. Drawing code therefore works in `px_t`
  (see `board_config.h`), 3 bytes here and 2 bytes on the 6.25".
* **Single lane, non-burst, no frame ack.** The bridge is 1-lane only, needs
  `NON_BURST_WITH_SYNC_PULSES` and a continuous HS clock, and stalls if the
  host waits for frame acks — ESP-IDF's defaults are wrong on all three, so
  they're overridden via `mipi_dsi_host_ll_*` after `esp_lcd_new_panel_dpi()`.
* **ATTINY I2C is slow and rude**: 100 kHz only (400 kHz NACKs), address and
  data phases as separate transactions, and it needs a retry loop right after
  reset before it answers at all.
* Touch reports landscape coordinates but both axes mirrored relative to the
  panel scan direction (`TOUCH_MIRROR_X/Y` in `board_config.h`).

Failure-mode hints: MCU unreachable at 0x45 → FPC seating; I2C acks but the
screen stays dark → bridge init; image but shifted/torn → the `LCD_HSW/HBP/HFP`
and `LCD_VSW/VBP/VFP` timings in `board_config.h`.

## Fix waveshare esp_lcd_dsi.c

It's out of date for esp-idf 5.3+ and need fixes from 
  https://github.com/apstrand/fork-waveshare-esp32-components.git
To do local development check out above repo and add an idf dependency override to main/idf_component.yml:

```
  esp_lcd_dsi:
    override_path: "../../fork-waveshare-esp32-components/display/lcd/esp_lcd_dsi"
```

Note: `override_path` must be the ONLY field under the dependency — adding it alongside `git`+`path` causes git to win and the override is silently ignored.

run `idf.py reconfigure` after every change to the fork

(Only the 6.25" path uses this component; the 4.3" QLED does not touch it.)
