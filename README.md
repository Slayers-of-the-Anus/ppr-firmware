# PhonePilot ESP32 Firmware

Independent Arduino IDE sketch for the robot-side ESP32 and MG995 pan servo.

## Current scope

- Arduino IDE 2.x + `esp32` board package by Espressif Systems.
- `ESP32Servo` library from Arduino Library Manager.
- Hobby-servo output on `GPIO18` by default.
- Serial control at `115200` baud.
- Commands are line based and deliberately simple so Android, a PC terminal or another controller can use the same protocol.

Supported commands:

```text
PING
STATUS
CENTER
ANGLE 95
S95
WIFI_STATUS
WIFI_CONFIG <base64-ssid> <base64-password>
MODE_STATUS
CONFIG USB
CONFIG HOTSPOT <base64-ssid> <base64-password>
CONFIG SHARED <base64-ssid> <base64-password>
```

Example response:

```text
PHONEPILOT_ESP32_READY v0.4-arduino
STATUS ready=1 angle=90 min=0 max=180
OK ANGLE 95
```

## Install in Arduino IDE

1. Install Arduino IDE 2.x.
2. In **File -> Preferences -> Additional Boards Manager URLs**, add the stable Espressif URL:

   ```text
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

3. In **Tools -> Board -> Boards Manager**, search for `esp32` and install **esp32 by Espressif Systems**.
4. In **Library Manager**, search for and install **ESP32Servo**.
5. Open `PhonePilotServo/PhonePilotServo.ino`.
6. Select the exact board and COM port. For a common ESP32 DevKit, start with **ESP32 Dev Module**.
7. Check `PhonePilotServo/board_config.h` and adjust GPIO/mechanical limits if necessary.
8. Click **Verify**, then **Upload**. Some boards require holding **BOOT** while upload starts.
9. Open Serial Monitor at `115200`, choose **Newline** or **Both NL & CR**, and send `STATUS` or `S95`.

For ESP32-S2/S3 boards with native USB, enable the board's **USB CDC On Boot** option when required. Classic ESP32 boards commonly expose USB serial through CP210x or CH340 without that option.

## Power warning

Do not power an MG995-class servo from the ESP32 5 V/3.3 V pin. Use a separate servo-capable supply and connect the ESP32 ground and servo supply ground together. The signal line alone goes to the configured GPIO.

## Android integration

Android USB Host integration sends canonical `S<angle>\n` commands and waits for `OK ANGLE <angle>`. It sends `STATUS\n` immediately after opening the serial port. The firmware remains independently buildable/testable and has no network/server dependency.

When PhonePilot creates a local-only hotspot, press the app's send button to transfer its generated credentials through USB. The sketch stores them in ESP32 NVS (`Preferences` namespace `phonepilot`), attempts connection immediately, and retries after board restart or a temporary hotspot outage. Once connected, it advertises `PHONEPILOT_ESP32 port=3333` over UDP port 3334 and accepts the same line protocol over TCP port 3333. Direct USB control remains available.

## Cable-free setup for a shared router

At every boot ESP32 opens a temporary WPA2 network named `PhonePilot-Setup-XXXXXX` while also accepting serial commands. A random 16-character setup password is generated once, saved in NVS, and printed in Arduino Serial Monitor at 115200 baud as `SETUP PASSWORD ...`; note it after flashing. It is not a password of the shared router. Setup network address is `192.168.4.1` and control/provisioning TCP port is 3333.

In Android Setup choose **JOIN SHARED WI-FI**, copy the phone's current Wi-Fi name, enter the router password, select **VIA SETUP WI-FI**, enter the board's setup SSID and password, and press **SEND MODE & NETWORK TO ESP32**. Android shows a system dialog to join the temporary setup network. The app uses a network-specific socket, so server traffic is not deliberately routed through ESP32's setup AP. The board acknowledges `OK CONFIG_SAVED mode=SHARED`, stores the mode and credentials, joins the router, turns off its temporary AP and advertises itself on that router's LAN. On subsequent boots it reconnects using the saved configuration; its temporary AP is also available at startup for reconfiguration. USB provisioning remains possible with the same `CONFIG` command.

For **JOIN PHONE HOTSPOT**, use USB provisioning because Android devices cannot reliably host LocalOnlyHotspot and simultaneously join the ESP32 setup network. **USB ONLY** can be selected by either provisioning path. The ordinary TCP servo protocol has no authentication; use it only on a trusted LAN. Router client isolation or blocked UDP broadcasts can prevent discovery.

Use a data-capable USB OTG connection between phone and ESP32. The MG995 needs a separate high-current supply. Connect supply GND, servo GND and ESP32 GND together; connect only the signal to GPIO18. Never power the servo from an ESP32 3.3 V/5 V pin or from the phone OTG supply.

See `docs/SERIAL_PROTOCOL.md` and the root `../docs/ARCHITECTURE.md`.
