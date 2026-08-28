/*
 * ESP32-C3 radio analog/PHY compatibility registers
 *
 * The reset values model the small subset polled by the closed ESP-IDF
 * Wi-Fi libraries.  The layout is based on the open PICSimLab C3 model.
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/guest-random.h"
#include "hw/misc/esp32c3_radio.h"

#define ESP32C3_RADIO_PHYA_PACKET_STATUS 0x320

static uint64_t esp32c3_radio_read(void *opaque, hwaddr addr, unsigned size)
{
    Esp32C3RadioBank *bank = opaque;
    uint32_t value = bank->regs[addr / sizeof(uint32_t)];

    switch (bank->region) {
    case ESP32C3_RADIO_REGION_ANA:
        switch (addr) {
        case 0x000:
        case 0x048:
            return 0x00ffffff;
        case 0x004:
            return 0xfdffffff;
        case 0x044:
        case 0x04c:
        case 0x0c4:
            return UINT32_MAX;
        case 0x050:
            return 0x07000000 | value;
        default:
            break;
        }
        break;
    case ESP32C3_RADIO_REGION_FE:
        if (addr == 0x07c || addr == 0x174) {
            return UINT32_MAX;
        }
        break;
    case ESP32C3_RADIO_REGION_PWR:
        switch (addr) {
        case 0x07c:
            qemu_guest_getrandom_nofail(&value, sizeof(value));
            return value;
        case 0x118:
        case 0x128:
            return 0;
        default:
            break;
        }
        break;
    case ESP32C3_RADIO_REGION_PHYA:
        break;
    default:
        g_assert_not_reached();
    }

    return value;
}

static void esp32c3_radio_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    Esp32C3RadioBank *bank = opaque;

    bank->regs[addr / sizeof(uint32_t)] = value;

    if (bank->region == ESP32C3_RADIO_REGION_ANA && addr == 0x150) {
        int channel = (((value >> 20) & 0xff) - 7) / 5;

        bank->radio->channel = CLAMP(channel, 0, 14);
    }
}

static const MemoryRegionOps esp32c3_radio_ops = {
    .read = esp32c3_radio_read,
    .write = esp32c3_radio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

void esp32c3_radio_set_packet_status(Esp32C3RadioState *s, uint32_t status)
{
    s->bank[ESP32C3_RADIO_REGION_PHYA]
        .regs[ESP32C3_RADIO_PHYA_PACKET_STATUS / sizeof(uint32_t)] = status;
}

static void esp32c3_radio_reset(DeviceState *dev)
{
    Esp32C3RadioState *s = ESP32C3_RADIO(dev);
    int i;

    for (i = 0; i < ESP32C3_RADIO_REGION_COUNT; i++) {
        memset(s->bank[i].regs, 0, sizeof(s->bank[i].regs));
    }
    s->channel = 0;
}

static void esp32c3_radio_init(Object *obj)
{
    static const char *const names[ESP32C3_RADIO_REGION_COUNT] = {
        "esp32c3.radio.ana",
        "esp32c3.radio.phya",
        "esp32c3.radio.fe",
        "esp32c3.radio.pwr",
    };
    Esp32C3RadioState *s = ESP32C3_RADIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    int i;

    for (i = 0; i < ESP32C3_RADIO_REGION_COUNT; i++) {
        Esp32C3RadioBank *bank = &s->bank[i];

        bank->radio = s;
        bank->region = i;
        memory_region_init_io(&bank->iomem, obj, &esp32c3_radio_ops, bank,
                              names[i], ESP32C3_RADIO_MMIO_SIZE);
        sysbus_init_mmio(sbd, &bank->iomem);
    }
}

static void esp32c3_radio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "ESP32-C3 radio analog and PHY compatibility registers";
    device_class_set_legacy_reset(dc, esp32c3_radio_reset);
}

static const TypeInfo esp32c3_radio_info = {
    .name = TYPE_ESP32C3_RADIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32C3RadioState),
    .instance_init = esp32c3_radio_init,
    .class_init = esp32c3_radio_class_init,
};

static void esp32c3_radio_register_types(void)
{
    type_register_static(&esp32c3_radio_info);
}

type_init(esp32c3_radio_register_types)
