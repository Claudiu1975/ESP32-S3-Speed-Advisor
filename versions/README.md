# Version archive

Each version is in its own folder, so it opens directly in the Arduino IDE (the folder name matches the `.ino` file). The newest version is also the main `SpeedAdvisor.ino` in the repository root. PS_1 to PS_3 are in the Git history.

| Version | What it adds | Extra libraries / settings |
|---------|--------------|----------------------------|
| [PS_4](SpeedAdvisor_PS_4/SpeedAdvisor_PS_4.ino) | **Wi-Fi captive portal.** The board opens the network "SpeedAdvisor" (password `speedadv`). The phone page shows the live speed and the over-limit warning (red speed, flashing in the buzzer rhythm, vibration, optional sound) and has the button settings: limit, brightness, GPS power save. Wi-Fi switches off after 10 minutes with no phone connected. | None (`WiFi`, `WebServer`, `DNSServer` come with the ESP32 core) |
| [PS_5](SpeedAdvisor_PS_5/SpeedAdvisor_PS_5.ino) | **Display type chosen at run time.** All four drivers are built in. Hold Down and short press Up to move to the next type (OLED 128x32 → OLED 128x64 → ST7735 → ST7789). Also selectable on the Wi-Fi page, and remembered after power-off. Long press Down (GPS power save) now acts on release. | All display libraries must be installed (SSD1306 and ST7735/ST7789) |
| [PS_6](SpeedAdvisor_PS_6/SpeedAdvisor_PS_6.ino) | **Bluetooth LE + Android web app.** Everything the Wi-Fi page does, over Bluetooth, from the app in [`docs/app/`](../docs/app/) (Chrome on Android, installable to the home screen, works offline). Wi-Fi and Bluetooth can run together; `BLE_ENABLE` / `PORTAL_ENABLE` turn each off. | NimBLE-Arduino (2.x recommended); Partition Scheme "Huge APP (3MB No OTA)" |
| [PS_7](SpeedAdvisor_PS_7/SpeedAdvisor_PS_7.ino) | **Tidier phone pages** (Wi-Fi page and Android app). A gear icon opens a Settings view with the display type and GPS power save. The limit card is collapsed and only shows the current limit, with a Show/Hide toggle; it hides again after Set, or 4 s after the last −/+ press. Also includes the fix for "portalStart was not declared" in the Arduino IDE. | Same as PS_6 |

All three versions keep the PS_3 behaviour: the limit in 0.5 km/h steps with short presses, brightness with a long press of Up, GPS power save with a long press of Down, and the alert timing (beep every second, faster after 45 s, continuous at 55 s).

PS_4 to PS_7 were checked only for compile errors against stand-in libraries, not on real hardware. Please test before relying on them.
