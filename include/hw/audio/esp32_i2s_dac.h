#pragma once

#include "audio/audio.h"
#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_ESP32_I2S_DAC "esp32.i2s-dac"
OBJECT_DECLARE_SIMPLE_TYPE(Esp32I2sDacState, ESP32_I2S_DAC)

#define ESP32_I2S_DAC_AUDIO_BUFFER_BYTES 65536

struct Esp32I2sDacState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUSoundCard card;
    SWVoiceOut *voice;
    QEMUTimer *dma_timer;

    uint32_t regs[0x100 / sizeof(uint32_t)];
    uint32_t current_descriptor;
    uint32_t sample_rate;
    uint32_t volume;
    bool link_running;

    uint8_t audio_buffer[ESP32_I2S_DAC_AUDIO_BUFFER_BYTES];
    uint32_t audio_read;
    uint32_t audio_write;
    uint32_t audio_count;
};
