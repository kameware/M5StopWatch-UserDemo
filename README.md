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
