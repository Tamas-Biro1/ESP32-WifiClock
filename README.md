# ESP32-S2-mini-GC9A01-WifiClock
This project demonstrates an analogue clock on a GC9A01 round display unit using ESP32-S2 mini system controller.

# Components
- GC9A01 round display (https://www.makerfabs.com/gc9a01-1-28-inch-round-lcd-module.html) - this variant has BLK pin to control the backlight intensity
- ESP32-S2 mini (https://www.wemos.cc/en/latest/s2/s2_mini.html) - S2FN4R2, with 2MB PSRAM and 4MB Flash
- DS3232 battery backed (CR2025) real time clock (I²C)
- BH1750 light sensor (I²C)
- SD Card reader (I²C)

# Features
- NTP synced clock
- offline mode with high precision (2ppm) RTC module
- ambient sensor to fade display brightness at night
- 2 different clock face (Big Ben and Mondaine)
- settings through web UI (with STA mode if there is no AP to connect)

# Circuit
TODO...
