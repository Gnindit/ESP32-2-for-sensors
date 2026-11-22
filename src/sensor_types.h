#pragma once

#include <stdint.h>

struct SensorState {
  const char *name;
  const char *unit;
  uint16_t rawValue;
  float engineeringValue;
  bool active;
  bool valid;
};
