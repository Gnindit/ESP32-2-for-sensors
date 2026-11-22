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

For the first installation, use USB as above. Then open a 115200-baud serial
terminal on the USB-to-UART port and send one line followed by Enter:

```text
wifi {"ssid":"YOUR_WIFI_NAME","password":"YOUR_WIFI_PASSWORD"}
```

Use an Internet-connected 2.4 GHz router fixed to channel 1 to preserve ESP-NOW
communication. Credentials are stored on the board and survive application OTA
updates. ESP32-2 does not create an access point.

Join the same router with your phone, open the IP address printed after
`Update page:` (or `http://esp32-2.local/`), and press **Update software**.
Send `wifi-status` in the serial terminal to print connection state/channel/IP
again. Startup diagnostics report whether the saved network is found on channel
1 and show disconnect reasons or connection timeouts without showing credentials.
The board downloads the matching hardware/simulation application from the latest
stable release, verifies HTTPS certificates, SHA-256 and the application image,
then restarts. Wait for the release workflow to finish attaching all assets.
No separate webpage password is required; anyone on the LAN who can reach the
board can trigger an update. Do not forward the webpage port to the Internet.

Older firmware without the update page must be upgraded over USB first. USB
also remains the recovery method if a newly installed application cannot boot.
See the project README for full setup, security and validation details.
