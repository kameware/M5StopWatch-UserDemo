# M5StopWatch-UserDemo
M5Stack StopWatch user demo for hardware evaluation.

## Japan time sync

This fork is configured for Japan time. The firmware sets `TZ` to the POSIX timezone string `JST-9`, which displays GMT+9/JST on the watch faces and alarm app. Time sync uses `jp.pool.ntp.org`, with `ntp.nict.jp` as a fallback server.

The clock syncs automatically on startup when Wi-Fi credentials are configured. You can also run sync manually from `Settings` > `Time & Date` > `Sync Time`.

To sync time from Wi-Fi, create `main/private_wifi_config.h` from `main/private_wifi_config.example.h` and set:

```c
#define M5_WIFI_SSID "your-ssid"
#define M5_WIFI_PASSWORD "your-password"
```

The private file is ignored by git.

## Guruguru menu

This fork also includes a `Guruguru` launcher menu based on the private `kameware/M5Stopwatch` firmware. It embeds the 9 direction avatar PNGs and switches the face direction from touch position.

## Phone notifications

The `Notify` launcher menu starts a BLE receiver for phone notifications. The watch advertises as `M5StopWatch`.

Android companion apps can write notifications to the custom service:

- Service UUID: `7b3f0001-6d6f-4d35-9d65-534d53545700`
- Write characteristic UUID: `7b3f0002-6d6f-4d35-9d65-534d53545700`
- Status characteristic UUID: `7b3f0003-6d6f-4d35-9d65-534d53545700`

Write UTF-8 JSON to the write characteristic:

```json
{"app":"Messages","title":"Alice","body":"Hello from phone"}
```

Android needs a companion app with `NotificationListenerService` permission to forward system notifications to this BLE characteristic.

iPhone notifications use Apple ANCS. Pair/connect the iPhone to `M5StopWatch`; after encryption succeeds the firmware subscribes to ANCS Notification Source and Data Source, then incoming iOS notifications appear in the same `Notify` history. No iOS app is required.

## Build

### Fetch Dependencies

```bash
python3 ./fetch_repos.py
```

### Tool Chains

[ESP-IDF v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/index.html)

### Build

```bash
idf.py build
```

### Flash

```bash
idf.py flash
```
