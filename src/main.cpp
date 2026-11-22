#include <Arduino.h>

#include "espnow_helper.h"
#include "sensor_helper.h"

// --- SOFTWARE SENSOR SIMULATION CONFIGURATION ---
// Set to true to generate synthetic sensor data in software without any physical hardware.
// Keep false for real hardware: fuel on GPIO5, RPM on GPIO1, INA226 on SDA=8/SCL=9.
#ifndef SENSOR_SIMULATION
#define SENSOR_SIMULATION true
#endif
constexpr bool ENABLE_SENSOR_SIMULATION = SENSOR_SIMULATION;

// This node is intentionally simpler than the Modbus node.
// It samples fuel level, engine RPM, and INA226 power telemetry, then broadcasts
// those measurements over ESP-NOW using the same packet family as the main node.

namespace {

constexpr bool ENABLE_SERIAL_SUMMARY = true;
constexpr uint32_t SERIAL_SUMMARY_INTERVAL_MS = 3000;
constexpr uint32_t ESP_NOW_SEND_INTERVAL_MS = 200;
uint32_t lastFuelSampleMs = 0;
uint32_t lastRpmUpdateMs = 0;
uint32_t lastPowerSampleMs = 0;
uint32_t lastEspNowSendMs = 0;
uint32_t lastSerialSummaryMs = 0;

inline bool isSerialHostConnected(size_t minBytes = 1) {
  return static_cast<bool>(Serial) &&
         Serial.availableForWrite() >= static_cast<int>(minBytes);
}

void broadcastSensorSnapshot() {
  EspNowHelper::sendSensorSnapshot(
      SensorHelper::getSensorStates(),
      SensorHelper::getSensorCount(),
      millis());
}

void printSerialSummary() {
  if (!isSerialHostConnected(96)) {
    return;
  }

  Serial.print("Fuel raw=");
  const SensorState *sensorStates = SensorHelper::getSensorStates();

  Serial.print(sensorStates[0].rawValue);
  Serial.print(" fuel=%");
  Serial.print(sensorStates[0].engineeringValue, 1);
  Serial.print(" rpm=");
  Serial.print(sensorStates[1].engineeringValue, 1);
  Serial.print(" busV=");
  Serial.print(sensorStates[2].engineeringValue, 3);
  Serial.print(" currentA=");
  Serial.print(sensorStates[3].engineeringValue, 3);
  Serial.print(" pulses/window=");
  Serial.print(SensorHelper::getRpmWindowPulseCount());
  Serial.print(" espnow_ok=");
  Serial.print(EspNowHelper::getSendCount());
  Serial.print(" espnow_fail=");
  Serial.println(EspNowHelper::getSendFailCount());
}

}  // namespace

void setup() {
  #if defined(RGB_BUILTIN)
    neopixelWrite(RGB_BUILTIN, 0, 0, 0); // RGB off
  #endif

  Serial.begin(115200);
  // setTxTimeoutMs is not available in some ESP32 Arduino core versions.

  // USB CDC can take a short time to enumerate on ESP32-S3 boards.
  const uint32_t serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart) < 4000UL) {
    delay(10);
  }

  Serial.println("Starting");

  if (isSerialHostConnected(32)) {
    Serial.println();
    Serial.println("ESP32-2 Fuel/RPM broadcaster starting...");
  }

  SensorHelper::begin(ENABLE_SENSOR_SIMULATION);

  EspNowHelper::begin();
  SensorHelper::updateFuelLevel();
  SensorHelper::updateRpm();
  SensorHelper::updatePowerMonitor();

  if (isSerialHostConnected(64)) {
    Serial.println("Setup complete. Broadcasting fuel, RPM, voltage, and current snapshots.");
  }
}

void loop() {
  const uint32_t now = millis();

  if (now - lastFuelSampleMs >= SensorHelper::getFuelSampleIntervalMs()) {
    lastFuelSampleMs = now;
    SensorHelper::updateFuelLevel();
  }

  if (now - lastRpmUpdateMs >= SensorHelper::getRpmWindowMs()) {
    lastRpmUpdateMs = now;
    SensorHelper::updateRpm();
  }

  if (now - lastPowerSampleMs >= SensorHelper::getPowerSampleIntervalMs()) {
    lastPowerSampleMs = now;
    SensorHelper::updatePowerMonitor();
  }

  if (now - lastEspNowSendMs >= ESP_NOW_SEND_INTERVAL_MS) {
    lastEspNowSendMs = now;
    broadcastSensorSnapshot();
  }

  if (ENABLE_SERIAL_SUMMARY &&
      (now - lastSerialSummaryMs >= SERIAL_SUMMARY_INTERVAL_MS)) {
    lastSerialSummaryMs = now;
    printSerialSummary();
  }
}
