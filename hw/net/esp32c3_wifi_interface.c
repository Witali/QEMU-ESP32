/* APSTA peer selection; radio, queues and DMA remain shared.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "esp32c3_wifi_interface.h"
#include "hw/net/esp32c3_wifi_80211.h"

/* Resolve a bounded virtual-client slot without changing the active radio peer. */
Esp32C3WifiInterface *esp32_wifi_softap_client(Esp32C3WifiState *s, unsigned index)
{
    assert(index < ESP32_WIFI_SOFTAP_CLIENTS);
    return index ? &s->softap_extra[index - 1] : &s->softap;
}

/* Queue identities retain the exact peer through later APSTA transmissions. */
uint8_t esp32_wifi_interface_id(Esp32C3WifiState *s, Esp32C3WifiInterface *interface)
{
    if (interface == &s->station) return 0;
    for (unsigned i = 0; i < ESP32_WIFI_SOFTAP_CLIENTS; i++) {
        if (interface == esp32_wifi_softap_client(s, i)) return i + 1;
    }
    g_assert_not_reached();
}

/* Restore the queued peer, never whichever interface happened to transmit last. */
Esp32C3WifiInterface *esp32_wifi_interface_by_id(Esp32C3WifiState *s, uint8_t id)
{
    return id ? esp32_wifi_softap_client(s, id - 1) : &s->station;
}

/* Only opt-in virtual AP clients obey the external presence control. */
bool esp32_wifi_client_online(Esp32C3WifiState *s, Esp32C3WifiInterface *interface)
{
    if (!s->softap_client_ssid || interface == &s->station) return true;
    return (s->softap_client_mask & (1u << (esp32_wifi_interface_id(s, interface) - 1))) != 0;
}

/* Clear associations on hardware reset; external client presence survives it. */
void esp32_wifi_reset_interfaces(Esp32C3WifiState *s)
{
    memset(&s->station, 0, sizeof(s->station));
    s->station.mode = Esp32_Mode_Station;
    memcpy(s->station.ap_macaddr, (uint8_t[]){0x10, 0x13, 0x46, 0xbf, 0x31, 0x50}, 6);
    for (unsigned i = 0; i < ESP32_WIFI_SOFTAP_CLIENTS; i++) {
        Esp32C3WifiInterface *peer = esp32_wifi_softap_client(s, i);
        memset(peer, 0, sizeof(*peer));
        peer->mode = Esp32_Mode_SoftAP;
        memcpy(peer->ap_macaddr, (uint8_t[]){0x52, 0x54, 0, 0x43, 0, 2}, 6);
        peer->ap_macaddr[5] += i;
    }
    s->softap_host_peer = 0;
    memset(s->softap_host_ip, 0, sizeof(s->softap_host_ip));
    s->wlan = &s->station;
}

/* Read the bounded SSID IE of a beacon or probe response, excluding its FCS. */
bool esp32_wifi_read_ssid(const struct mac80211_frame *frame, char *ssid, size_t capacity)
{
    size_t end, pos = 12;
    if (frame->frame_length < IEEE80211_HEADER_SIZE + pos + 4) return false;
    end = frame->frame_length - IEEE80211_HEADER_SIZE - 4;
    while (pos + 2 <= end) {
        uint8_t tag = frame->data_and_fcs[pos++];
        size_t len = frame->data_and_fcs[pos++];
        if (len > end - pos) return false;
        if (tag == IEEE80211_BEACON_PARAM_SSID) {
            if (len > 32 || len >= capacity) return false;
            memcpy(ssid, frame->data_and_fcs + pos, len);
            ssid[len] = 0;
            return true;
        }
        pos += len;
    }
    return false;
}

/* Route by transmitting MAC and 802.11 direction, never last-seen AP state. */
bool esp32_wifi_select_guest_interface(Esp32C3WifiState *s, const struct mac80211_frame *frame)
{
    bool softap = false;
    if (s->softap_client_ssid) {
        if (frame->frame_control.type == IEEE80211_TYPE_MGT &&
            frame->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_BEACON) {
            char ssid[33];
            if (!esp32_wifi_read_ssid(frame, ssid, sizeof(ssid)) ||
                strcmp(ssid, s->softap_client_ssid)) {
                return false;
            }
            softap = true;
        } else if (frame->frame_control.type == IEEE80211_TYPE_DATA) {
            softap = (frame->frame_control.flags & 3) == 2;
        } else {
            softap = !memcmp(frame->source_address, s->softap.macaddr, 6) ||
                     !memcmp(frame->destination_address, s->softap.ap_macaddr, 6);
        }
    }
    s->wlan = softap ? &s->softap : &s->station;
    if (softap) {
        const uint8_t *client = frame->destination_address;
        /* Only a fully bounded DHCP reply may select a broadcast client. */
        Esp32WifiDhcpReply reply;
        if (frame->frame_control.type == IEEE80211_TYPE_DATA &&
            frame->frame_length >= IEEE80211_HEADER_SIZE + 8 + 4 &&
            esp32_wifi_dhcp_reply(frame->data_and_fcs,
                frame->frame_length - IEEE80211_HEADER_SIZE - 4, &reply)) {
            client = reply.mac;
        }
        if (!memcmp(client, (uint8_t[]){255, 255, 255, 255, 255, 255}, 6)) {
            Esp32C3WifiInterface *relay = esp32_wifi_softap_relay(s);
            if (relay) s->wlan = relay;
        }
        for (unsigned i = 0; i < ESP32_WIFI_SOFTAP_CLIENTS; i++) {
            Esp32C3WifiInterface *peer = esp32_wifi_softap_client(s, i);
            if (!memcmp(client, peer->ap_macaddr, 6)) {
                s->wlan = peer;
                break;
            }
        }
    }
    return esp32_wifi_client_online(s, s->wlan);
}

/* Learn an interface's source IPv4 address from its actual data/ARP packets. */
void esp32_wifi_learn_guest_ip(Esp32C3WifiState *s, const struct mac80211_frame *frame)
{
    const uint8_t *data = frame->data_and_fcs, *ip = NULL;
    size_t size;
    if (frame->frame_control.type != IEEE80211_TYPE_DATA ||
        frame->frame_length < IEEE80211_HEADER_SIZE + 8 + 4) return;
    size = frame->frame_length - IEEE80211_HEADER_SIZE - 4;
    if (lduw_be_p(data + 6) == 0x0800 && size >= 28 && data[8] >> 4 == 4) {
        ip = data + 20;
    } else if (lduw_be_p(data + 6) == 0x0806 && size >= 36 &&
               lduw_be_p(data + 8) == 1 && lduw_be_p(data + 10) == 0x0800 &&
               data[12] == 6 && data[13] == 4) {
        ip = data + 22;
    }
    if (ip && ldl_be_p(ip) && ip[0] < 224) memcpy(s->wlan->ipaddr, ip, 4);
}

/* The shared Ethernet bridge uses one present, DHCP-bound AP peer.
 * Selecting by association state avoids routing through a departed primary.
 * Separate per-client backends/host endpoints remain a different capability.
 */
Esp32C3WifiInterface *esp32_wifi_softap_relay(Esp32C3WifiState *s)
{
    for (unsigned i = 0; i < ESP32_WIFI_SOFTAP_CLIENTS; i++) {
        Esp32C3WifiInterface *peer = esp32_wifi_softap_client(s, i);
        if (esp32_wifi_client_online(s, peer) &&
            peer->ap_state == Esp32_WLAN__STATE_STA_ASSOCIATED) {
            return peer;
        }
    }
    return NULL;
}

/* Demultiplex SLIRP replies by guest MAC or target IP, including broadcast ARP.
 * DHCP broadcast replies belong to STA; the AP DHCP server lives in the guest.
 */
void esp32_wifi_select_ethernet_interface(Esp32C3WifiState *s, const uint8_t *data, size_t size)
{
    const uint8_t *target = NULL;
    bool softap = false;
    if (s->softap_client_ssid && size >= 14) {
        if (lduw_be_p(data + 12) == 0x0800 && size >= 34) target = data + 30;
        else if (lduw_be_p(data + 12) == 0x0806 && size >= 42) target = data + 38;
        softap = !memcmp(data, s->softap.macaddr, 6) ||
                 (target && ldl_be_p(s->softap.ipaddr) && !memcmp(target, s->softap.ipaddr, 4));
    }
    Esp32C3WifiInterface *relay = softap ? esp32_wifi_softap_relay(s) : NULL;
    s->wlan = softap ? (relay ? relay : &s->softap) : &s->station;
}
