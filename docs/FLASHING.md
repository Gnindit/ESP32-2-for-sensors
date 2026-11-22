# ESP32-2 release firmware

Target: ESP32-S3-DevKitC-1 with 16 MB flash and 8 MB octal PSRAM.

- `esp32-2-hardware.bin`: application for real fuel, RPM and INA226 sensors.
- `esp32-2-simulation.bin`: application generating synthetic sensor readings.
- `*-factory.bin`: bootloader, partition table, OTA initialization and application
  merged into one image for USB flashing at address `0x0`.

Download from https://github.com/Gnindit/ESP32-2-for-sensors/releases.
Verify the downloaded files with `sha256sum -c SHA256SUMS` when all assets are
in the same directory.

For USB installation, close the serial monitor, install esptool in a Python
virtual environment, and replace `/dev/ttyUSB0` with your actual serial port:

```sh
python3 -m venv .venv
.venv/bin/pip install esptool==4.11.0
.venv/bin/python -m esptool --chip esp32s3 --port /dev/ttyUSB0 write_flash 0x0 esp32-2-hardware-factory.bin
```

Use the simulation factory image instead for a bench test without sensors.
Factory images replace the bootloader, partition table and OTA selection; use
them only on this target board. No full flash erase is needed.

The application-only images belong at `0x10000` when the board already has this
project's bootloader/partition layout and boots the first application slot.
Use the factory image for a straightforward USB installation.

This firmware has no webpage, WiFi login or OTA update endpoint. Downloading a
release from another location gives you the firmware files; installation uses USB.
