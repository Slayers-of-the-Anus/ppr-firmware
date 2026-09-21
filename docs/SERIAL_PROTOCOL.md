# ESP32 control protocol v0.4 (Arduino IDE)

Transport: USB serial ASCII text, 115200 baud, 8 data bits, 1 stop bit, no parity. Commands are one per line. Firmware accepts `\n` and `\r\n`.

The same commands and responses are available over TCP port 3333 after Wi-Fi provisioning. ESP32 discovery beacons use UDP port 3334 with payload `PHONEPILOT_ESP32 port=3333`.

At boot ESP32 also exposes a password-protected `PhonePilot-Setup-XXXXXX` access point at `192.168.4.1`. Its randomly generated password is printed to the Arduino Serial Monitor and persists in NVS. TCP port 3333 on that network accepts configuration. Network configuration commands from the ordinary shared LAN are rejected; USB and the setup AP can change the mode.

Commands:

- `PING` -> `PONG`
- `STATUS` -> `STATUS ready=<0|1> angle=<n> min=<n> max=<n>`
- `CENTER` -> move to configured center
- `ANGLE <n>` -> set absolute angle
- `S<n>` -> canonical compact command used by Android
- `WIFI_CONFIG <base64-ssid> <base64-password>` -> validate and persist generated Android hotspot credentials, then connect
- `WIFI_STATUS` -> `WIFI STATUS state=<state> ip=<address>`
- `MODE_STATUS` -> `MODE USB|HOTSPOT|SHARED`
- `CONFIG USB` -> save USB-only mode
- `CONFIG HOTSPOT <base64-ssid> <base64-password>` -> save phone-hotspot mode and credentials
- `CONFIG SHARED <base64-ssid> <base64-password>` -> save shared-router mode and credentials

Successful configuration returns `OK CONFIG_SAVED mode=<mode>` before the Wi-Fi transition begins. `WIFI_CONFIG` remains supported as a legacy alias for hotspot provisioning.

Successful motion returns `OK ANGLE <n>` only after the target has been accepted by the servo driver. Invalid/not-ready commands return `ERR <code> ...`.

Android must not assume one USB read equals one response. It buffers partial packets until newline and can parse multiple lines from one packet. On port open it sends `STATUS`; `ready=1` is required before web controls become available.

Firmware source: `../PhonePilotServo/PhonePilotServo.ino`. Configuration is in the adjacent `board_config.h`. These are ordinary Arduino sketch tabs and contain no PlatformIO dependency.

Wi-Fi provisioning responses:

```text
OK WIFI_SAVED
WIFI CONNECTING
WIFI CONNECTED ip=192.168.x.x
ERR WIFI_INVALID_CONFIG
ERR WIFI_SAVE_FAILED
ERR WIFI_TIMEOUT
```

SSID and password are Base64 encoded only to make the line protocol safe for spaces/non-ASCII bytes; Base64 is not encryption. Credentials are transferred over the physical USB provisioning connection and are not logged by Android.

Angles are clamped by firmware. Future motor/sensor commands should use new command names and preserve backwards compatibility with these servo commands.
