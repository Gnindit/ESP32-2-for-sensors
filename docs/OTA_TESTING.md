# First hardware verification

Local builds and host policy tests cannot prove radio stability or a real GitHub
OTA installation. Run these checks on a bench before relying on phone updates.

1. Flash the simulation environment over USB, provision Wi-Fi through serial,
   and confirm no ESP32-2 access point appears. Use an Internet-connected router
   with its 2.4 GHz channel fixed to 1. Confirm the printed IP opens on your phone.
2. Confirm the installed version/mode, live sensor readings at the receiver,
   normal ESP-NOW send counters, and reconnect after reboot without reprovisioning.
3. Publish a distinct stable version containing the OTA implementation. Wait for
   both application images and SHA256SUMS. Press Update software and verify
   progress, reboot, the new version, simulation mode, retained Wi-Fi settings,
   and fresh receiver values. Repeat with hardware firmware when safe to do so.
4. Press Update software again: it must report that the latest release is
   already installed without restarting. Multiple presses while busy must not
   start parallel updates.
5. With Internet/NTP blocked or a release whose assets are not ready, verify a
   readable error and continued operation. Interrupt the router connection during
   a download and verify failure/retry without booting the partial image.
6. Confirm GET /api/update does not install anything. A POST without the page's
   X-Update-Token must return 403; an unrelated Origin must also return 403.
   Page polling after a reboot should work; reload the page before another update
   because its old token intentionally expires at reboot.
7. Turn off the router and verify ESP-NOW operation on channel 1. Reconnect and
   check receiver freshness/counters during network connection and flash writes.
   Automatic rollback is not enabled: retain the USB recovery path.

Host checks, from the project directory:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -Isrc test/ota_policy_test.cpp -o /tmp/ota-policy-test
/tmp/ota-policy-test
platformio run -e esp32-s3-2 -e esp32-s3-2-hardware
git diff --check
```
