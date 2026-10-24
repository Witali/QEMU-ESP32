/* Exact one-shot packet loss at the opt-in virtual AP client's boundary.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "esp32c3_wifi_fault.h"
#include "esp32c3_wifi_interface.h"
#include "hw/net/esp32c3_wifi_80211.h"

/* Classify model-owned requests, not arbitrary network input. The constructor
 * always emits SNAP/fixed IPv4/UDP/BOOTP with option 53 first. Replies use the
 * normal bounded DHCP parser, including variable IHL and option ordering.
 */
static int packet_stage(const struct mac80211_frame *frame, bool to_guest)
{
    if (frame->frame_control.type == IEEE80211_TYPE_MGT && to_guest) {
        switch (frame->frame_control.sub_type) {
        case IEEE80211_TYPE_MGT_SUBTYPE_PROBE_REQ: return 0;
        case IEEE80211_TYPE_MGT_SUBTYPE_AUTHENTICATION:
            if (frame->frame_length >= IEEE80211_HEADER_SIZE + 6 + 4 &&
                frame->data_and_fcs[2] == 1 && frame->data_and_fcs[3] == 0) return 1;
            break;
        case IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_REQ: return 2;
        }
    }
    if (frame->frame_control.type != IEEE80211_TYPE_DATA ||
        frame->frame_length < IEEE80211_HEADER_SIZE + 8 + 4) return -1;
    size_t size = frame->frame_length - IEEE80211_HEADER_SIZE - 4;
    if (to_guest) {
        size_t at = 8 + sizeof(dhcp_request_t);
        if (size >= at + 3 && frame->data_and_fcs[at] == 53 &&
            frame->data_and_fcs[at + 1] == 1) {
            if (frame->data_and_fcs[at + 2] == 1) return 3;
            if (frame->data_and_fcs[at + 2] == 3) return 4;
        }
    } else {
        Esp32WifiDhcpReply reply;
        if (esp32_wifi_dhcp_reply(frame->data_and_fcs, size, &reply)) {
            if (reply.type == 2) return 5;
            if (reply.type == 5) return 6;
        }
    }
    return -1;
}

/* Consume exactly one armed bit. No guest event is fabricated, no payload is
 * retained and the default zero mask exits before packet parsing or clock reads.
 */
bool esp32_wifi_fault_drop(Esp32C3WifiState *s, const struct mac80211_frame *frame,
                           bool to_guest)
{
    if (!s->softap_drop_mask || !s->softap_client_ssid || s->wlan == &s->station) return false;
    int stage = packet_stage(frame, to_guest);
    if (stage < 0) return false;
    unsigned peer = esp32_wifi_interface_id(s, s->wlan) - 1;
    uint32_t bit = 1u << (peer * ESP32_WIFI_FAULT_STAGES + stage);
    if (!(s->softap_drop_mask & bit)) return false;
    s->softap_drop_mask &= ~bit;
    s->softap_dropped[peer][stage]++;
    s->softap_drop_last_ns[peer][stage] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return true;
}
