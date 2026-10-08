# Dialed

DIY ESP32-based MTB suspension telemetry and tuning helper for enduro/downhill.

The device rides on the bike and records every run. It logs suspension travel (a linear position sensor on the fork and one on the shock), an IMU and GPS to an SD card at 500 Hz, and serves its own web dashboard for reviewing a run, comparing two runs and tuning the suspension from what was recorded.

## Documentation

- [summary.md](summary.md) — the record: hardware, pin table, firmware architecture, how each part works and what was learned building it.
- [todo.md](todo.md) — the working task list.

## Layout

```
src/            firmware, one folder per module (suspension, imu, gps, run, storage, web, ...)
data/           the dashboard (HTML, CSS, JS) — becomes the device's LittleFS image
tools/          build scripts run by PlatformIO
platformio.ini  board, libraries and build options
```

## Building

The project builds with [PlatformIO](https://platformio.org/) (`esp32dev`, Arduino framework). Two files hold secrets and this machine's settings, and are not in the repository; create them from their examples:

| Copy | To | What to set |
|---|---|---|
| `src/config/secrets.example.h` | `src/config/secrets.h` | The OTA password the firmware will ask for |
| `platformio.local.ini.example` | `platformio.local.ini` | The device's address and the same password |

The firmware does not compile without `secrets.h`. `platformio.local.ini` is optional: without it, uploads go over USB.

```
pio run               # build the firmware
pio run -t upload     # upload the firmware, then the dashboard (data/)
pio run -t uploadfs   # upload the dashboard only
```

Every build is stamped with `git describe` and its build time; Settings → System shows the stamp of the firmware and of the dashboard, and each run records the firmware that made it. A release is a git tag.

A board that has never run this firmware has to be flashed over USB once; after that, uploads can go over the air.

## License

MIT — see [LICENSE](LICENSE).
