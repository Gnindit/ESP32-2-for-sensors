#pragma once

#include <stdint.h>

#include "sensor_types.h"

namespace SensorHelper {

void begin(bool enableSensorSimulation);
void updateFuelLevel();
void updateRpm();
void updatePowerMonitor();

const SensorState *getSensorStates();
uint8_t getSensorCount();
uint32_t getRpmWindowPulseCount();

uint32_t getFuelSampleIntervalMs();
uint32_t getRpmWindowMs();
uint32_t getPowerSampleIntervalMs();

}  // namespace SensorHelper
