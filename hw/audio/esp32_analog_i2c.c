/*
 * ESP32 internal analog I2C register bridge.
 *
 * The ESP32 ROM accesses BBPLL/APLL byte registers through command words in
 * the analog peripheral.  The APLL calibration completion bit is modelled as
 * immediate because QEMU has no analog settling time.
 */

#include "qemu/osdep.h"
#include "hw/audio/esp32_analog_i2c.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#define ANALOG_I2C_APLL_COMMAND  0x0c
#define ANALOG_I2C_BBPLL_COMMAND 0x10
#define ANALOG_I2C_WRITE         BIT(24)
#define ANALOG_I2C_APLL_BLOCK    0x6d
#define ANALOG_I2C_APLL_CAL_REG  3
#define ANALOG_I2C_APLL_CAL_END  BIT(7)

static bool esp32_analog_i2c_is_command(hwaddr address)
{
    return address == ANALOG_I2C_APLL_COMMAND ||
           address == ANALOG_I2C_BBPLL_COMMAND;
}

static uint64_t esp32_analog_i2c_read(void *opaque, hwaddr address,
                                      unsigned size)
{
    Esp32AnalogI2cState *s = opaque;

    return address < sizeof(s->command)
               ? s->command[address / sizeof(uint32_t)]
               : 0;
}

static void esp32_analog_i2c_write(void *opaque, hwaddr address,
                                   uint64_t value, unsigned size)
{
    Esp32AnalogI2cState *s = opaque;
    const uint32_t command = value;

    if (!esp32_analog_i2c_is_command(address)) {
        return;
    }

    const uint8_t block = command;
    const uint8_t reg = command >> 8;
    uint8_t data = command >> 16;
    if (reg >= sizeof(s->registers[block])) {
        return;
    }
    if (command & ANALOG_I2C_WRITE) {
        s->registers[block][reg] = data;
    } else {
        data = s->registers[block][reg];
        if (block == ANALOG_I2C_APLL_BLOCK &&
            reg == ANALOG_I2C_APLL_CAL_REG) {
            data |= ANALOG_I2C_APLL_CAL_END;
        }
    }
    s->command[address / sizeof(uint32_t)] =
        (command & ~0x00ff0000U) | ((uint32_t)data << 16);
}

static const MemoryRegionOps esp32_analog_i2c_ops = {
    .read = esp32_analog_i2c_read,
    .write = esp32_analog_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void esp32_analog_i2c_reset(DeviceState *dev)
{
    Esp32AnalogI2cState *s = ESP32_ANALOG_I2C(dev);

    memset(s->command, 0, sizeof(s->command));
    memset(s->registers, 0, sizeof(s->registers));
}

static void esp32_analog_i2c_init(Object *obj)
{
    Esp32AnalogI2cState *s = ESP32_ANALOG_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32_analog_i2c_ops, s,
                          TYPE_ESP32_ANALOG_I2C, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void esp32_analog_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, esp32_analog_i2c_reset);
}

static const TypeInfo esp32_analog_i2c_info = {
    .name = TYPE_ESP32_ANALOG_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32AnalogI2cState),
    .instance_init = esp32_analog_i2c_init,
    .class_init = esp32_analog_i2c_class_init,
};

static void esp32_analog_i2c_register_types(void)
{
    type_register_static(&esp32_analog_i2c_info);
}

type_init(esp32_analog_i2c_register_types)
