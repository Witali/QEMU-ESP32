/*
 * ESP32 I2S0 TX DMA and built-in DAC output.
 *
 * This implements the register and descriptor subset used by the ESP-IDF
 * dac_continuous driver.  GPIO26 is DAC channel 1 on the physical
 * ESP32-2432S028 board; the I2S DMA stream contains unsigned 8-bit samples in
 * the high byte of each 16-bit word.
 */

#include "qemu/osdep.h"
#include "audio/audio.h"
#include "exec/address-spaces.h"
#include "hw/audio/esp32_i2s_dac.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define I2S_CONF_REG               0x08
#define I2S_INT_RAW_REG            0x0c
#define I2S_INT_ST_REG             0x10
#define I2S_INT_ENA_REG            0x14
#define I2S_INT_CLR_REG            0x18
#define I2S_OUT_LINK_REG           0x30
#define I2S_OUT_EOF_DES_ADDR_REG   0x38
#define I2S_LC_CONF_REG            0x60

#define I2S_CONF_TX_RESET          BIT(0)
#define I2S_CONF_TX_START          BIT(4)
#define I2S_INT_OUT_EOF            BIT(12)
#define I2S_INT_OUT_DSCR_ERR       BIT(14)
#define I2S_INT_OUT_TOTAL_EOF      BIT(16)
#define I2S_OUT_LINK_ADDR_MASK     0x000fffffU
#define I2S_OUT_LINK_STOP          BIT(28)
#define I2S_OUT_LINK_START         BIT(29)
#define I2S_OUT_LINK_PARK          BIT(31)
#define I2S_LC_OUT_RESET           BIT(1)
#define I2S_LC_OUT_AUTO_WRBACK     BIT(6)
#define I2S_LC_CHECK_OWNER         BIT(12)

#define DMA_DESCRIPTOR_PREFIX      0x3ff00000U
#define DMA_DESCRIPTOR_MAX_BYTES   4092U
#define DMA_DESCRIPTOR_SIZE_MASK   0x00000fffU
#define DMA_DESCRIPTOR_LENGTH_MASK 0x00fff000U
#define DMA_DESCRIPTOR_LENGTH_SHIFT 12
#define DMA_DESCRIPTOR_OWNER       BIT(31)

static void esp32_i2s_dac_update_irq(Esp32I2sDacState *s)
{
    const uint32_t status =
        s->regs[I2S_INT_RAW_REG / 4] & s->regs[I2S_INT_ENA_REG / 4];
    qemu_set_irq(s->irq, status != 0);
}

static void esp32_i2s_dac_set_active(Esp32I2sDacState *s, bool active)
{
    if (s->voice) {
        AUD_set_active_out(s->voice, active);
    }
}

static void esp32_i2s_dac_stop(Esp32I2sDacState *s)
{
    s->link_running = false;
    s->current_descriptor = 0;
    timer_del(s->dma_timer);
    esp32_i2s_dac_set_active(s, false);
}

static void esp32_i2s_dac_push_sample(Esp32I2sDacState *s, uint8_t sample)
{
    if (s->audio_count == ESP32_I2S_DAC_AUDIO_BUFFER_BYTES) {
        s->audio_read =
            (s->audio_read + 1) % ESP32_I2S_DAC_AUDIO_BUFFER_BYTES;
        --s->audio_count;
    }
    s->audio_buffer[s->audio_write] = sample;
    s->audio_write =
        (s->audio_write + 1) % ESP32_I2S_DAC_AUDIO_BUFFER_BYTES;
    ++s->audio_count;
}

static void esp32_i2s_dac_audio_callback(void *opaque, int free)
{
    Esp32I2sDacState *s = opaque;

    while (free > 0 && s->audio_count) {
        const uint32_t contiguous = MIN(
            s->audio_count,
            ESP32_I2S_DAC_AUDIO_BUFFER_BYTES - s->audio_read);
        const uint32_t wanted = MIN(contiguous, (uint32_t)free);
        const size_t written = AUD_write(
            s->voice, &s->audio_buffer[s->audio_read], wanted);
        if (!written) {
            break;
        }
        s->audio_read =
            (s->audio_read + written) % ESP32_I2S_DAC_AUDIO_BUFFER_BYTES;
        s->audio_count -= written;
        free -= written;
    }
}

static void esp32_i2s_dac_schedule(Esp32I2sDacState *s,
                                    uint32_t sample_count)
{
    const uint64_t delay = MAX(
        1ULL,
        (uint64_t)sample_count * NANOSECONDS_PER_SECOND / s->sample_rate);
    timer_mod(s->dma_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
}

static void esp32_i2s_dac_dma_timer(void *opaque)
{
    Esp32I2sDacState *s = opaque;
    uint32_t descriptor[3] = {0};
    uint8_t data[DMA_DESCRIPTOR_MAX_BYTES];

    if (!s->link_running ||
        !(s->regs[I2S_CONF_REG / 4] & I2S_CONF_TX_START) ||
        !s->current_descriptor) {
        esp32_i2s_dac_stop(s);
        return;
    }

    cpu_physical_memory_read(
        s->current_descriptor, descriptor, sizeof(descriptor));
    uint32_t control = le32_to_cpu(descriptor[0]);
    const uint32_t buffer = le32_to_cpu(descriptor[1]);
    const uint32_t next = le32_to_cpu(descriptor[2]);
    const uint32_t capacity = control & DMA_DESCRIPTOR_SIZE_MASK;
    const uint32_t length =
        (control & DMA_DESCRIPTOR_LENGTH_MASK) >>
        DMA_DESCRIPTOR_LENGTH_SHIFT;

    if (!buffer || !length || length > capacity ||
        length > DMA_DESCRIPTOR_MAX_BYTES ||
        ((s->regs[I2S_LC_CONF_REG / 4] & I2S_LC_CHECK_OWNER) &&
         !(control & DMA_DESCRIPTOR_OWNER))) {
        s->regs[I2S_INT_RAW_REG / 4] |= I2S_INT_OUT_DSCR_ERR;
        esp32_i2s_dac_stop(s);
        esp32_i2s_dac_update_irq(s);
        return;
    }

    cpu_physical_memory_read(buffer, data, length);
    for (uint32_t offset = 1; offset < length; offset += 2) {
        esp32_i2s_dac_push_sample(s, data[offset]);
    }

    if (s->regs[I2S_LC_CONF_REG / 4] & I2S_LC_OUT_AUTO_WRBACK) {
        control &= ~DMA_DESCRIPTOR_OWNER;
        descriptor[0] = cpu_to_le32(control);
        cpu_physical_memory_write(
            s->current_descriptor, descriptor, sizeof(descriptor[0]));
    }

    s->regs[I2S_OUT_EOF_DES_ADDR_REG / 4] = s->current_descriptor;
    s->regs[I2S_INT_RAW_REG / 4] |= I2S_INT_OUT_EOF;
    if (next) {
        s->current_descriptor = next;
        esp32_i2s_dac_schedule(s, MAX(1U, length / 2U));
    } else {
        s->regs[I2S_INT_RAW_REG / 4] |= I2S_INT_OUT_TOTAL_EOF;
        s->regs[I2S_OUT_LINK_REG / 4] |= I2S_OUT_LINK_PARK;
        esp32_i2s_dac_stop(s);
    }
    esp32_i2s_dac_update_irq(s);
}

static uint64_t esp32_i2s_dac_read(void *opaque, hwaddr address,
                                   unsigned size)
{
    Esp32I2sDacState *s = opaque;

    if (address >= sizeof(s->regs)) {
        return 0;
    }
    if (address == I2S_INT_ST_REG) {
        return s->regs[I2S_INT_RAW_REG / 4] &
               s->regs[I2S_INT_ENA_REG / 4];
    }
    return s->regs[address / 4];
}

static void esp32_i2s_dac_write(void *opaque, hwaddr address,
                                uint64_t value, unsigned size)
{
    Esp32I2sDacState *s = opaque;
    const uint32_t val = value;

    if (address >= sizeof(s->regs)) {
        return;
    }
    switch (address) {
    case I2S_INT_RAW_REG:
    case I2S_INT_ST_REG:
    case I2S_OUT_EOF_DES_ADDR_REG:
        break;
    case I2S_INT_ENA_REG:
        s->regs[address / 4] = val;
        esp32_i2s_dac_update_irq(s);
        break;
    case I2S_INT_CLR_REG:
        s->regs[I2S_INT_RAW_REG / 4] &= ~val;
        esp32_i2s_dac_update_irq(s);
        break;
    case I2S_CONF_REG:
        s->regs[address / 4] = val;
        if ((val & I2S_CONF_TX_RESET) ||
            !(val & I2S_CONF_TX_START)) {
            esp32_i2s_dac_stop(s);
        }
        break;
    case I2S_LC_CONF_REG:
        s->regs[address / 4] = val;
        if (val & I2S_LC_OUT_RESET) {
            esp32_i2s_dac_stop(s);
        }
        break;
    case I2S_OUT_LINK_REG:
        s->regs[address / 4] = val & ~I2S_OUT_LINK_PARK;
        if (val & I2S_OUT_LINK_STOP) {
            esp32_i2s_dac_stop(s);
        }
        if (val & I2S_OUT_LINK_START) {
            s->current_descriptor =
                DMA_DESCRIPTOR_PREFIX | (val & I2S_OUT_LINK_ADDR_MASK);
            s->link_running = true;
            esp32_i2s_dac_set_active(s, true);
            timer_mod(
                s->dma_timer,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
        }
        break;
    default:
        s->regs[address / 4] = val;
        break;
    }
}

static const MemoryRegionOps esp32_i2s_dac_ops = {
    .read = esp32_i2s_dac_read,
    .write = esp32_i2s_dac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void esp32_i2s_dac_reset(DeviceState *dev)
{
    Esp32I2sDacState *s = ESP32_I2S_DAC(dev);

    esp32_i2s_dac_stop(s);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[I2S_OUT_LINK_REG / 4] = I2S_OUT_LINK_PARK;
    s->audio_read = 0;
    s->audio_write = 0;
    s->audio_count = 0;
    esp32_i2s_dac_update_irq(s);
}

static void esp32_i2s_dac_realize(DeviceState *dev, Error **errp)
{
    Esp32I2sDacState *s = ESP32_I2S_DAC(dev);
    const struct audsettings settings = {
        s->sample_rate, 1, AUDIO_FORMAT_U8, 0
    };

    if (!s->sample_rate || s->sample_rate > 96000) {
        error_setg(errp, "ESP32 DAC sample-rate must be 1..96000 Hz");
        return;
    }
    if (s->volume > 100) {
        error_setg(errp, "ESP32 DAC volume must be 0..100");
        return;
    }
    if (s->card.state) {
        if (!AUD_register_card("esp32-dac", &s->card, errp)) {
            return;
        }
        s->voice = AUD_open_out(
            &s->card, NULL, "esp32-dac-gpio26", s,
            esp32_i2s_dac_audio_callback,
            (struct audsettings *)&settings);
        if (!s->voice) {
            error_setg(errp, "Could not open ESP32 DAC audio output");
            AUD_remove_card(&s->card);
            return;
        }
        const uint8_t gain = (uint8_t)((s->volume * 255U + 50U) / 100U);
        AUD_set_volume_out(s->voice, s->volume == 0, gain, gain);
    }
}

static void esp32_i2s_dac_unrealize(DeviceState *dev)
{
    Esp32I2sDacState *s = ESP32_I2S_DAC(dev);

    timer_del(s->dma_timer);
    if (s->voice) {
        AUD_close_out(&s->card, s->voice);
        s->voice = NULL;
    }
    if (s->card.state) {
        AUD_remove_card(&s->card);
    }
}

static void esp32_i2s_dac_init(Object *obj)
{
    Esp32I2sDacState *s = ESP32_I2S_DAC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(
        &s->iomem, obj, &esp32_i2s_dac_ops, s, TYPE_ESP32_I2S_DAC, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->dma_timer = timer_new_ns(
        QEMU_CLOCK_VIRTUAL, esp32_i2s_dac_dma_timer, s);
}

static Property esp32_i2s_dac_properties[] = {
    DEFINE_AUDIO_PROPERTIES(Esp32I2sDacState, card),
    DEFINE_PROP_UINT32("sample-rate", Esp32I2sDacState, sample_rate, 8000),
    DEFINE_PROP_UINT32("volume", Esp32I2sDacState, volume, 70),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32_i2s_dac_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = esp32_i2s_dac_realize;
    dc->unrealize = esp32_i2s_dac_unrealize;
    device_class_set_legacy_reset(dc, esp32_i2s_dac_reset);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    device_class_set_props(dc, esp32_i2s_dac_properties);
}

static const TypeInfo esp32_i2s_dac_info = {
    .name = TYPE_ESP32_I2S_DAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32I2sDacState),
    .instance_init = esp32_i2s_dac_init,
    .class_init = esp32_i2s_dac_class_init,
};

static void esp32_i2s_dac_register_types(void)
{
    type_register_static(&esp32_i2s_dac_info);
}

type_init(esp32_i2s_dac_register_types)
