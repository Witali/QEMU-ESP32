/* ESP32-C2/C3 register-layout GP-SPI master. GPL-2.0-or-later. */
#pragma once
#include "hw/sysbus.h"
#include "hw/ssi/ssi.h"
#include "hw/dma/esp_gdma.h"
#define TYPE_ESP32C2_GPSPI "esp32c2.gpspi"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32C2GpspiState, ESP32C2_GPSPI)
struct ESP32C2GpspiState {
    SysBusDevice parent;
    MemoryRegion iomem;
    SSIBus *bus;
    qemu_irq irq, cs[6];
    ESPGdmaState *gdma;
    uint32_t regs[0x100/4];
};
