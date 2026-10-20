# ESP32 legacy serial/TCP control protocol (Arduino IDE, firmware v2)

Transport: USB serial ASCII text, 115200 baud, 8 data bits, 1 stop bit, no parity. Commands are one per line. Firmware accepts `\n` and `\r\n`.

The same commands and responses are available over TCP port 3333 after Wi-Fi provisioning. ESP32 discovery beacons use UDP port 3334 with payload `PHONEPILOT_ESP32 port=3333`.

The password-protected `PhonePilot-Setup-XXXXXX` access point at `192.168.4.1` is a recovery path. On upgraded boards the v0.4 password remains in NVS. On fresh boards, set an 8–63 character password over physical USB serial with `SETUP_PASSWORD <password>` and then send `SETUP_AP` or hold BOOT for 3 seconds. The password is never printed. TCP port 3333 on the setup AP accepts configuration; configuration commands from the ordinary shared LAN are rejected.

Commands:

- `PING` -> `PONG`
- `STATUS` -> servo status plus `DRIVE_STATE ready=1 direction=stop|forward|backward`
- `DRIVE FORWARD` / `DRIVE BACKWARD` -> set both L298N bridges; refresh while held
- `DRIVE STOP` -> disable both bridges immediately (coasting stop)
- `CENTER` -> move to configured center
- `ANGLE <n>` -> set absolute angle
- `S<n>` -> canonical compact command used by Android
- `WIFI_CONFIG <base64-ssid> <base64-password>` -> validate and try generated Android hotspot credentials, then persist on success
- `WIFI_STATUS` -> `WIFI STATUS state=<state> ip=<address>`
- `MODE_STATUS` -> `MODE USB|HOTSPOT|SHARED`
- `CONFIG USB` -> save USB-only mode
- `CONFIG HOTSPOT <base64-ssid> <base64-password>` -> save phone-hotspot mode and credentials
- `CONFIG SHARED <base64-ssid> <base64-password>` -> save shared-router mode and credentials
- `SETUP_PASSWORD <password>` -> physical USB only; set a private setup AP password without echoing it
- `SETUP_AP` -> physical USB only; start the protected recovery AP

`CONFIG USB` returns `OK CONFIG_SAVED mode=USB` after NVS commit. Wi-Fi configuration returns `OK CONFIG_ACCEPTED mode=<mode>` before the Wi-Fi attempt, then `OK CONFIG_SAVED mode=<mode>` only after successful connection and NVS commit. On timeout it returns `ERR WIFI_TIMEOUT` and restores the previous working profile. `WIFI_CONFIG` remains a legacy alias for hotspot provisioning.

Successful motion returns `OK ANGLE <n>` only after the target has been accepted by the servo driver. Invalid/not-ready commands return `ERR <code> ...`.

Android must not assume one USB read equals one response. It buffers partial packets until newline and can parse multiple lines from one packet. On port open it sends `STATUS`; `ready=1` is required before web controls become available.

Firmware source: `../PhonePilotRobot_ESP32/PhonePilotRobot_ESP32.ino`. Configuration is in the adjacent `board_config.h`. These are ordinary Arduino sketch tabs and contain no PlatformIO dependency.

Wi-Fi provisioning responses:

```text
OK CONFIG_ACCEPTED mode=HOTSPOT
OK CONFIG_SAVED mode=HOTSPOT
ERR WIFI_INVALID_CONFIG
ERR WIFI_SAVE_FAILED
ERR WIFI_TIMEOUT
```

Serial Monitor also reports `WIFI CONNECTING mode=... ssid=...` and `WIFI CONNECTED mode=... ip=...` without printing the Wi-Fi password. The setup-AP TCP socket stays open for the final result when possible. Older Android setup clients wait only five seconds for `CONFIG_SAVED`, so a slow Wi-Fi join may time out in that client even if the board succeeds later; the BLE v2 status provides the authoritative outcome.

SSID and password are Base64 encoded only to make the line protocol safe for spaces/non-ASCII bytes; Base64 is not encryption. Send credentials only over physical USB or the protected setup AP. The sketch does not log the shared-network or hotspot password.

Angles are clamped by firmware. The drive watchdog disables the L298N outputs after 700 ms without a fresh FORWARD/BACKWARD command; STATUS and servo commands do not refresh it. Direction changes include a 100 ms disabled interval. Motion is never restored from NVS. `DRIVE_STATE` is sent at boot, on STATUS and whenever the actual output direction changes. Existing servo-only Android parsers may ignore the extra status line.
