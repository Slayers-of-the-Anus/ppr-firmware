# PhonePilot firmware: ESP32 and ESP8266

## Каталог прошивок для Arduino IDE

| Файл скетча | Плата | Призначення / вихід |
| --- | --- | --- |
| [PhonePilotRobot_ESP32.ino](PhonePilotRobot_ESP32/PhonePilotRobot_ESP32.ino) | ESP32 | Основна прошивка v2: USB, Wi-Fi, BLE, серво GPIO18 та L298N |
| [ESP32_L298N_MotorCycleTest.ino](ESP32_L298N_MotorCycleTest/ESP32_L298N_MotorCycleTest.ino) | ESP32 | Циклічний тест L298N: вперед / пауза / назад; IN1–IN4: GPIO25/26/27/33, ENA/ENB: GPIO32/23 |
| [ESP32_GPIO18_ServoCycleTest.ino](ESP32_GPIO18_ServoCycleTest/ESP32_GPIO18_ServoCycleTest.ino) | ESP32 | Плавний циклічний рух серво 60–120°, сигнал GPIO18 |
| [ESP32_GPIO19_MotorPulseTest.ino](ESP32_GPIO19_MotorPulseTest/ESP32_GPIO19_MotorPulseTest.ino) | ESP32 | Імпульси керування двигуном 1400/1418/1374 мкс, 100 або 200 Гц, сигнал GPIO19 |
| [ESP8266_GPIO5_MotorPulseTest.ino](ESP8266_GPIO5_MotorPulseTest/ESP8266_GPIO5_MotorPulseTest.ino) | ESP8266 | Ті самі імпульси, сигнал GPIO5 / D1 на NodeMCU та Wemos D1 mini |

Кожен скетч збережений в окремій папці з тією самою назвою, як вимагає Arduino IDE. Відкривай потрібний `.ino` разом із його папкою: для основної прошивки потрібні також усі сусідні `.h` файли. Схеми підключення та параметри тестів описані в README відповідних папок. Тестові скетчі автоматично запускають рух після ввімкнення; вони призначені для ручної перевірки власником і не позначені як перевірені стабільні релізи. Зібрані `.bin` файли не публікуються — прошивки компілюються та завантажуються власником через Arduino IDE.

## Main robot firmware v2

Arduino IDE sketch in `PhonePilotRobot_ESP32/` for the original ESP32, servo, USB serial, Wi-Fi control, and BLE provisioning. This is the previous `PhonePilotServo/PhonePilotServo.ino` under a name that reflects the complete robot functionality.

## Toolchain

- Arduino IDE 2.x, **esp32 by Espressif Systems 3.3.11**, original **ESP32 Dev Module** target.
- Bundled ESP32 core **BLE** library (Bluedroid); do not install a second BLE library. The sketch requires authenticated MITM GATT writes and rejects other stacks at preprocessing.
- **ESP32Servo 3.2.1** from Library Manager. `cJSON`, `Preferences`, `WiFi`, and `mbedtls` come with the core.

These versions match the local Arduino installation inspected for APIs. **The firmware was not compiled or uploaded in this task.** The owner should use Arduino IDE to verify and upload.

## BLE first use

1. Power the board and open Serial Monitor at 115200 baud. Servo signal defaults to GPIO18; give the servo a separate supply with common ground.
2. The board advertises at every boot as `PhonePilot-ESP32-XXXXXX`. Search by service UUID `6c40cb73-3396-4ec6-bcc0-cb2db37ace10` and verify the full 12-character eFuse `deviceId` from status. The name is only a display hint.
3. An unbonded board allows pairing for 60 seconds and prints its unique six-digit PIN for that pairing session. Enter it on the phone. Hold **BOOT for 3 seconds after normal startup** to reopen a 60-second pairing window. Bonded phones reconnect without a new PIN.
4. Command characteristic: `6c40cb73-3396-4ec6-bcc0-cb2db37ace11`, encrypted authenticated bonded Write With Response. Status characteristic: `6c40cb73-3396-4ec6-bcc0-cb2db37ace12`, Read + Notify. Secure Connections, MITM, bonding, and MITM GATT permissions are required.
5. The board waits 5 seconds from boot for a phone. If one connects in that interval but sends no configuration, it falls back to the last successful NVS mode after 15 seconds from connection. BLE stays available afterward; servo and USB control start immediately.

BLE carries a stream of `uint16 little-endian JSON length` plus UTF-8 JSON, at most 512 JSON bytes. Each GATT write/notification is a fragment; reassemble at the receiver. The board requests MTU 247 but supports the default 23. Frame validation is in [PhonePilotProtocol.h](PhonePilotRobot_ESP32/PhonePilotProtocol.h); command/status JSON handling is in [PhonePilotRobot_ESP32.ino](PhonePilotRobot_ESP32/PhonePilotRobot_ESP32.ino).

`select_mode` chooses an existing `SHARED` or `HOTSPOT` profile without a password. `set_config` provides new credentials. Separate profiles are stored for the two Wi-Fi modes. `ack.accepted` means processing started; only a later `status` with the same `requestId`, `phase=ready`, and `wifiState=connected` proves Wi-Fi success. A candidate profile is committed to an alternate NVS record only after connection. A 15-second failure restores the previous record and reports `CONNECT_TIMEOUT` with the restored mode. Old v0.4 `mode`, `ssid`, and `password` keys are migrated once without deleting them. Recent duplicate `requestId` values return the cached ACK without reapplying.

## L298N forward/backward wiring

The new drive controls use both L298N bridges together at full output (no speed PWM yet).

| ESP32 pin | Connection |
| --- | --- |
| GPIO25 | L298N IN1 |
| GPIO26 | L298N IN2 |
| GPIO27 | L298N IN3 |
| GPIO33 | L298N IN4 |
| GPIO32 | L298N ENA, remove its jumper |
| GPIO23 | L298N ENB, remove its jumper |
| GPIO18 | Existing servo signal |
| GND | Common ground: ESP32, L298N, motor supply and servo supply |

Connect motor A to OUT1/OUT2, and motor B (if used) to OUT3/OUT4. A single traction motor needs only channel A. Feed the L298N motor-supply terminal from the motor power supply, not from an ESP32 GPIO/3V3. Supply the module's logic according to its regulator/jumper configuration; its 5V terminal must not be connected to an ESP32 GPIO. ENA/ENB jumpers are separate from the module's 5V regulator jumper. Fit 10 kΩ pull-down resistors from ENA and ENB to GND so motors remain disabled while the ESP32 boots or is unpowered. Keep the servo's separate supply and common ground.

The GPIO defaults are in `PhonePilotRobot_ESP32/board_config.h`. If one motor rotates the wrong way, swap that motor's output leads or set its `MOTOR_A_INVERT`/`MOTOR_B_INVERT` flag. First test with the wheels clear of the ground. The L298 input HIGH threshold is 2.3V, so ESP32's 3.3V control signals are compatible ([ST datasheet](https://www.st.com/resource/en/datasheet/l298.pdf)).

Serial/TCP commands: `DRIVE FORWARD`, `DRIVE BACKWARD`, `DRIVE STOP`. `STATUS` additionally returns `DRIVE_STATE ready=1 direction=stop|forward|backward`. Forward/backward must be refreshed while held; after 700 ms without a fresh drive command the ESP32 disables both bridges. STOP also disables both bridges (coasting stop), and reversal has a 100 ms disabled interval. Servo or STATUS traffic never refreshes the drive watchdog. Motor motion is not persisted in NVS or restored after boot. The app does not replay expired drive commands after reconnect.

Upload the updated sketch manually through Arduino IDE, update the Android app, and restart the server to enable the website controls. Legacy servo-only firmware reports no drive capability, so forward/backward stay disabled. `tests/drive_tests.cpp` exercises watchdog, reversal, STOP and millis rollover using a desktop C++ compiler; this is not a firmware build.

## Existing control paths

USB and TCP retain `PING`, `STATUS`, `S<n>`, `ANGLE <n>`, `CENTER`, `WIFI_STATUS`, `MODE_STATUS`, `CONFIG USB`, `CONFIG SHARED <base64-ssid> <base64-password>`, `CONFIG HOTSPOT ...`, and `WIFI_CONFIG ...`. TCP listens on port 3333 when Wi-Fi or the setup AP is active; UDP discovery uses port 3334. `STATUS` reports servo readiness independently of BLE and Wi-Fi.

Legacy Wi-Fi configuration now returns `OK CONFIG_ACCEPTED mode=...` immediately and `OK CONFIG_SAVED mode=...` only after connection and NVS commit. A failed attempt emits `ERR WIFI_TIMEOUT` and keeps the previous profile. Base64 is **not encryption**: send credentials over physical USB or the protected setup AP, never over the ordinary shared LAN. TCP servo commands have no authentication; use them on a trusted network.

The protected setup AP is a recovery path. An upgraded v0.4 board retains its NVS AP password. On a fresh board, choose your own password through physical USB Serial Monitor with `SETUP_PASSWORD <8–63 character password>`, then send `SETUP_AP` or hold BOOT for 3 seconds. The password is stored but never echoed. The AP is `PhonePilot-Setup-XXXXXX` at `192.168.4.1`. Only its own subnet may send configuration over TCP. It shuts down after the board joins Wi-Fi. Keep your chosen password in a password manager.

## Manual checks after Arduino IDE upload

1. Verify the service/characteristic UUIDs, full eFuse ID, PIN pairing, MITM write, and bonded reconnect.
2. Confirm that new pairing is closed after 60 seconds until BOOT is held, and an unbonded write cannot submit credentials.
3. Reboot with a saved Wi-Fi profile but no phone; check the 5-second fallback. Connect a bonded phone during startup without sending a command; check the 15-second fallback.
4. Send a wrong Wi-Fi password; check rollback, `phase=error`, and the restored mode. Send a correct one; check `ready` and reboot persistence.
5. Check USB `STATUS` and `S95`, TCP 3333, UDP 3334, and setup AP recovery.

`tests/protocol_tests.cpp` covers frame boundaries, timeout, length limits, UUID, mode, credentials, duplicate ACK lookup, boot fallback timing, and Wi-Fi success/timeout/retry decisions on a desktop C++ compiler. It does not compile the firmware. See `docs/SERIAL_PROTOCOL.md` for legacy serial/TCP details.
