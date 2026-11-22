#include "sensor_helper.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/pcnt.h>

namespace {

constexpr uint8_t FUEL_ADC_PIN = 5;   // GPIO5 / ADC1_4
constexpr uint8_t RPM_INPUT_PIN = 1;  // GPIO1 pulse input
constexpr uint8_t INA226_SDA_PIN = 8;
constexpr uint8_t INA226_SCL_PIN = 9;
constexpr uint8_t INA226_I2C_ADDRESS = 0x40;

constexpr uint8_t INA226_REG_CONFIG = 0x00;
constexpr uint8_t INA226_REG_BUS_VOLTAGE = 0x02;
constexpr uint8_t INA226_REG_CURRENT = 0x04;
constexpr uint8_t INA226_REG_CALIBRATION = 0x05;

constexpr uint32_t FUEL_SAMPLE_INTERVAL_MS = 1000;
constexpr uint32_t RPM_WINDOW_MS = 300;
constexpr uint32_t RPM_ZERO_TIMEOUT_MS = 1000;
constexpr uint32_t POWER_SAMPLE_INTERVAL_MS = 500;

constexpr uint16_t FUEL_EMPTY_MV = 0; // Volts x 1000
constexpr uint16_t FUEL_FULL_MV = 1165; // 3.3V * 180/(330+180) * 1000
constexpr uint16_t FUEL_SIM_EMPTY_RAW = 0;
constexpr uint16_t FUEL_SIM_FULL_RAW = 4095;
constexpr float FUEL_FILTER_ALPHA = 0.05f;

constexpr float RPM_FILTER_ALPHA_UP = 0.15f;
constexpr float RPM_FILTER_ALPHA_DOWN = 0.10f;
constexpr float RPM_FILTER_DEADBAND = 4.0f;

constexpr uint16_t RPM_PULSES_PER_REVOLUTION = 94;
constexpr uint8_t SENSOR_COUNT = 4;
constexpr pcnt_unit_t RPM_PCNT_UNIT = PCNT_UNIT_0;
constexpr pcnt_channel_t RPM_PCNT_CHANNEL = PCNT_CHANNEL_0;
constexpr int16_t RPM_PCNT_COUNTER_H_LIMIT = 10000;
constexpr int16_t RPM_PCNT_COUNTER_L_LIMIT = 0;

constexpr float INA226_BUS_VOLTAGE_LSB_VOLTS = 0.00125f;
constexpr float INA226_SHUNT_RESISTOR_OHMS = 0.005f;   // Typical 20A INA226 module shunt (R005)
constexpr uint16_t INA226_CALIBRATION_VALUE = 1024;    // Sets current LSB near 1 mA with 0.005 ohm shunt
constexpr float INA226_CURRENT_LSB_AMPS =
  0.00512f /
  (static_cast<float>(INA226_CALIBRATION_VALUE) * INA226_SHUNT_RESISTOR_OHMS);
constexpr uint16_t INA226_CONFIG_DEFAULT = 0x4127;  // Continuous bus + shunt conversion
constexpr float POWER_SIM_VOLTAGE_MIN_V = 12.0f;
constexpr float POWER_SIM_VOLTAGE_MAX_V = 14.8f;
constexpr float POWER_SIM_CURRENT_MIN_A = 0.0f;
constexpr float POWER_SIM_CURRENT_MAX_A = 8.0f;
constexpr float POWER_SIM_VOLTAGE_STEP_V = 0.40f;
constexpr float POWER_SIM_CURRENT_STEP_A = 1.00f;

SensorState gSensorStates[SENSOR_COUNT] = {
    {"FuelLev1", "%", 0, 0.0f, true, false},
    {"EngRPM1", "rpm", 0, 0.0f, true, false},
    {"BusV1", "V", 0, 0.0f, true, false},
    {"Curr1", "A", 0, 0.0f, true, false},
};

uint32_t gLastPulseMicros = 0;

bool gEnableSensorSimulation = false;
bool gFuelAdcReady = false;
bool gFuelFilterInitialized = false;
float gFilteredFuelMilliVolts = 0.0f;
bool gRpmFilterInitialized = false;
float gFilteredRpm = 0.0f;
float gCurrentRpm = 0.0f;
uint32_t gRpmWindowPulseCount = 0;
bool gPcntInitialized = false;
bool gIna226Ready = false;

float gSimFuelRaw = 0.0f;
float gSimFuelStep = 30.0f;
float gSimRpmValue = 0.0f;
float gSimRpmStep = 250.0f;
float gSimVoltageValue = POWER_SIM_VOLTAGE_MIN_V;
float gSimVoltageStep = POWER_SIM_VOLTAGE_STEP_V;
float gSimCurrentValue = POWER_SIM_CURRENT_MIN_A;
float gSimCurrentStep = POWER_SIM_CURRENT_STEP_A;

bool writeIna226Register(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(INA226_I2C_ADDRESS);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>((value >> 8) & 0xFF));
  Wire.write(static_cast<uint8_t>(value & 0xFF));
  return Wire.endTransmission() == 0;
}

bool readIna226Register(uint8_t reg, uint16_t &value) {
  Wire.beginTransmission(INA226_I2C_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(static_cast<int>(INA226_I2C_ADDRESS), 2) != 2) {
    return false;
  }

  const uint8_t msb = Wire.read();
  const uint8_t lsb = Wire.read();
  value = static_cast<uint16_t>((static_cast<uint16_t>(msb) << 8) | lsb);
  return true;
}

bool initializeIna226() {
  // Set continuous measurement mode and calibration for common 0-36V/20A INA226 modules.
  if (!writeIna226Register(INA226_REG_CONFIG, INA226_CONFIG_DEFAULT)) {
    return false;
  }
  if (!writeIna226Register(INA226_REG_CALIBRATION, INA226_CALIBRATION_VALUE)) {
    return false;
  }

  uint16_t verify = 0;
  if (!readIna226Register(INA226_REG_CONFIG, verify)) {
    return false;
  }
  return true;
}

inline bool isSerialHostConnected(size_t minBytes = 1) {
  return static_cast<bool>(Serial) &&
         Serial.availableForWrite() >= static_cast<int>(minBytes);
}

int8_t getAnalogChannelForPin(uint8_t pin) {
#if defined(digitalPinToAnalogChannel)
  return static_cast<int8_t>(digitalPinToAnalogChannel(pin));
#else
  return (pin >= 1 && pin <= 20) ? static_cast<int8_t>(pin - 1) : static_cast<int8_t>(-1);
#endif
}

float clampPercent(float value) {
  if (value < 0.0f) {
    return 0.0f;
  }
  if (value > 100.0f) {
    return 100.0f;
  }
  return value;
}

float fuelMilliVoltsToPercent(float milliVolts) {
  if (FUEL_FULL_MV <= FUEL_EMPTY_MV) {
    return 0.0f;
  }

  const float percent =
      ((milliVolts - static_cast<float>(FUEL_EMPTY_MV)) * 100.0f) /
      static_cast<float>(FUEL_FULL_MV - FUEL_EMPTY_MV);
  return clampPercent(percent);
}

float filterRpm(float measuredRpm, bool forceZero) {
  if (forceZero) {
    gFilteredRpm = 0.0f;
    gRpmFilterInitialized = true;
    return gFilteredRpm;
  }

  if (measuredRpm < 0.0f) {
    measuredRpm = 0.0f;
  }

  if (!gRpmFilterInitialized) {
    gFilteredRpm = measuredRpm;
    gRpmFilterInitialized = true;
    return gFilteredRpm;
  }

  const float delta = measuredRpm - gFilteredRpm;
  if (fabsf(delta) < RPM_FILTER_DEADBAND) {
    return gFilteredRpm;
  }

  const float alpha = (delta >= 0.0f) ? RPM_FILTER_ALPHA_UP : RPM_FILTER_ALPHA_DOWN;
  gFilteredRpm += delta * alpha;
  if (gFilteredRpm < 0.0f) {
    gFilteredRpm = 0.0f;
  }
  return gFilteredRpm;
}

bool initializeRpmPulseCounter() {
  if (gPcntInitialized) {
    return true;
  }

  pcnt_config_t pcntConfig = {};
  pcntConfig.pulse_gpio_num = RPM_INPUT_PIN;
  pcntConfig.ctrl_gpio_num = -1;
  pcntConfig.lctrl_mode = PCNT_MODE_KEEP;
  pcntConfig.hctrl_mode = PCNT_MODE_KEEP;
  pcntConfig.pos_mode = PCNT_COUNT_DIS;
  pcntConfig.neg_mode = PCNT_COUNT_INC;
  pcntConfig.counter_h_lim = RPM_PCNT_COUNTER_H_LIMIT;
  pcntConfig.counter_l_lim = RPM_PCNT_COUNTER_L_LIMIT;
  pcntConfig.unit = RPM_PCNT_UNIT;
  pcntConfig.channel = RPM_PCNT_CHANNEL;

  const esp_err_t initErr = pcnt_unit_config(&pcntConfig);
  if (initErr != ESP_OK) {
    if (isSerialHostConnected(64)) {
      Serial.print("PCNT init failed: ");
      Serial.println(esp_err_to_name(initErr));
    }
    return false;
  }

  const esp_err_t clearErr = pcnt_counter_clear(RPM_PCNT_UNIT);
  if (clearErr != ESP_OK) {
    if (isSerialHostConnected(64)) {
      Serial.print("PCNT clear failed: ");
      Serial.println(esp_err_to_name(clearErr));
    }
    return false;
  }

  const esp_err_t resumeErr = pcnt_counter_resume(RPM_PCNT_UNIT);
  if (resumeErr != ESP_OK) {
    if (isSerialHostConnected(64)) {
      Serial.print("PCNT resume failed: ");
      Serial.println(esp_err_to_name(resumeErr));
    }
    return false;
  }

  gPcntInitialized = true;
  return true;
}

}  // namespace

namespace SensorHelper {

void begin(bool enableSensorSimulation) {
  gEnableSensorSimulation = enableSensorSimulation;

  pinMode(RPM_INPUT_PIN, INPUT_PULLUP);
  if (!gEnableSensorSimulation) {
    initializeRpmPulseCounter();
  }

  analogReadResolution(12);
  analogSetAttenuation(ADC_2_5db);
  const int8_t analogChannel = getAnalogChannelForPin(FUEL_ADC_PIN);
  if (analogChannel >= 0) {
    gFuelAdcReady = true;
  } else if (isSerialHostConnected(64)) {
    Serial.print("Fuel ADC pin is not valid for analog input: GPIO");
    Serial.println(FUEL_ADC_PIN);
  }

  Wire.begin(INA226_SDA_PIN, INA226_SCL_PIN);
  gIna226Ready = initializeIna226();
  if (!gIna226Ready && isSerialHostConnected(64)) {
    Serial.println("INA226 init failed on SDA=8/SCL=9 (addr 0x40)");
  }
}

void updateFuelLevel() {
  if (!gFuelAdcReady && !gEnableSensorSimulation) {
    gSensorStates[0].valid = false;
    return;
  }

  uint16_t rawSample = 0;
  float sampleMilliVolts = 0.0f;

  if (gEnableSensorSimulation) {
    gSimFuelRaw += gSimFuelStep;
    if (gSimFuelRaw >= static_cast<float>(FUEL_SIM_FULL_RAW)) {
      gSimFuelRaw = static_cast<float>(FUEL_SIM_FULL_RAW);
      gSimFuelStep = -gSimFuelStep;
    } else if (gSimFuelRaw <= static_cast<float>(FUEL_SIM_EMPTY_RAW)) {
      gSimFuelRaw = static_cast<float>(FUEL_SIM_EMPTY_RAW);
      gSimFuelStep = -gSimFuelStep;
    }
    rawSample = static_cast<uint16_t>(gSimFuelRaw);
    sampleMilliVolts =
        (static_cast<float>(rawSample) * static_cast<float>(FUEL_FULL_MV)) /
        static_cast<float>(FUEL_SIM_FULL_RAW);
  } else {
    rawSample = analogRead(FUEL_ADC_PIN);
    sampleMilliVolts = static_cast<float>(analogReadMilliVolts(FUEL_ADC_PIN));
  }

  if (!gFuelFilterInitialized) {
    gFilteredFuelMilliVolts = sampleMilliVolts;
    gFuelFilterInitialized = true;
  } else {
    gFilteredFuelMilliVolts += (sampleMilliVolts - gFilteredFuelMilliVolts) *
                               FUEL_FILTER_ALPHA;
  }

  gSensorStates[0].rawValue = rawSample;
  gSensorStates[0].engineeringValue = fuelMilliVoltsToPercent(gFilteredFuelMilliVolts);
  gSensorStates[0].valid = true;
}

void updateRpm() {
  if (gEnableSensorSimulation) {
    gSimRpmValue += gSimRpmStep;
    if (gSimRpmValue >= 7000.0f) {
      gSimRpmValue = 7000.0f;
      gSimRpmStep = -gSimRpmStep;
    } else if (gSimRpmValue <= 0.0f) {
      gSimRpmValue = 0.0f;
      gSimRpmStep = -gSimRpmStep;
    }
    const float measuredRpm = gSimRpmValue;

    const float windowSeconds = static_cast<float>(RPM_WINDOW_MS) / 1000.0f;
    gRpmWindowPulseCount = static_cast<uint32_t>(
        (measuredRpm * static_cast<float>(RPM_PULSES_PER_REVOLUTION) * windowSeconds) / 60.0f);

    gCurrentRpm = filterRpm(measuredRpm, false);

    gSensorStates[1].rawValue =
        static_cast<uint16_t>(gRpmWindowPulseCount > 65535 ? 65535 : gRpmWindowPulseCount);
    gSensorStates[1].engineeringValue = gCurrentRpm;
    gSensorStates[1].valid = true;
    return;
  }

  uint32_t pulses = 0;
  uint32_t lastPulseSnapshot = 0;

  if (gPcntInitialized) {
    int16_t pcntValue = 0;
    if (pcnt_get_counter_value(RPM_PCNT_UNIT, &pcntValue) == ESP_OK) {
      pulses = static_cast<uint32_t>(pcntValue > 0 ? pcntValue : 0);
      if (pulses > 0) {
        gLastPulseMicros = micros();
      }
    }
    pcnt_counter_clear(RPM_PCNT_UNIT);
  }

  lastPulseSnapshot = gLastPulseMicros;
  gRpmWindowPulseCount = pulses;

  const float windowSeconds = static_cast<float>(RPM_WINDOW_MS) / 1000.0f;
  float measuredRpm = 0.0f;
  bool forceRpmZero = false;
  if (windowSeconds > 0.0f) {
    measuredRpm = (static_cast<float>(pulses) * 60.0f) /
                  (static_cast<float>(RPM_PULSES_PER_REVOLUTION) * windowSeconds);
  }

  if (lastPulseSnapshot == 0 ||
      (micros() - lastPulseSnapshot) > (RPM_ZERO_TIMEOUT_MS * 1000UL)) {
    measuredRpm = 0.0f;
    forceRpmZero = true;
  }

  gCurrentRpm = filterRpm(measuredRpm, forceRpmZero);

  gSensorStates[1].rawValue = static_cast<uint16_t>(pulses > 65535 ? 65535 : pulses);
  gSensorStates[1].engineeringValue = gCurrentRpm;
  gSensorStates[1].valid = true;
}

void updatePowerMonitor() {
  if (gEnableSensorSimulation) {
    gSimVoltageValue += gSimVoltageStep;
    if (gSimVoltageValue >= POWER_SIM_VOLTAGE_MAX_V) {
      gSimVoltageValue = POWER_SIM_VOLTAGE_MAX_V;
      gSimVoltageStep = -gSimVoltageStep;
    } else if (gSimVoltageValue <= POWER_SIM_VOLTAGE_MIN_V) {
      gSimVoltageValue = POWER_SIM_VOLTAGE_MIN_V;
      gSimVoltageStep = -gSimVoltageStep;
    }

    gSimCurrentValue += gSimCurrentStep;
    if (gSimCurrentValue >= POWER_SIM_CURRENT_MAX_A) {
      gSimCurrentValue = POWER_SIM_CURRENT_MAX_A;
      gSimCurrentStep = -gSimCurrentStep;
    } else if (gSimCurrentValue <= POWER_SIM_CURRENT_MIN_A) {
      gSimCurrentValue = POWER_SIM_CURRENT_MIN_A;
      gSimCurrentStep = -gSimCurrentStep;
    }

    const uint16_t simulatedBusRaw =
        static_cast<uint16_t>(gSimVoltageValue / INA226_BUS_VOLTAGE_LSB_VOLTS);
    const int16_t simulatedCurrentRawSigned =
        static_cast<int16_t>(gSimCurrentValue / INA226_CURRENT_LSB_AMPS);

    gSensorStates[2].rawValue = simulatedBusRaw;
    gSensorStates[2].engineeringValue = gSimVoltageValue;
    gSensorStates[2].valid = true;

    gSensorStates[3].rawValue = static_cast<uint16_t>(simulatedCurrentRawSigned);
    gSensorStates[3].engineeringValue = gSimCurrentValue;
    gSensorStates[3].valid = true;
    return;
  }

  if (!gIna226Ready) {
    gSensorStates[2].valid = false;
    gSensorStates[3].valid = false;
    return;
  }

  uint16_t busVoltageRaw = 0;
  uint16_t currentRawUnsigned = 0;

  if (!readIna226Register(INA226_REG_BUS_VOLTAGE, busVoltageRaw) ||
      !readIna226Register(INA226_REG_CURRENT, currentRawUnsigned)) {
    gSensorStates[2].valid = false;
    gSensorStates[3].valid = false;
    return;
  }

  const float busVoltage = static_cast<float>(busVoltageRaw) * INA226_BUS_VOLTAGE_LSB_VOLTS;
  const int16_t currentRawSigned = static_cast<int16_t>(currentRawUnsigned);
  const float currentAmps = static_cast<float>(currentRawSigned) * INA226_CURRENT_LSB_AMPS;

  gSensorStates[2].rawValue = busVoltageRaw;
  gSensorStates[2].engineeringValue = busVoltage;
  gSensorStates[2].valid = true;

  gSensorStates[3].rawValue = currentRawUnsigned;
  gSensorStates[3].engineeringValue = currentAmps;
  gSensorStates[3].valid = true;
}

const SensorState *getSensorStates() {
  return gSensorStates;
}

uint8_t getSensorCount() {
  return SENSOR_COUNT;
}

uint32_t getRpmWindowPulseCount() {
  return gRpmWindowPulseCount;
}

uint32_t getFuelSampleIntervalMs() {
  return FUEL_SAMPLE_INTERVAL_MS;
}

uint32_t getRpmWindowMs() {
  return RPM_WINDOW_MS;
}

uint32_t getPowerSampleIntervalMs() {
  return POWER_SAMPLE_INTERVAL_MS;
}

}  // namespace SensorHelper
