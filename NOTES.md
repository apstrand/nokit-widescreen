# Instant on computer

Based on OLIMEX esp32p4 with a 6.25inch 720x1560 IPS capacitive touch display



Display: https://www.waveshare.com/6.25inch-dsi-lcd.htm?srsltid=AfmBOoodj1cXJ192xlun9L4I3CCfr3atRyFUubcsOoZufZWbGNzGg-6K
   wiki: https://www.waveshare.com/wiki/6.25inch_DSI_LCD

Board: https://www.olimex.com/Products/IoT/ESP32-P4/ESP32-P4-DevKit/open-source-hardware


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

