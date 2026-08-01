/*
 * ESP32 GPIO emulation
 *
 * Copyright (c) 2019 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/gpio/esp32_gpio.h"



REG32(GPIO_OUT, 0x0004)
REG32(GPIO_OUT_W1TS, 0x0008)
REG32(GPIO_OUT_W1TC, 0x000c)
REG32(GPIO_OUT1, 0x0010)
REG32(GPIO_OUT1_W1TS, 0x0014)
REG32(GPIO_OUT1_W1TC, 0x0018)

static void esp32_gpio_set_outputs(Esp32GpioState *s,
                                   uint32_t out, uint32_t out1)
{
    s->out = out;
    s->out1 = out1 & 0xff;
    for (int pin = 0; pin < ESP32_GPIO_PIN_COUNT; pin++) {
        uint32_t level = pin < 32
            ? extract32(s->out, pin, 1)
            : extract32(s->out1, pin - 32, 1);
        qemu_set_irq(s->out_gpio[pin], level);
    }
}

void esp32_gpio_set_input(Esp32GpioState *s, unsigned pin, int level)
{
    if (pin >= ESP32_GPIO_PIN_COUNT) {
        return;
    }
    if (pin < 32) {
        s->in = deposit32(s->in, pin, 1, level != 0);
    } else {
        s->in1 = deposit32(s->in1, pin - 32, 1, level != 0);
    }
}

static uint64_t esp32_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);
    uint64_t r = 0;
    switch (addr) {
    case A_GPIO_OUT:
        r = s->out;
        break;
    case A_GPIO_OUT1:
        r = s->out1;
        break;
    case A_GPIO_STRAP:
        r = s->strap_mode;
        break;
    case A_GPIO_IN:
        r = s->in;
        break;
    case A_GPIO_IN1:
        r = s->in1;
        break;

    default:
        break;
    }
    return r;
}

static void esp32_gpio_write(void *opaque, hwaddr addr,
                       uint64_t value, unsigned int size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);

    switch (addr) {
    case A_GPIO_OUT:
        esp32_gpio_set_outputs(s, value, s->out1);
        break;
    case A_GPIO_OUT_W1TS:
        esp32_gpio_set_outputs(s, s->out | value, s->out1);
        break;
    case A_GPIO_OUT_W1TC:
        esp32_gpio_set_outputs(s, s->out & ~value, s->out1);
        break;
    case A_GPIO_OUT1:
        esp32_gpio_set_outputs(s, s->out, value);
        break;
    case A_GPIO_OUT1_W1TS:
        esp32_gpio_set_outputs(s, s->out, s->out1 | value);
        break;
    case A_GPIO_OUT1_W1TC:
        esp32_gpio_set_outputs(s, s->out, s->out1 & ~value);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps uart_ops = {
    .read =  esp32_gpio_read,
    .write = esp32_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32_gpio_reset_hold(Object *obj, ResetType type)
{
    Esp32GpioState *s = ESP32_GPIO(obj);

    esp32_gpio_set_outputs(s, 0, 0);
}

static void esp32_gpio_realize(DeviceState *dev, Error **errp)
{
}

static void esp32_gpio_init(Object *obj)
{
    Esp32GpioState *s = ESP32_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /* Set the default value for the strap_mode property */
    object_property_set_int(obj, "strap_mode", ESP32_STRAP_MODE_FLASH_BOOT, &error_fatal);
    /* GPIO0 has an external pull-up and the BOOT button drives it low. */
    s->in = 1;

    memory_region_init_io(&s->iomem, obj, &uart_ops, s,
                          TYPE_ESP32_GPIO, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(DEVICE(s), s->out_gpio,
                             "gpio-out", ESP32_GPIO_PIN_COUNT);
}

static Property esp32_gpio_properties[] = {
    /* The strap_mode needs to be explicitly set in the instance init, thus, set
     * the default value to 0. */
    DEFINE_PROP_UINT32("strap_mode", Esp32GpioState, strap_mode, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32_gpio_reset_hold;
    dc->realize = esp32_gpio_realize;
    device_class_set_props(dc, esp32_gpio_properties);
}

static const TypeInfo esp32_gpio_info = {
    .name = TYPE_ESP32_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32GpioState),
    .instance_init = esp32_gpio_init,
    .class_init = esp32_gpio_class_init,
    .class_size = sizeof(Esp32GpioClass),
};

static void esp32_gpio_register_types(void)
{
    type_register_static(&esp32_gpio_info);
}

type_init(esp32_gpio_register_types)
