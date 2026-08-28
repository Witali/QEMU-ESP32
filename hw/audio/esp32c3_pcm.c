/*
 * ESP32-C3 QEMU-only stereo PCM sink.
 *
 * The physical chip exposes I2S/PDM at this address.  The virtual device uses
 * a deliberately small register ABI so headless firmware profiles can send
 * decoded 48 kHz signed 16-bit stereo samples to QEMU's audio backends.
 */

#include "qemu/osdep.h"
#include "audio/audio.h"
#include "hw/audio/esp32c3_pcm.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/module.h"

#define PCM_VERSION_REG       0x00
#define PCM_CONTROL_REG       0x04
#define PCM_STATUS_REG        0x08
#define PCM_DATA_REG          0x0c
#define PCM_DROPPED_REG       0x10
#define PCM_MMIO_SIZE         0x100

#define PCM_VERSION           0x00010000U
#define PCM_CONTROL_ENABLE    0x00000001U
#define PCM_FRAME_BYTES       4U
#define PCM_SAMPLE_RATE       48000U

static void esp32c3_pcm_set_active(Esp32C3PcmState *s)
{
    if (s->voice) {
        AUD_set_active_out(s->voice, s->enabled);
    }
}

static void esp32c3_pcm_audio_callback(void *opaque, int free_bytes)
{
    Esp32C3PcmState *s = opaque;

    while (free_bytes >= PCM_FRAME_BYTES && s->frame_count) {
        uint32_t contiguous_frames = MIN(
            s->frame_count, ESP32C3_PCM_BUFFER_FRAMES - s->read_frame);
        uint32_t wanted_frames = MIN(
            contiguous_frames, (uint32_t)free_bytes / PCM_FRAME_BYTES);
        size_t written = AUD_write(
            s->voice, s->buffer + s->read_frame * PCM_FRAME_BYTES,
            wanted_frames * PCM_FRAME_BYTES);
        uint32_t written_frames = written / PCM_FRAME_BYTES;
        if (!written_frames) {
            break;
        }
        s->read_frame =
            (s->read_frame + written_frames) % ESP32C3_PCM_BUFFER_FRAMES;
        s->frame_count -= written_frames;
        free_bytes -= written_frames * PCM_FRAME_BYTES;
    }
}

static uint64_t esp32c3_pcm_read(void *opaque, hwaddr address, unsigned size)
{
    Esp32C3PcmState *s = ESP32C3_PCM(opaque);

    if (size != sizeof(uint32_t)) {
        return 0;
    }
    switch (address) {
    case PCM_VERSION_REG:
        return PCM_VERSION;
    case PCM_CONTROL_REG:
        return s->enabled ? PCM_CONTROL_ENABLE : 0;
    case PCM_STATUS_REG:
        return ESP32C3_PCM_BUFFER_FRAMES - s->frame_count;
    case PCM_DROPPED_REG:
        return s->dropped_frames;
    default:
        return 0;
    }
}

static void esp32c3_pcm_write(void *opaque, hwaddr address, uint64_t value,
                              unsigned size)
{
    Esp32C3PcmState *s = ESP32C3_PCM(opaque);

    if (size != sizeof(uint32_t)) {
        return;
    }
    switch (address) {
    case PCM_CONTROL_REG:
        s->enabled = (value & PCM_CONTROL_ENABLE) != 0;
        if (!s->enabled) {
            s->read_frame = 0;
            s->write_frame = 0;
            s->frame_count = 0;
        }
        esp32c3_pcm_set_active(s);
        break;
    case PCM_DATA_REG:
        if (!s->enabled || s->frame_count == ESP32C3_PCM_BUFFER_FRAMES) {
            ++s->dropped_frames;
            break;
        }
        stl_le_p(s->buffer + s->write_frame * PCM_FRAME_BYTES,
                 (uint32_t)value);
        s->write_frame =
            (s->write_frame + 1) % ESP32C3_PCM_BUFFER_FRAMES;
        ++s->frame_count;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps esp32c3_pcm_ops = {
    .read = esp32c3_pcm_read,
    .write = esp32c3_pcm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void esp32c3_pcm_reset(DeviceState *dev)
{
    Esp32C3PcmState *s = ESP32C3_PCM(dev);

    s->enabled = false;
    s->read_frame = 0;
    s->write_frame = 0;
    s->frame_count = 0;
    s->dropped_frames = 0;
    esp32c3_pcm_set_active(s);
}

static void esp32c3_pcm_realize(DeviceState *dev, Error **errp)
{
    Esp32C3PcmState *s = ESP32C3_PCM(dev);
    struct audsettings settings = {
        PCM_SAMPLE_RATE, 2, AUDIO_FORMAT_S16, 0
    };

    if (!AUD_register_card("esp32c3-pcm", &s->card, errp)) {
        return;
    }
    s->voice = AUD_open_out(&s->card, NULL, "esp32c3-pcm-stereo", s,
                            esp32c3_pcm_audio_callback, &settings);
    if (!s->voice) {
        error_setg(errp, "Could not open ESP32-C3 PCM audio output");
        AUD_remove_card(&s->card);
        return;
    }
}

static void esp32c3_pcm_unrealize(DeviceState *dev)
{
    Esp32C3PcmState *s = ESP32C3_PCM(dev);

    if (s->voice) {
        AUD_close_out(&s->card, s->voice);
        s->voice = NULL;
    }
    AUD_remove_card(&s->card);
}

static void esp32c3_pcm_init(Object *obj)
{
    Esp32C3PcmState *s = ESP32C3_PCM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32c3_pcm_ops, s,
                          TYPE_ESP32C3_PCM, PCM_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    s->buffer = g_malloc0(ESP32C3_PCM_BUFFER_FRAMES * PCM_FRAME_BYTES);
}

static void esp32c3_pcm_finalize(Object *obj)
{
    Esp32C3PcmState *s = ESP32C3_PCM(obj);

    g_free(s->buffer);
    s->buffer = NULL;
}

static Property esp32c3_pcm_properties[] = {
    DEFINE_AUDIO_PROPERTIES(Esp32C3PcmState, card),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32c3_pcm_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = esp32c3_pcm_realize;
    dc->unrealize = esp32c3_pcm_unrealize;
    device_class_set_legacy_reset(dc, esp32c3_pcm_reset);
    device_class_set_props(dc, esp32c3_pcm_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
}

static const TypeInfo esp32c3_pcm_info = {
    .name = TYPE_ESP32C3_PCM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32C3PcmState),
    .instance_init = esp32c3_pcm_init,
    .instance_finalize = esp32c3_pcm_finalize,
    .class_init = esp32c3_pcm_class_init,
};

static void esp32c3_pcm_register_types(void)
{
    type_register_static(&esp32c3_pcm_info);
}

type_init(esp32c3_pcm_register_types)
