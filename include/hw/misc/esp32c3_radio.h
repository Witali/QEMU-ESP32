/*
 * ESP32-C3 radio analog/PHY compatibility registers
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */
#pragma once

#include "hw/sysbus.h"

#define TYPE_ESP32C3_RADIO "esp32c3-radio"
OBJECT_DECLARE_SIMPLE_TYPE(Esp32C3RadioState, ESP32C3_RADIO)

#define ESP32C3_RADIO_MMIO_SIZE 0x1000
#define ESP32C3_RADIO_PHYA_ACK  0x00000000
#define ESP32C3_RADIO_PHYA_NACK 0x00002300

enum Esp32C3RadioRegion {
    ESP32C3_RADIO_REGION_ANA,
    ESP32C3_RADIO_REGION_PHYA,
    ESP32C3_RADIO_REGION_FE,
    ESP32C3_RADIO_REGION_PWR,
    ESP32C3_RADIO_REGION_COUNT,
};

typedef struct Esp32C3RadioBank {
    MemoryRegion iomem;
    uint32_t regs[ESP32C3_RADIO_MMIO_SIZE / sizeof(uint32_t)];
    struct Esp32C3RadioState *radio;
    enum Esp32C3RadioRegion region;
} Esp32C3RadioBank;

struct Esp32C3RadioState {
    SysBusDevice parent_obj;

    Esp32C3RadioBank bank[ESP32C3_RADIO_REGION_COUNT];
    uint8_t channel;
};

void esp32c3_radio_set_packet_status(Esp32C3RadioState *s, uint32_t status);
