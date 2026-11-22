#include "espnow_helper.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <string.h>

namespace {

constexpr uint32_t ESP_NOW_MAGIC = 0x574E5053;
constexpr uint8_t ESP_NOW_VERSION = 1;
constexpr uint8_t VALUE_MODE_DIRECT_SCALED = 1;
constexpr uint8_t MAX_SENSOR_SNAPSHOTS = 16;
constexpr uint8_t WIFI_CHANNEL = 1;
constexpr uint8_t ESP_NOW_BROADCAST_MAC[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

struct __attribute__((packed)) EspNowSensorSnapshot {
  uint8_t active;
  uint8_t valid;
  uint8_t hasCurrentMa;
  uint8_t valueMode;
  uint8_t slaveId;
  uint16_t registerAddress;
  uint16_t rawValue;
  float engineeringValue;
};

struct __attribute__((packed)) EspNowBroadcastPacket {
  uint32_t magic;
  uint8_t version;
  uint8_t sensorCount;
  uint16_t reserved;
  uint32_t uptimeMs;
  EspNowSensorSnapshot sensors[MAX_SENSOR_SNAPSHOTS];
};

uint8_t gBroadcastMac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
uint8_t gWifiChannel = WIFI_CHANNEL;
bool gEspNowReady = false;
uint32_t gEspNowSendCount = 0;
uint32_t gEspNowSendFailCount = 0;

inline bool isSerialHostConnected(size_t minBytes = 1) {
  return static_cast<bool>(Serial) &&
         Serial.availableForWrite() >= static_cast<int>(minBytes);
}

void onEspNowSent(const uint8_t *mac_addr,
                  esp_now_send_status_t status) {
  (void)mac_addr;
  ++gEspNowSendCount;
  if (status != ESP_NOW_SEND_SUCCESS) {
    ++gEspNowSendFailCount;
  }
}

}  // namespace

namespace EspNowHelper {

void begin() {
  gWifiChannel = WIFI_CHANNEL;
  memcpy(gBroadcastMac, ESP_NOW_BROADCAST_MAC, sizeof(gBroadcastMac));

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(gWifiChannel, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    if (isSerialHostConnected(32)) {
      Serial.println("ESP-NOW init failed");
    }
    gEspNowReady = false;
    return;
  }

  esp_now_register_send_cb(onEspNowSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, gBroadcastMac, sizeof(gBroadcastMac));
  peerInfo.channel = gWifiChannel;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA;

  if (!esp_now_is_peer_exist(gBroadcastMac) &&
      esp_now_add_peer(&peerInfo) != ESP_OK) {
    if (isSerialHostConnected(48)) {
      Serial.println("Failed to add ESP-NOW broadcast peer");
    }
    gEspNowReady = false;
    return;
  }

  gEspNowReady = true;
  if (isSerialHostConnected(32)) {
    Serial.println("ESP-NOW ready for broadcast");
  }
}

bool isReady() {
  return gEspNowReady;
}

void sendSensorSnapshot(const SensorState *sensorStates,
                        uint8_t sensorCount,
                        uint32_t uptimeMs) {
  if (!gEspNowReady || sensorStates == nullptr) {
    return;
  }

  const uint8_t boundedSensorCount =
      sensorCount > MAX_SENSOR_SNAPSHOTS ? MAX_SENSOR_SNAPSHOTS : sensorCount;

  EspNowBroadcastPacket packet = {};
  packet.magic = ESP_NOW_MAGIC;
  packet.version = ESP_NOW_VERSION;
  packet.sensorCount = boundedSensorCount;
  packet.uptimeMs = uptimeMs;

  for (uint8_t i = 0; i < boundedSensorCount; ++i) {
    packet.sensors[i].active = sensorStates[i].active ? 1 : 0;
    packet.sensors[i].valid = sensorStates[i].valid ? 1 : 0;
    packet.sensors[i].hasCurrentMa = 0;
    packet.sensors[i].valueMode = VALUE_MODE_DIRECT_SCALED;
    packet.sensors[i].slaveId = 0;
    packet.sensors[i].registerAddress = 0;
    packet.sensors[i].rawValue = sensorStates[i].rawValue;
    packet.sensors[i].engineeringValue = sensorStates[i].engineeringValue;
  }

  const esp_err_t status =
      esp_now_send(gBroadcastMac,
                   reinterpret_cast<const uint8_t *>(&packet),
                   sizeof(packet));
  if (status != ESP_OK) {
    ++gEspNowSendFailCount;
    if (isSerialHostConnected(48)) {
      Serial.print("ESP-NOW send failed: ");
      Serial.println(static_cast<int>(status));
    }
  }
}

uint32_t getSendCount() {
  return gEspNowSendCount;
}

uint32_t getSendFailCount() {
  return gEspNowSendFailCount;
}

}  // namespace EspNowHelper
