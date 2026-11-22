#pragma once

#include <stdint.h>

#include "sensor_types.h"

namespace EspNowHelper {

void begin();
bool isReady();
void sendSensorSnapshot(const SensorState *sensorStates,
                        uint8_t sensorCount,
                        uint32_t uptimeMs);
uint32_t getSendCount();
uint32_t getSendFailCount();

}  // namespace EspNowHelper
