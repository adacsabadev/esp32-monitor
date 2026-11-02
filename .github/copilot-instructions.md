# Monitor project notes

ESP32-S3 firmware (ESP-IDF 6.1). Read this before building or flashing.

## Environment
- ESP-IDF: `C:\esp\v6.1\esp-idf`, tools: `C:\Espressif\tools`, Python venv: `C:\Espressif\tools\python\v6.1\venv`.
- Project settings are in `.vscode/settings.json` (`idf.currentSetup`, `idf.customExtraVars`). Do not set `IDF_PATH`, `IDF_TOOLS_PATH` or `IDF_PYTHON_ENV_PATH` as Windows user environment variables: they caused a "Missing required tools" popup in the ESP-IDF extension.
- The extension popup "ESP-IDF Setup from environment variables is not valid" is cosmetic. `ESP-IDF: Doctor Command` reports the install as OK.

## Building from a plain shell (outside the extension)
Set these before running `python C:\esp\v6.1\esp-idf\tools\idf.py build`:
- `PYTHONUTF8=1`
- `ESP_IDF_VERSION=6.1` (the component manager fails without it)
- `IDF_PATH`, `IDF_TOOLS_PATH`, `IDF_PYTHON_ENV_PATH` as above
- `PATH` prefixed with: `esp-idf\tools`, cmake `4.0.3\bin`, ninja `1.12.1`, `idf-exe\1.0.3`, `xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin`, `esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin`, venv `Scripts` (all under `C:\Espressif\tools`)

The build produces `build\esp32s3_monitor.bin` (about 80% of the 1 MB app partition). `idf.py` may return exit code 1 in PowerShell because of stderr output even on success; check for "Project build complete".

## Hardware and ports
- COM6 is an FT232R USB-serial adapter. It has no JTAG.
  - DTR is wired to IO0 and RTS to EN (confirmed by user; always use them for boot/reset control).
  - Download mode: DTR=1, RTS=0, then RTS=1, then RTS=0, then DTR=0.
  - Reset into the app: DTR=0, pulse RTS.
- The S3's native USB port (VID 303A, PID 1001, currently COM7) provides the built-in USB-JTAG and USB-serial. The COM number can change.

## Flashing (STANDING RULES)
- ALWAYS flash over JTAG (native USB, `board/esp32s3-builtin.cfg`). Do not use `idf.py -p COM6 flash` unless the user asks.
- ALWAYS use COM6's DTR and RTS lines to control boot and reset: DTR -> IO0 (boot), RTS -> EN (reset). Use them to enter download mode and to reset into the app after flashing.
- Never enable flash encryption, secure boot or write any eFuse.
- JTAG command (reliable form), from `build\`, with `OPENOCD_SCRIPTS` pointing at `C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20260703\openocd-esp32\share\openocd\scripts`:
  `openocd -f board/esp32s3-builtin.cfg -c "adapter speed 2000" -c "init; reset halt" -c "program_esp_bins . flasher_args.json verify exit"`
  (A plain run without `reset halt` once timed out in the flasher stub.) Then reset the app via COM6 RTS.
- Serial (works, but not the preferred route): `idf.py -p COM6 flash`.
- `interface/ftdi/esp_ftdi.cfg` does not work here, because the FT232R on COM6 is not a JTAG adapter. `.vscode/settings.json` uses `board/esp32s3-builtin.cfg` with `idf.flashType` set to `JTAG`.

## Hardware info tab (main/hwinfo.c, UI in main/index.html)
- `GET /api/hw` static HW info incl. read-only eFuse widget (never burns fuses; key blocks never exposed). `GET /api/hw/scan` = scan results. `POST /api/hw/scan?type=wifi|ble|i2c` needs header `X-Monitor: 1`.
- NEVER enable flash encryption / secure boot / write eFuses (user request: it bricks the device).
- Benchmark tab (main/bench.c): `GET /api/bench`, `POST /api/bench?type=cpu|mem|crypto` (header `X-Monitor: 1`, 409 if busy). Runs in its own task on core 1; bench.c is built with -O2. Only plain AES/SHA/MPI engines are used, never HMAC/DS/key manager. Software AES/SHA are own reference code checked against HW output; ChaCha20 is not enabled in mbedTLS.
- BT = NimBLE observer only (BT Classic does not exist on S3). I2C pins are Kconfig `MON_I2C_SDA/SCL` (default 8/9, unconfirmed wiring).
- Custom `partitions.csv` (3 MB factory app). `sdkconfig` overrides `sdkconfig.defaults`: edit both, don't delete sdkconfig (keeps flash mode dio).
- JTAG flash recipe (user prefers JTAG): from `build\`, with OPENOCD_SCRIPTS set: `openocd -f board/esp32s3-builtin.cfg -c "adapter speed 2000" -c "init; reset halt" -c "program_esp_bins . flasher_args.json verify exit"`; then reset via COM6 RTS. Env setup script recipe is in the Build section.
