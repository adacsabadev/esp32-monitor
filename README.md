# ESP32-S3 Monitor

Self-monitoring firmware for the ESP32-S3 (ESP-IDF 6.1). The board serves a web dashboard on port 80, with two tabs:

- **Monitor**: live CPU load per core, internal RAM and PSRAM, temperature, uptime, reset reason, Wi-Fi, FreeRTOS tasks, and the ULP RISC-V coprocessor heartbeat. Refreshes every second.
- **Hardware**: an HWiNFO/AIDA64-style view of the chip, split into sub-tabs:
  - **Overview**: SoC, package, features, clocks, MAC addresses, on-chip hardware list.
  - **Memory & storage**: flash (JEDEC ID, vendor, unique ID), PSRAM, heap regions, partition table.
  - **Peripherals & GPIO**: peripheral clock-gate state and a per-pin GPIO map.
  - **Network & Wi-Fi**: network interfaces, Wi-Fi radio state, nearby access points (scan).
  - **Bluetooth**: BLE controller status and nearby BLE devices (scan).
  - **Buses (I2C)**: I2C device scan with a hint table for common addresses.
  - **Security & eFuses**: read-only eFuse fields and raw non-secret blocks.

The Hardware tab loads once and scans once when first opened. It does not refresh by itself. Use **Refresh all** or the scan button of the current section to update.

## HTTP API

| Endpoint | Description |
|---|---|
| `GET /` | Dashboard page |
| `GET /api/stats` | Live stats (JSON) |
| `GET /api/hw` | Static hardware info (JSON) |
| `GET /api/hw/scan` | Latest Wi-Fi, BLE and I2C scan results (JSON) |
| `POST /api/hw/scan?type=wifi\|ble\|i2c` | Start a scan. Requires the header `X-Monitor: 1` (403 without it) |

## Build

Requires ESP-IDF 6.1 and an ESP32-S3 with 16 MB flash and 8 MB octal PSRAM (adjust `sdkconfig.defaults` for other boards).

```
idf.py build
```

### Wi-Fi credentials

Credentials are not stored in the repository.

1. Copy `main/wifi_credentials.h.example` to `main/wifi_credentials.h` (gitignored) and set `WIFI_SSID` and `WIFI_PASSWORD`.
2. Optionally set `Monitor` options with `idf.py menuconfig`: Wi-Fi SSID/password, the fallback access point password, and the I2C pins used by the I2C scan (`MON_I2C_SDA` default 8, `MON_I2C_SCL` default 9).

If no SSID is configured, the device starts its own access point `esp32s3-monitor`.

## Flash

JTAG over the S3's native USB (built-in USB-JTAG) with OpenOCD:

```
cd build
openocd -f board/esp32s3-builtin.cfg -c "adapter speed 2000" -c "init; reset halt" -c "program_esp_bins . flasher_args.json verify exit"
```

Serial flashing also works: `idf.py -p COMx flash`.

## Notes

- Partition table is custom (`partitions.csv`): 3 MB factory app, because NimBLE enlarges the image.
- Bluetooth uses NimBLE as an observer only. The ESP32-S3 has no Bluetooth Classic.
- The firmware only reads eFuses. It never enables flash encryption or secure boot and never writes any eFuse.
- Scans are on demand. Nothing drives the I2C pins until you press Scan I2C, so check that the configured pins match your wiring.
- USB devices attached to the board cannot be listed, because the USB PHY is shared with USB-Serial-JTAG.
- `.github/copilot-instructions.md` has notes on the build environment and the flashing workflow for this project.
