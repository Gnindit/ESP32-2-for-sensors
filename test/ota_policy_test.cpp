#include "ota_policy.h"
#include <cassert>

int main() {
  using namespace OtaPolicy;
  assert(allowedUrl("https://api.github.com/repos/Gnindit/ESP32-2-for-sensors/releases/latest"));
  assert(allowedUrl("https://release-assets.githubusercontent.com/path?signature=123"));
  for (const auto *url : {"http://github.com/file", "https://github.com.evil.example/file",
       "https://github.com@evil.example/file", "https://github.com:443/file",
       "https://evil.example@github.com/file", "https://github.com/file#fragment",
       "https://github.com/file\r\nHost: evil.example", "https://github.com\\@evil.example/file",
       "//github.com/file", "https:///file"}) assert(!allowedUrl(url));
  std::string digest;
  const std::string hash(64, 'a');
  const std::string hardware = "esp32-2-hardware.bin";
  const std::string simulation = "esp32-2-simulation.bin";
  assert(checksum(hash + "  " + hardware + "\n", hardware, digest));
  assert(digest == hash);
  assert(checksum(std::string(64, 'A') + " *" + hardware + "\r\n", hardware, digest));
  assert(digest == hash);
  assert(!checksum(hash + "  " + simulation + "\n", hardware, digest));
  assert(!checksum(hash + "  " + hardware + "-factory.bin\n", hardware, digest));
  assert(!checksum("abcd  " + hardware + "\n", hardware, digest));
  assert(!checksum(std::string(64, 'z') + "  " + hardware, hardware, digest));
  assert(!checksum(hash + "  " + hardware + " extra", hardware, digest));
  assert(!checksum(hash + "  " + hardware + "\n" + hash + "  " + hardware, hardware, digest));
}
