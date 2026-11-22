# ESP32-2 Sensor ESP-NOW Broadcaster

This is a standalone PlatformIO project for the second ESP32-S3 node.

It reads:
- `FuelLev1` from `GPIO5` (`ADC1_4`)
- `EngRPM1` pulse input from `GPIO1`
- `BusV1` bus voltage and `Curr1` current from an INA226 on SDA=`GPIO8`, SCL=`GPIO9` (address `0x40`)

It then broadcasts all four measurements over ESP-NOW using the same packet family as the main ESP32 node.

## Firmware downloads

[GitHub Releases](https://github.com/Gnindit/ESP32-2-for-sensors/releases) hosts
firmware files once a release has been published and its build has completed.
Use `esp32-2-hardware-factory.bin` for real sensors or
`esp32-2-simulation-factory.bin` for testing. See [USB installation](docs/FLASHING.md).
There is no webpage or OTA update endpoint in this firmware.

## Serial port

USB CDC is disabled in the existing build configuration. Use the board's
USB-to-UART port. Local serial port overrides belong in ignored
`platformio.local.ini`; copy `platformio.local.ini.example` to start.
The public configuration uses automatic port detection by default.

## Wiring notes

- `GPIO5` is the fuel level analog input.
- `GPIO1` is the RPM pulse input.
- The RPM input is specified as a `12 V` NPN pulse source and must not be connected directly to the ESP32.
  Use a proper level-shifting or isolation stage first.

## Fuel calibration

Fuel percentage is calculated from measured millivolts at `GPIO5`:
- `FUEL_EMPTY_MV` (default `0`)
- `FUEL_FULL_MV` (default `1165`)

The firmware maps fuel as:

`fuel_percent = clamp((fuel_mV - FUEL_EMPTY_MV) * 100 / (FUEL_FULL_MV - FUEL_EMPTY_MV), 0, 100)`

ADC setup is:
- 12-bit read resolution
- global attenuation `ADC_2_5db`

### Sender and divider model

For a 0-180 ohm fuel sender with a 330 ohm fixed resistor to 3.3 V,
and the sender to ground, the ADC node voltage is:

`Vout = Vcc * Rsender / (Rfixed + Rsender)`

With `Vcc = 3.3 V`, `Rfixed = 330 ohm`, `Rsender = 0..180 ohm`:
- Empty (`0 ohm`): `Vout = 0.0 V`
- Full (`180 ohm`): `Vout = 3.3 * 180 / (330 + 180) = 1.165 V`

So a practical full-scale constant is about `FUEL_FULL_MV = 1165`.
If your actual full voltage differs due to wiring, sensor tolerance, or supply variation,
set `FUEL_FULL_MV` to your measured full value.

## Timing

- RPM measurement window: `300 ms`
- Fuel sample interval: `1000 ms`
- ESP-NOW broadcast interval: `200 ms`
- INA226 sample interval: `500 ms`
- Serial summary interval: `3000 ms`

## ESP-NOW output

The node broadcasts one ESP-NOW packet every `200 ms`.

Each packet contains:
- `magic`: packet family identifier used by the receiver to recognize the frame
- `version`: packet format version
- `sensorCount`: number of active sensor entries in the packet
- `uptimeMs`: device uptime when the packet was sent
- `sensors[0]`: fuel level snapshot
- `sensors[1]`: RPM snapshot
- `sensors[2]`: bus voltage snapshot
- `sensors[3]`: current snapshot

Each sensor snapshot includes:
- `active`: whether the sensor slot is enabled
- `valid`: whether the current reading is valid
- `valueMode`: direct scaled value mode
- `rawValue`: raw sample or pulse count
- `engineeringValue`: converted value, such as percent fuel or RPM

For this firmware, the broadcasted sensor mapping is:
- `FuelLev1` -> raw ADC sample and fuel percent
- `EngRPM1` -> pulse count for the window and filtered RPM
- `BusV1` -> bus voltage in volts
- `Curr1` -> current in amps

## Filtering

- Fuel uses an exponential moving average (EMA) on ADC millivolts with `FUEL_FILTER_ALPHA = 0.05`.
  This intentionally makes fuel movement more sluggish for stable gauge behavior.
- RPM uses asymmetric EMA filtering:
  - `RPM_FILTER_ALPHA_UP = 0.15` for faster rise response
  - `RPM_FILTER_ALPHA_DOWN = 0.10` for slower decay response
  - `RPM_FILTER_DEADBAND = 4 rpm` to suppress tiny jitter
- RPM is forced to zero when pulse timeout is reached (`RPM_ZERO_TIMEOUT_MS = 1000`) so stale values do not linger.

## Serial port debug output
- Fuel raw=480 fuel=%4.3 rpm=2955.6 pulses/window=1002 espnow_ok=14 espnow_fail=0

## Build and upload

Install PlatformIO Core 6.2.0 (or use the PlatformIO IDE terminal). The project
pins Espressif32 7.0.1 and includes its N16R8 board definition.

```sh
# Existing default: simulated readings, no sensor hardware required
platformio run -e esp32-s3-2

# Real fuel, RPM and INA226 sensors
platformio run -e esp32-s3-2-hardware

# Upload real sensor firmware over USB when ready
platformio run -e esp32-s3-2-hardware -t upload
platformio device monitor -e esp32-s3-2-hardware
```

Application outputs are `.pio/build/esp32-s3-2/firmware.bin` and
`.pio/build/esp32-s3-2-hardware/firmware.bin`. The existing default simulation
setting in `src/main.cpp` is unchanged; the hardware environment explicitly
disables simulation. VS Code tasks use `platformio` from the terminal PATH.

## Publish a release

The Firmware workflow builds both variants for pushes to `main`, pull requests,
and manual runs, and saves downloadable build artifacts. To publish a version:

1. Open GitHub Releases and choose **Draft a new release**.
2. Create a version tag such as `v1.0.0` targeting the reviewed `main` commit.
3. Add release notes and publish the release.
4. Wait for the Firmware workflow to attach application images, merged factory
   images, USB instructions and `SHA256SUMS` to that release.

The workflow uses GitHub's built-in token; no personal token belongs in the repo.
Asset attachment uses [GitHub CLI release upload](https://cli.github.com/manual/gh_release_upload).
The same package can be generated locally after building both environments:

```sh
python3 scripts/package_release.py
```

Run it with a Python environment containing the installed PlatformIO esptool
dependencies (the PlatformIO virtual environment works). Outputs go in ignored
`dist/`. This packages files only; it does not upload firmware or publish anything.

## Repository hygiene

Build output, release packages, local serial ports, local editor settings,
`.env` files, secret headers/JSON and private key files are ignored. Share only
placeholder example configuration. Extra local configuration is loaded using
[PlatformIO extra_configs](https://docs.platformio.org/en/stable/projectconf/sections/platformio/options/generic/extra_configs.html).
Before publishing, review staged files and commit metadata as well as history:
`.gitignore` does not remove information already committed.
