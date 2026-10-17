/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Passive format conversion for Wi-Fi packet tracing; never retains payload. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "hw/net/esp32c3_wifi_80211.h"
#include "esp32c3_wifi_trace.h"
#include "trace.h"

/* Describe only TCP timing identifiers from a bounded 802.11/LLC prefix. */
static int tcp_description(const mac80211_frame *frame, char text[96])
{
    const uint8_t *raw = (const uint8_t *)frame;
    size_t offset = IEEE80211_HEADER_SIZE;
    size_t length = frame->frame_length;
    unsigned ip_length;

    text[0] = '-';
    text[1] = 0;
    if (frame->frame_control.type != IEEE80211_TYPE_DATA ||
        (frame->frame_control.flags & 3) == 3) {
        return -1;
    }
    if (frame->frame_control.sub_type & 8) {
        offset += 2; /* QoS control precedes LLC in QoS data frames. */
    }
    if (length < offset + 8 + 20 ||
        memcmp(raw + offset, "\xaa\xaa\x03\0\0\0\x08\0", 8)) {
        return -1;
    }
    offset += 8;
    ip_length = (raw[offset] & 15) * 4;
    if ((raw[offset] >> 4) != 4 || ip_length < 20 ||
        raw[offset + 9] != 6 || (lduw_be_p(raw + offset + 6) & 0x1fff) ||
        length < offset + ip_length + 20) {
        return -1;
    }
    offset += ip_length;
    snprintf(text, 96, "src=%u dst=%u seq=%" PRIu32 " flags=0x%02x kind=0x%02x",
             lduw_be_p(raw + offset), lduw_be_p(raw + offset + 2),
             ldl_be_p(raw + offset + 4), raw[offset + 13],
             (frame->frame_control.type << 4) | frame->frame_control.sub_type);
    return raw[offset + 13];
}

/* Convert packet identifiers only when this dedicated trace event is enabled. */
void esp32_wifi_trace_packet(Esp32C3WifiState *s, mac80211_frame *frame,
                            const char *stage, uint32_t descriptor,
                            uint32_t next_descriptor)
{
    char tcp[96];
    bool packets = trace_event_get_state_backends(TRACE_ESP32C3_WIFI_PACKET_STAGE);
    bool syns = trace_event_get_state_backends(TRACE_ESP32C3_WIFI_SYN_STAGE);
    int flags;

    if (!packets && !syns) {
        return;
    }
    flags = tcp_description(frame, tcp);
    if (packets) {
        trace_esp32c3_wifi_packet_stage(qemu_clock_get_us(QEMU_CLOCK_VIRTUAL),
            stage, descriptor, next_descriptor, frame->sequence_control.sequence_number,
            (frame->frame_control.type << 4) | frame->frame_control.sub_type,
            frame->frame_length, s->inject_queue_size, tcp, frame->signal_strength);
    }
    if (syns && flags >= 0 && (flags & 2)) {
        /* A sparse SYN trace reduces file I/O while observing the same path.
         * get_pc reports a CPU translation-block boundary, not an exact sample. */
        CPUState *cpu = qemu_get_cpu(0);
        uint64_t pc = cpu && CPU_GET_CLASS(cpu)->get_pc ?
                      CPU_GET_CLASS(cpu)->get_pc(cpu) : 0;
        trace_esp32c3_wifi_syn_stage(qemu_clock_get_us(QEMU_CLOCK_VIRTUAL),
            stage, descriptor, next_descriptor, frame->sequence_control.sequence_number,
            s->inject_queue_size, s->interrupt_status, pc,
            frame->signal_strength, tcp);
    }
}
