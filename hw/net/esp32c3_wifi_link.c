/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Link timing and loss are separate from 802.11 conversion and DMA queues. */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/guest-random.h"
#include "esp32c3_wifi_link.h"

/* Convert the configured finite wire rate to a complete frame duration. */
static int64_t esp32_wifi_transfer_time_ns(Esp32C3WifiState *s, size_t size)
{
    uint64_t bits_ns = (uint64_t)size * 8 * NANOSECONDS_PER_SECOND;
    return s->bandwidth_bps ? bits_ns / s->bandwidth_bps +
           (bits_ns % s->bandwidth_bps != 0) : 0;
}

/* Apply the requested physical link state and random-loss setting once. */
bool esp32_wifi_packet_lost(Esp32C3WifiState *s)
{
    uint32_t sample;

    if (s->link_down) {
        return true;
    }
    if (!s->loss_ppm) {
        return false;
    }
    qemu_guest_getrandom_nofail(&sample, sizeof(sample));
    return sample % 1000000 < s->loss_ppm;
}

/* Compute a nominal arrival with the same latency and jitter controls. */
int64_t esp32_wifi_delivery_time(Esp32C3WifiState *s, size_t size,
                                        int64_t *serial_ready_ns)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t serialization_ns = esp32_wifi_transfer_time_ns(s, size);
    int64_t jitter_ns = 0;
    uint32_t sample;

    *serial_ready_ns = MAX(now, *serial_ready_ns) + serialization_ns;

    if (s->jitter_ms) {
        uint64_t range = (uint64_t)s->jitter_ms * 2 + 1;

        qemu_guest_getrandom_nofail(&sample, sizeof(sample));
        jitter_ns = ((int64_t)(sample % range) - s->jitter_ms) *
                    SCALE_MS;
    }
    return MAX(now, *serial_ready_ns + (int64_t)s->latency_ms * SCALE_MS +
                    jitter_ns);
}

/* Preserve finite transfer time when a FIFO head is already overdue.
 * Otherwise max(now, nominal) completes a whole backlog at the same virtual
 * instant, even while the guest has not had time to service the first frame.
 * Zero bandwidth retains the explicit unlimited-throughput mode. */
int64_t esp32_wifi_next_delivery(Esp32C3WifiState *s, int64_t nominal_ns,
                                size_t size)
{
    int64_t earliest = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                       esp32_wifi_transfer_time_ns(s, size);
    return MAX(earliest, nominal_ns);
}
