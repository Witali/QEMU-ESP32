#pragma once

#include "hw/sysbus.h"

#define TYPE_ESP32_ANALOG_I2C "esp32-analog-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(Esp32AnalogI2cState, ESP32_ANALOG_I2C)

struct Esp32AnalogI2cState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t command[5];
    uint8_t registers[256][16];
};
