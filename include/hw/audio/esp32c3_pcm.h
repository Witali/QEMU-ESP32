#pragma once

#include "audio/audio.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_ESP32C3_PCM "esp32c3.pcm"
OBJECT_DECLARE_SIMPLE_TYPE(Esp32C3PcmState, ESP32C3_PCM)

#define ESP32C3_PCM_BUFFER_FRAMES 32768

struct Esp32C3PcmState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QEMUSoundCard card;
    SWVoiceOut *voice;
    uint8_t *buffer;
    uint32_t read_frame;
    uint32_t write_frame;
    uint32_t frame_count;
    uint32_t dropped_frames;
    bool enabled;
};
