#include "web_update.h"
#include "build_info.h"
#include "ota_policy.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_app_format.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>
#include <time.h>

// Use the ESP-IDF Mozilla CA bundle, rather than Arduino's similarly named shim.
extern "C" esp_err_t esp_crt_bundle_attach(void *config);

namespace {
constexpr char releaseApi[] = "https://api.github.com/repos/Gnindit/ESP32-2-for-sensors/releases/latest";
constexpr char assetPrefix[] = "https://github.com/Gnindit/ESP32-2-for-sensors/releases/download/";
constexpr uint8_t wifiChannel = 1;
WebServer server(80);
bool simulationMode;
bool webStarted = false;
bool timeStarted = false;
bool wasConnected = false;
uint32_t lastConnectAttempt = 0;
uint32_t connectionStarted = 0;
bool scanning = false;
bool connecting = false;
const char *networkState = "notConfigured";
String stationSsid, stationPassword, serialLine, csrfToken;
bool droppingSerialLine = false;
portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t disconnectReason = 0;
struct Status {
  bool busy = false;
  unsigned progress = 0;
  char message[160] = "Ready";
} status;

Status snapshot() {
  portENTER_CRITICAL(&statusMux);
  Status copy = status;
  portEXIT_CRITICAL(&statusMux);
  return copy;
}

void report(const String &message, unsigned progress = 0, bool busy = true) {
  portENTER_CRITICAL(&statusMux);
  status.busy = busy;
  status.progress = progress;
  strlcpy(status.message, message.c_str(), sizeof(status.message));
  portEXIT_CRITICAL(&statusMux);
}

// Each redirect gets a fresh, certificate-verified TLS connection. Never log signed URLs.
class Download {
 public:
  ~Download() { close(); }
  bool open(String url, String &error) {
    for (unsigned hop = 0; hop <= 5; ++hop) {
      close();
      if (!OtaPolicy::allowedUrl(url.c_str())) { error = "Unexpected download host or unsafe URL"; return false; }
      location = "";
      encoding = "";
      esp_http_client_config_t config = {};
      config.url = url.c_str();
      config.crt_bundle_attach = esp_crt_bundle_attach;
      config.timeout_ms = 10000;
      config.disable_auto_redirect = true;
      config.buffer_size = 2048;
      config.buffer_size_tx = url.length() + 1024;
      config.event_handler = onEvent;
      config.user_data = this;
      config.user_agent = "ESP32-2/" FIRMWARE_VERSION;
      client = esp_http_client_init(&config);
      if (!client) { error = "Not enough memory for HTTPS"; return false; }
      esp_http_client_set_header(client, "Accept-Encoding", "identity");
      const esp_err_t result = esp_http_client_open(client, 0);
      if (result != ESP_OK) { error = "HTTPS connection failed: " + String(esp_err_to_name(result)); return false; }
      length = esp_http_client_fetch_headers(client);
      const int code = esp_http_client_get_status_code(client);
      if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
        if (location.startsWith("/") && !location.startsWith("//")) {
          const int slash = url.indexOf('/', 8);
          location = url.substring(0, slash < 0 ? url.length() : slash) + location;
        }
        url = location;
        continue;
      }
      if (code != 200) { error = "GitHub returned HTTP " + String(code); return false; }
      if (encoding.length() && encoding != "identity") { error = "Unexpected compressed response"; return false; }
      return true;
    }
    error = "Too many download redirects";
    return false;
  }
  bool text(const String &url, String &body, size_t limit, String &error) {
    if (!open(url, error)) return false;
    if (length > static_cast<int64_t>(limit)) { error = "GitHub response too large"; return false; }
    char buffer[1024];
    uint32_t lastData = millis();
    while (!complete()) {
      const int count = read(buffer, sizeof(buffer));
      if (count < 0) { error = "Download interrupted"; return false; }
      if (count == 0) {
        if (millis() - lastData > 15000) { error = "Download timed out"; return false; }
        delay(1);
        continue;
      }
      lastData = millis();
      if (body.length() + count > limit || !body.concat(buffer, count)) { error = "GitHub response too large"; return false; }
    }
    return true;
  }
  int read(char *buffer, size_t size) { return esp_http_client_read(client, buffer, size); }
  bool complete() { return esp_http_client_is_complete_data_received(client); }
  int64_t length = -1;
 private:
  esp_http_client_handle_t client = nullptr;
  String location, encoding;
  void close() {
    if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); client = nullptr; }
  }
  static esp_err_t onEvent(esp_http_client_event_t *event) {
    auto *self = static_cast<Download *>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_HEADER) {
      if (!strcasecmp(event->header_key, "Location")) self->location = event->header_value;
      if (!strcasecmp(event->header_key, "Content-Encoding")) self->encoding = event->header_value;
    }
    return ESP_OK;
  }
};

String jsonString(cJSON *object, const char *key) {
  cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(value) && value->valuestring ? String(value->valuestring) : String();
}

bool installLatest(String &error) {
  if (WiFi.status() != WL_CONNECTED) { error = "Wi-Fi is disconnected"; return false; }
  report("Synchronizing clock for HTTPS");
  const uint32_t start = millis();
  while (time(nullptr) < 1704067200) {
    if (millis() - start > 20000) { error = "Clock not synchronized; check Internet/NTP access"; return false; }
    delay(100);
  }
  report("Checking latest GitHub release");
  String metadata;
  {
    Download download;
    if (!download.text(releaseApi, metadata, 65536, error)) return false;
  }
  cJSON *release = cJSON_Parse(metadata.c_str());
  if (!release) { error = "Invalid GitHub release response"; return false; }
  const String tag = jsonString(release, "tag_name");
  const bool unsuitable = cJSON_IsTrue(cJSON_GetObjectItem(release, "draft")) ||
                           cJSON_IsTrue(cJSON_GetObjectItem(release, "prerelease"));
  const String filename = simulationMode ? "esp32-2-simulation.bin" : "esp32-2-hardware.bin";
  String binaryUrl, sumsUrl;
  cJSON *asset = nullptr;
  cJSON_ArrayForEach(asset, cJSON_GetObjectItem(release, "assets")) {
    const String name = jsonString(asset, "name");
    if (name == filename) binaryUrl = jsonString(asset, "browser_download_url");
    if (name == "SHA256SUMS") sumsUrl = jsonString(asset, "browser_download_url");
  }
  cJSON_Delete(release);
  metadata = "";
  if (unsuitable || tag.isEmpty()) { error = "No stable release available"; return false; }
  if (tag == FIRMWARE_VERSION) { report("Already running the latest release: " + tag, 100, false); return true; }
  // Resolve both assets from the same release; never use a moving /latest/download URL.
  const String prefix = String(assetPrefix);
  if (!binaryUrl.startsWith(prefix) || !sumsUrl.startsWith(prefix) ||
      binaryUrl.substring(0, binaryUrl.lastIndexOf('/')) != sumsUrl.substring(0, sumsUrl.lastIndexOf('/'))) {
    error = "Release assets missing; wait for the release build to finish"; return false;
  }
  String sums;
  {
    Download download;
    if (!download.text(sumsUrl, sums, 8192, error)) return false;
  }
  std::string expectedHash;
  if (!OtaPolicy::checksum(sums.c_str(), filename.c_str(), expectedHash)) { error = "Missing or invalid firmware checksum"; return false; }
  report("Downloading " + tag);
  Download firmware;
  if (!firmware.open(binaryUrl, error)) return false;
  const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
  if (!partition || firmware.length < static_cast<int64_t>(sizeof(esp_image_header_t)) || firmware.length > partition->size) {
    error = "Firmware size invalid or no OTA partition available"; return false;
  }
  esp_ota_handle_t handle;
  esp_err_t result = esp_ota_begin(partition, firmware.length, &handle);
  if (result != ESP_OK) { error = "Cannot begin update: " + String(esp_err_to_name(result)); return false; }
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  if (mbedtls_sha256_starts_ret(&hash, 0) != 0) {
    mbedtls_sha256_free(&hash);
    esp_ota_abort(handle);
    error = "Cannot initialize firmware checksum";
    return false;
  }
  size_t received = 0;
  uint8_t firstHeader[sizeof(esp_image_header_t)];
  size_t headerBytes = 0;
  char buffer[4096];
  uint32_t lastData = millis();
  while (received < static_cast<size_t>(firmware.length)) {
    const int count = firmware.read(buffer, std::min(sizeof(buffer), static_cast<size_t>(firmware.length) - received));
    if (count < 0 || (count == 0 && (firmware.complete() || millis() - lastData > 15000))) {
      error = "Firmware download incomplete"; break;
    }
    if (count == 0) { delay(1); continue; }
    lastData = millis();
    if (headerBytes < sizeof(firstHeader)) {
      const size_t take = std::min(static_cast<size_t>(count), sizeof(firstHeader) - headerBytes);
      memcpy(firstHeader + headerBytes, buffer, take);
      headerBytes += take;
      if (headerBytes == sizeof(firstHeader)) {
        esp_image_header_t image;
        memcpy(&image, firstHeader, sizeof(image));
        if (image.magic != ESP_IMAGE_HEADER_MAGIC || image.chip_id != ESP_CHIP_ID_ESP32S3) {
          error = "Release image is not ESP32-S3 application firmware"; break;
        }
      }
    }
    if (mbedtls_sha256_update_ret(&hash, reinterpret_cast<uint8_t *>(buffer), count) != 0) {
      error = "Cannot calculate firmware checksum"; break;
    }
    result = esp_ota_write(handle, buffer, count);
    if (result != ESP_OK) { error = "Flash write failed: " + String(esp_err_to_name(result)); break; }
    received += count;
    report("Downloading " + tag, received * 100 / firmware.length);
    delay(1);
  }
  uint8_t digest[32];
  const int hashResult = mbedtls_sha256_finish_ret(&hash, digest);
  mbedtls_sha256_free(&hash);
  char actualHash[65];
  for (size_t i = 0; i < sizeof(digest); ++i) snprintf(actualHash + i * 2, 3, "%02x", digest[i]);
  if (error.isEmpty() && (hashResult != 0 || expectedHash != actualHash)) error = "Firmware checksum mismatch";
  if (!error.isEmpty()) { esp_ota_abort(handle); return false; }
  report("Verifying firmware", 100);
  result = esp_ota_end(handle);  // Validates the image before selecting it for boot.
  if (result != ESP_OK) { error = "Image validation failed: " + String(esp_err_to_name(result)); return false; }
  result = esp_ota_set_boot_partition(partition);
  if (result != ESP_OK) { error = "Cannot select updated firmware"; return false; }
  report("Update installed. Restarting...", 100);
  delay(2500);
  ESP.restart();
  return true;
}

void updateTask(void *) {
  String error;
  if (!installLatest(error)) report(error, 0, false);
  vTaskDelete(nullptr);
}

const char page[] PROGMEM = R"HTML(<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32-2 software update</title>
<style>body{font:18px system-ui;margin:2rem auto;padding:0 1rem;max-width:34rem;background:#101923;color:#eef4fa}button{font:inherit;padding:1rem;border:0;border-radius:.5rem;background:#68b9ff;color:#101923;width:100%}button:disabled{opacity:.5}progress{width:100%;margin:1rem 0}p{line-height:1.5}small{color:#b2c3d3}</style>
<h1>ESP32-2</h1><p>Software: <strong id="version"></strong><br>Mode: <span id="mode"></span></p>
<button id="update" disabled>Update software</button><progress id="progress" max="100" value="0"></progress>
<p id="status">Connecting...</p><small>Downloads the latest stable GitHub release. The board restarts after installation.</small>
<script>
const token='__TOKEN__';const button=document.getElementById('update');let starting=false;
async function poll(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw Error();const s=await r.json();document.getElementById('version').textContent=s.version;document.getElementById('mode').textContent=s.mode;document.getElementById('status').textContent=s.message;document.getElementById('progress').value=s.progress;button.disabled=s.busy||starting;}catch(e){document.getElementById('status').textContent='Board unavailable. Reconnecting...';button.disabled=true;}}
button.onclick=async()=>{if(!confirm('Install the latest software and restart this board?'))return;starting=true;button.disabled=true;try{const r=await fetch('/api/update',{method:'POST',headers:{'X-Update-Token':token}});if(!r.ok){const message=await r.text();alert(message);if(r.status===403)location.reload();}}catch(e){alert('Could not start update. Check the connection.');}finally{starting=false;poll();}};
poll();setInterval(poll,1500);
</script></html>)HTML";

bool localHost() {
  const String host = server.hostHeader();
  const String ip = WiFi.localIP().toString();
  return host == ip || host == ip + ":80" || host == "esp32-2.local" || host == "esp32-2.local:80";
}

void configureServer() {
  const char *headers[] = {"X-Update-Token", "Origin"};
  server.collectHeaders(headers, 2);
  server.on("/", HTTP_GET, [] {
    if (!localHost()) { server.send(403, "text/plain", "Use the board IP address or esp32-2.local"); return; }
    String html = FPSTR(page);
    html.replace("__TOKEN__", csrfToken);
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-Frame-Options", "DENY");
    server.sendHeader("Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'");
    server.send(200, "text/html", html);
  });
  server.on("/api/status", HTTP_GET, [] {
    if (!localHost()) { server.send(403); return; }
    const Status copy = snapshot();
    cJSON *json = cJSON_CreateObject();
    if (!json) { server.send(503); return; }
    cJSON_AddStringToObject(json, "version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(json, "mode", simulationMode ? "Simulation" : "Hardware");
    cJSON_AddBoolToObject(json, "busy", copy.busy);
    cJSON_AddNumberToObject(json, "progress", copy.progress);
    cJSON_AddStringToObject(json, "message", copy.message);
    char *body = cJSON_PrintUnformatted(json);
    server.sendHeader("Cache-Control", "no-store");
    if (body) server.send(200, "application/json", body);
    else server.send(503);
    cJSON_free(body);
    cJSON_Delete(json);
  });
  server.on("/api/update", HTTP_POST, [] {
    const String origin = server.header("Origin");
    if (!localHost() || server.header("X-Update-Token") != csrfToken ||
        (origin.length() && origin != "http://" + server.hostHeader())) {
      server.send(403, "text/plain", "Reload the update page on this board and try again"); return;
    }
    if (snapshot().busy) { server.send(409, "text/plain", "An update is already running"); return; }
    report("Starting update");
    if (xTaskCreatePinnedToCore(updateTask, "github-update", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
      report("Not enough memory to start update", 0, false);
      server.send(503, "text/plain", "Not enough memory"); return;
    }
    server.send(202, "text/plain", "Update started");
  });
  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
}

void connectStation() {
  lastConnectAttempt = millis();
  if (stationSsid.isEmpty()) return;
  if (scanning) { esp_wifi_scan_stop(); WiFi.scanDelete(); }
  connecting = false;
  const int result = WiFi.scanNetworks(true, false, false, 120, wifiChannel, stationSsid.c_str());
  scanning = result == WIFI_SCAN_RUNNING;
  networkState = scanning ? "scanningChannel1" : "scanFailed";
  Serial.println(scanning ? "Wi-Fi: scanning for saved network on channel 1..." : "Wi-Fi: could not start scan; retrying in 30 seconds.");
}

void printNetworkStatus() {
  Serial.printf("Wi-Fi: state=%s, saved=%s, status=%d, channel=%d, IP=%s\n",
                networkState, stationSsid.isEmpty() ? "no" : "yes",
                static_cast<int>(WiFi.status()), WiFi.channel(), WiFi.localIP().toString().c_str());
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Update page: http://%s/ (or http://esp32-2.local/)\n", WiFi.localIP().toString().c_str());
  }
}

void handleStationConnection() {
  uint8_t reason;
  portENTER_CRITICAL(&statusMux);
  reason = disconnectReason;
  disconnectReason = 0;
  portEXIT_CRITICAL(&statusMux);
  if (reason) {
    Serial.printf("Wi-Fi: disconnected, reason=%u", reason);
    if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
        reason == WIFI_REASON_HANDSHAKE_TIMEOUT) Serial.print(" (authentication/handshake failed; check password and router security)");
    else if (reason == WIFI_REASON_NO_AP_FOUND) Serial.print(" (router not found)");
    Serial.println();
  }
  if (connecting) {
    if (WiFi.status() == WL_CONNECTED) { connecting = false; return; }
    if (millis() - connectionStarted >= 12000) {
      WiFi.disconnect(false, false);
      connecting = false;
      networkState = "connectionFailed";
      lastConnectAttempt = millis();
      esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
      Serial.println("Wi-Fi: connection timed out; retrying in 30 seconds. Check credentials/router settings.");
    }
    return;
  }
  if (!scanning) return;
  const int count = WiFi.scanComplete();
  if (count == WIFI_SCAN_RUNNING) return;
  scanning = false;
  for (int i = 0; i < count; ++i) {
    if (WiFi.SSID(i) == stationSsid && WiFi.channel(i) == wifiChannel) {
      uint8_t bssid[6];
      memcpy(bssid, WiFi.BSSID(i), sizeof(bssid));
      const int rssi = WiFi.RSSI(i);
      WiFi.scanDelete();
      WiFi.begin(stationSsid.c_str(), stationPassword.c_str(), wifiChannel, bssid);
      connecting = true;
      connectionStarted = millis();
      networkState = "connecting";
      Serial.printf("Wi-Fi: saved network found on channel 1 (signal %d dBm); connecting...\n", rssi);
      return;
    }
  }
  WiFi.scanDelete();
  esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
  networkState = count < 0 ? "scanFailed" : "routerNotOnChannel1";
  Serial.println(count < 0 ? "Wi-Fi: scan failed; retrying in 30 seconds." :
                 "Wi-Fi: saved network not found on channel 1. Check SSID, 2.4 GHz, channel, and range.");
}

void serialConfiguration() {
  // Bounded work per loop, no credential echo. Wi-Fi settings survive public OTA releases in NVS.
  for (unsigned i = 0; i < 128 && Serial.available(); ++i) {
    const char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (serialLine.length() >= 384) { droppingSerialLine = true; serialLine = ""; }
      if (!droppingSerialLine) serialLine += c;
      continue;
    }
    if (droppingSerialLine) { droppingSerialLine = false; serialLine = ""; continue; }
    if (serialLine == "wifi-status") printNetworkStatus();
    else if (serialLine.startsWith("wifi ") && !snapshot().busy) {
      cJSON *config = cJSON_Parse(serialLine.c_str() + 5);
      const String ssid = jsonString(config, "ssid");
      const String password = jsonString(config, "password");
      cJSON *passwordField = cJSON_GetObjectItemCaseSensitive(config, "password");
      if (ssid.length() >= 1 && ssid.length() <= 32 && cJSON_IsString(passwordField) &&
          (password.isEmpty() || (password.length() >= 8 && password.length() <= 63))) {
        Preferences preferences;
        bool saved = preferences.begin("network", false);
        // Store a single JSON value so an interrupted write cannot mix credentials.
        if (saved) saved = preferences.putString("credentials", serialLine.substring(5)) > 0;
        preferences.end();
        if (saved) {
          stationSsid = ssid;
          stationPassword = password;
          WiFi.disconnect(false, false);
          connectStation();
          Serial.println("Wi-Fi settings saved. Connecting on channel 1.");
        } else Serial.println("Could not save Wi-Fi settings.");
      } else Serial.println("Invalid Wi-Fi settings. Use: wifi {\"ssid\":\"...\",\"password\":\"...\"}");
      cJSON_Delete(config);
    }
    serialLine = "";
  }
}
}  // namespace

namespace WebUpdate {
void begin(bool simulation) {
  simulationMode = simulation;
  char token[33];
  for (unsigned i = 0; i < 4; ++i) snprintf(token + i * 8, 9, "%08x", static_cast<unsigned>(esp_random()));
  csrfToken = token;
  Preferences preferences;
  if (preferences.begin("network", true)) {
    String credentials = preferences.getString("credentials", "");
    cJSON *config = cJSON_Parse(credentials.c_str());
    stationSsid = jsonString(config, "ssid");
    stationPassword = jsonString(config, "password");
    cJSON_Delete(config);
    preferences.end();
  }
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.setHostname("esp32-2");
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    portENTER_CRITICAL(&statusMux);
    disconnectReason = info.wifi_sta_disconnected.reason;
    portEXIT_CRITICAL(&statusMux);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  configureServer();
  Serial.println("Software " FIRMWARE_VERSION ". Wi-Fi station only; router must use 2.4 GHz channel 1.");
  if (stationSsid.isEmpty()) Serial.println("Configure via USB: wifi {\"ssid\":\"YOUR_WIFI\",\"password\":\"YOUR_PASSWORD\"}");
  else Serial.println("Wi-Fi settings restored from NVS. Type wifi-status for connection state/IP.");
  connectStation();
}

void loop() {
  serialConfiguration();
  handleStationConnection();
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && WiFi.channel() != wifiChannel) {
    WiFi.disconnect(false, false);
    esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
    Serial.println("Wi-Fi disconnected: router must use 2.4 GHz channel 1 for ESP-NOW.");
    report("Router must use 2.4 GHz channel 1 for ESP-NOW", 0, snapshot().busy);
    connected = false;
  }
  if (!connected && wasConnected) esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
  if (connected && !wasConnected) {
    networkState = "connected";
    if (!timeStarted) { configTime(0, 0, "pool.ntp.org", "time.cloudflare.com"); timeStarted = true; }
    if (!webStarted) {
      server.begin();
      if (MDNS.begin("esp32-2")) MDNS.addService("http", "tcp", 80);
      webStarted = true;
    }
    Serial.print("Update page: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/ (or http://esp32-2.local/)");
  }
  if (!connected && wasConnected) { networkState = "disconnected"; lastConnectAttempt = millis(); }
  if (!connected && !scanning && !connecting && !snapshot().busy && millis() - lastConnectAttempt >= 30000) {
    esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
    connectStation();
  }
  wasConnected = connected;
  if (webStarted && connected) server.handleClient();
}
}  // namespace WebUpdate
