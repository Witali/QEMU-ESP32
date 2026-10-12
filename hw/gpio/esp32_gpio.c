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



struct EspGpioSink { qemu_irq irq; struct EspGpioSink *next; };

/* Transaction-level GPIO matrix routing used by modular peripherals. */
static unsigned gpio_iomux_offset(Esp32GpioState *s, unsigned pin)
{
    static const unsigned esp32_offset[40] = {
        0x44,0x88,0x40,0x84,0x48,0x6c,0x60,0x64,0x68,0x54,
        0x58,0x5c,0x34,0x38,0x30,0x3c,0x4c,0x50,0x70,0x74,
        0,0x7c,0x80,0x8c,0,0x24,0x28,0x2c,0,0,0,0,
        0x1c,0x20,0x14,0x18,0x04,0x08,0x0c,0x10
    };
    return s->matrix_c3 ? 4 * (pin + 1) : esp32_offset[pin];
}

static unsigned gpio_native_signal(Esp32GpioState *s, unsigned pin)
{
    unsigned fn = extract32(s->iomux_regs[gpio_iomux_offset(s,pin)/4],12,3);
    if (s->matrix_c3 && fn == 2) {
        switch (pin) {
        case 6: return 63;
        case 7: return 65;
        case 2: return 64;
        case 10: return 68;
        }
    } else if (!s->matrix_c3 && fn == 1) {
        switch (pin) {
        case 14: return 8;
        case 13: return 10;
        case 12: return 9;
        case 15: return 11;
        case 18: return 63;
        case 23: return 65;
        case 19: return 64;
        case 5: return 68;
        }
    }
    return UINT_MAX;
}

bool esp_gpio_output_enabled(Esp32GpioState *s, unsigned pin)
{
    return !s->matrix_enabled || (pin < s->pin_count &&
                                 (s->enabled & (UINT64_C(1) << pin)));
}

bool esp_gpio_has_signal(Esp32GpioState *s, unsigned pin,
                         unsigned signal, bool input)
{
    unsigned select, fn;
    if (pin >= s->pin_count || signal >= 256) return false;
    if (!s->matrix_enabled) return true; /* legacy machines retain behavior */
    if (gpio_native_signal(s,pin) == signal) return true;
    fn = extract32(s->iomux_regs[gpio_iomux_offset(s,pin)/4],12,3);
    if (fn != (s->matrix_c3 ? 1 : 2)) return false;
    select = input ? s->func_in[signal] : s->func_out[pin];
    if (input) {
        return (select & BIT(s->matrix_c3 ? 6 : 7)) &&
               (select & (s->matrix_c3 ? 31 : 63)) == pin;
    }
    return esp_gpio_output_enabled(s,pin) &&
           (select & (s->matrix_c3 ? 255 : 511)) == signal;
}

static unsigned gpio_pin_duty(Esp32GpioState *s, unsigned pin)
{
    unsigned signal = gpio_native_signal(s,pin);
    unsigned select = s->func_out[pin];
    unsigned mask = s->matrix_c3 ? 255 : 511;
    bool invert = select & BIT(s->matrix_c3 ? 8 : 9);
    unsigned duty;
    if (!esp_gpio_output_enabled(s,pin)) return 0;
    if (signal == UINT_MAX) signal = select & mask;
    if (signal == (s->matrix_c3 ? 128 : 256)) {
        duty = ((pin < 32 ? s->out >> pin : s->out1 >> (pin-32)) & 1) * 1000;
    } else {
        duty = signal < 256 ? s->signal_duty[signal] : 0;
    }
    return invert ? 1000 - duty : duty;
}

static void gpio_refresh(Esp32GpioState *s)
{
    if (!s->matrix_enabled) return;
    for (unsigned pin = 0; pin < s->pin_count; pin++) {
        unsigned duty = gpio_pin_duty(s,pin);
        qemu_set_irq(s->out_gpio[pin], duty != 0);
        qemu_set_irq(s->duty_gpio[pin], duty);
        for (struct EspGpioSink *sink=s->gpio_sinks[pin];sink;sink=sink->next) qemu_set_irq(sink->irq,duty!=0);
        for (struct EspGpioSink *sink=s->duty_sinks[pin];sink;sink=sink->next) qemu_set_irq(sink->irq,duty);
    }
}

void esp_gpio_set_signal_duty(Esp32GpioState *s, unsigned signal, unsigned duty)
{
    if (signal < 256) {
        s->signal_duty[signal] = MIN(duty,1000);
        gpio_refresh(s);
    }
}

static void gpio_signal_in(void *opaque, int signal, int level)
{
    esp_gpio_set_signal_duty(opaque,signal,level ? 1000 : 0);
}

void esp_gpio_connect(Esp32GpioState *s, unsigned pin, qemu_irq sink, bool pwm)
{
    struct EspGpioSink *binding = g_new0(struct EspGpioSink,1);
    struct EspGpioSink **list = pwm ? &s->duty_sinks[pin] : &s->gpio_sinks[pin];
    binding->irq=sink; binding->next=*list; *list=binding;
    qemu_set_irq(sink, pwm ? gpio_pin_duty(s,pin) : gpio_pin_duty(s,pin) != 0);
}

static uint64_t gpio_iomux_read(void *opaque, hwaddr addr, unsigned size)
{
    Esp32GpioState *s = opaque;
    return s->iomux_regs[addr/4];
}

static void gpio_iomux_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    Esp32GpioState *s = opaque;
    s->iomux_regs[addr/4] = value;
    gpio_refresh(s);
}

static const MemoryRegionOps gpio_iomux_ops = {
    .read = gpio_iomux_read, .write = gpio_iomux_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

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
    if (s->matrix_enabled) { gpio_refresh(s); return; }
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
    if (s->matrix_enabled) {
        unsigned in_base = s->matrix_c3 ? 0x154 : 0x130;
        unsigned out_base = s->matrix_c3 ? 0x554 : 0x530;
        if (addr >= in_base && addr < in_base + 256*4) return s->func_in[(addr-in_base)/4];
        if (addr >= out_base && addr < out_base + s->pin_count*4) return s->func_out[(addr-out_base)/4];
        if (addr == 0x20) return s->enabled;
        if (addr == 0x2c) return s->enabled >> 32;
    }
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

    if (s->matrix_enabled) {
        unsigned in_base = s->matrix_c3 ? 0x154 : 0x130;
        unsigned out_base = s->matrix_c3 ? 0x554 : 0x530;
        if (addr >= out_base && addr < out_base + s->pin_count*4) {
            s->func_out[(addr-out_base)/4] = value; gpio_refresh(s); return;
        }
        if (addr >= in_base && addr < in_base + 256*4) {
            s->func_in[(addr-in_base)/4] = value; return;
        }
        switch (addr) {
        case 0x20: s->enabled = (s->enabled & ~UINT64_C(0xffffffff)) | value; break;
        case 0x24: s->enabled |= value; break;
        case 0x28: s->enabled &= ~value; break;
        case 0x2c: s->enabled = (s->enabled & UINT64_C(0xffffffff)) | (value << 32); break;
        case 0x30: s->enabled |= value << 32; break;
        case 0x34: s->enabled &= ~(value << 32); break;
        default: goto regular_write;
        }
        gpio_refresh(s); return;
    }
regular_write:
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

    s->enabled = 0;
    memset(s->func_in,0,sizeof(s->func_in));
    memset(s->signal_duty,0,sizeof(s->signal_duty));
    memset(s->iomux_regs,0,sizeof(s->iomux_regs));
    for (unsigned i=0;i<40;i++) s->func_out[i] = s->matrix_c3 ? 128 : 256;
    for (unsigned i=0;i<s->pin_count;i++) s->iomux_regs[gpio_iomux_offset(s,i)/4] = (s->matrix_c3 ? 1 : 2) << 12;
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
    memory_region_init_io(&s->iomux,obj,&gpio_iomux_ops,s,"esp.gpio.iomux",0x400);
    sysbus_init_mmio(sbd,&s->iomux);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(DEVICE(s),s->duty_gpio,"gpio-duty",40);
    qdev_init_gpio_in_named(DEVICE(s),gpio_signal_in,"func-in",256);
    qdev_init_gpio_out_named(DEVICE(s), s->out_gpio,
                             "gpio-out", ESP32_GPIO_PIN_COUNT);
}

static Property esp32_gpio_properties[] = {
    /* The strap_mode needs to be explicitly set in the instance init, thus, set
     * the default value to 0. */
    DEFINE_PROP_BOOL("matrix-enabled",Esp32GpioState,matrix_enabled,false),
    DEFINE_PROP_BOOL("matrix-c3",Esp32GpioState,matrix_c3,false),
    DEFINE_PROP_UINT32("pin-count",Esp32GpioState,pin_count,40),
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
