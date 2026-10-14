/* Local DNS/NTP services for the isolated ESP32-C2 clock network.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "hw/net/esp32c3_wifi.h"
#include "hw/net/esp32c3_wifi_frame.h"

static uint16_t ip_checksum(const uint8_t *p, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i += 2) sum += lduw_be_p(p + i);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return ~sum;
}

/* Advertise the local synthetic DNS in opt-in clock-service DHCP replies.
 * Restricted SLIRP omits DNS because its normal host resolver is disabled;
 * the guest must learn our isolated replacement through its real DHCP client. */
size_t esp32c2_clock_prepare_dhcp(Esp32C3WifiState *s, const uint8_t *data,
                                size_t size, uint8_t *out, size_t capacity)
{
    size_t ihl, total, udp_size, pos, end;
    const uint8_t *ip, *udp;
    if (!s->clock_services || s->mode != Esp32_Mode_Station || size < 282 ||
        lduw_be_p(data + 12) != 0x0800) return 0;
    ip = data + 14;
    ihl = (ip[0] & 15) * 4;
    total = lduw_be_p(ip + 2);
    if (ip[0] >> 4 != 4 || ip[9] != 17 || ihl < 20 ||
        total < ihl + 248 || total > size - 14 ||
        (lduw_be_p(ip + 6) & 0x3fff)) return 0;
    udp = ip + ihl;
    udp_size = lduw_be_p(udp + 4);
    if (lduw_be_p(udp) != 67 || lduw_be_p(udp + 2) != 68 ||
        udp_size != total - ihl || udp[8] != 2 ||
        ldl_be_p(udp + 244) != 0x63825363) return 0;
    pos = 14 + ihl + 248;
    end = 14 + total;
    while (pos < end) {
        uint8_t code = data[pos];
        if (code == 255) break;
        if (!code) { pos++; continue; }
        if (pos + 2 > end || data[pos + 1] > end - pos - 2) return 0;
        if (code == 6) return 0; /* Keep an explicitly supplied DNS option. */
        pos += 2 + data[pos + 1];
    }
    if (pos == end || size + 6 > capacity) return 0;
    memcpy(out, data, pos);
    out[pos] = 6; out[pos + 1] = 4;
    memcpy(out + pos + 2, ip + 12, 3); out[pos + 5] = 3;
    memcpy(out + pos + 6, data + pos, size - pos);
    stw_be_p(out + 14 + 2, total + 6);
    stw_be_p(out + 14 + 10, 0);
    stw_be_p(out + 14 + 10, ip_checksum(out + 14, ihl));
    stw_be_p(out + 14 + ihl + 4, udp_size + 6);
    stw_be_p(out + 14 + ihl + 6, 0); /* Valid IPv4 UDP checksum omission. */
    return size + 6;
}

/* Return true only for a handled DNS/NTP request. Ordinary packets still
 * pass through the existing virtual AP and SLIRP backend. */
bool esp32c2_clock_service(Esp32C3WifiState *s, const uint8_t *data, size_t size)
{
    uint8_t reply[1518] = { 0 }, payload[512] = { 0 };
    const uint8_t *ip, *udp, *request;
    size_t ihl, request_len, payload_len;
    uint16_t port;
    struct mac80211_frame *frame;

    /* Do not impersonate a client of a guest AP: its real DHCP/ARP stack owns
     * that subnet. Station service addresses follow the configured SLIRP LAN. */
    if (!s->clock_services || s->mode != Esp32_Mode_Station) return false;

    if (size >= 42 && lduw_be_p(data + 12) == 0x0806 &&
        lduw_be_p(data + 14) == 1 && lduw_be_p(data + 16) == 0x0800 &&
        data[18] == 6 && data[19] == 4 && lduw_be_p(data + 20) == 1 &&
        !memcmp(data + 38, data + 28, 3) &&
        (data[41] == 2 || data[41] == 3 ||
         (s->ntp_separate_servers && (data[41] == 4 || data[41] == 5)))) {
        memcpy(reply, data + 6, 6);
        reply[6] = 0x52; reply[7] = 0x55; memcpy(reply + 8, data + 38, 4);
        memcpy(reply + 12, data + 12, 10); stw_be_p(reply + 20, 2);
        memcpy(reply + 22, reply + 6, 6); memcpy(reply + 28, data + 38, 4);
        memcpy(reply + 32, data + 22, 10);
        frame = Esp32_WLAN_create_data_packet(s, reply, 42);
        if (frame) { Esp32_WLAN_init_ap_frame(s, frame); Esp32_WLAN_insert_frame(s, frame); }
        return true;
    }
    if (!s->clock_services || size < 14 + 20 + 8 ||
        lduw_be_p(data + 12) != 0x0800) return false;
    ip = data + 14;
    ihl = (ip[0] & 15) * 4;
    if (ip[0] >> 4 != 4 || ihl < 20 || size < 14 + ihl + 8 ||
        ip[9] != 17 || (lduw_be_p(ip + 6) & 0x3fff)) return false;
    udp = ip + ihl;
    request_len = lduw_be_p(udp + 4);
    if (request_len < 8 || request_len > size - 14 - ihl) return false;
    request_len -= 8;
    request = udp + 8;
    port = lduw_be_p(udp + 2);
    if (port == 123) {
        struct timeval now;
        uint32_t seconds, fraction;
        int64_t unix_seconds;
        unsigned server = 1;
        if (request_len < 48 || (request[0] & 7) != 3) return true;
        if (s->ntp_separate_servers) {
            if (memcmp(ip + 16, ip + 12, 3)) return false;
            switch (ip[19]) {
            case 2: server = 1; break;
            case 4: server = 2; break;
            case 5: server = 3; break;
            default: return false;
            }
        }
        fprintf(stderr, "C2 NTP request server=%u virtual_us=%" PRId64 "\n",
                server, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000);
        if (s->ntp_drop_mask & (1u << (server - 1))) {
            fprintf(stderr, "C2 NTP drop server=%u\n", server);
            return true;
        }
        if (s->ntp_unix_seconds >= 0) {
            unix_seconds = s->ntp_unix_seconds;
            fraction = 0;
        } else {
            gettimeofday(&now, NULL);
            unix_seconds = now.tv_sec;
            fraction = ((uint64_t)now.tv_usec << 32) / 1000000;
        }
        /* Keep 64-bit UNIX time outside Windows timeval.tv_sec (32-bit long).
         * NTP wire seconds intentionally wrap at the 2036 era boundary. */
        seconds = (uint64_t)unix_seconds + 2208988800ULL;
        payload[0] = (request[0] & 0x38) | 4; /* server, synchronized */
        payload[1] = 2; /* host clock, not a claimed primary reference */
        payload[2] = request[2];
        payload[3] = (uint8_t)-20;
        stl_be_p(payload + 8, 655); /* 10 ms root dispersion */
        memcpy(payload + 12, "QEMU", 4);
        memcpy(payload + 24, request + 40, 8); /* originate timestamp */
        for (unsigned offset = 16; offset <= 32; offset += 16) {
            stl_be_p(payload + offset, seconds);
            stl_be_p(payload + offset + 4, fraction);
        }
        stl_be_p(payload + 40, seconds);
        stl_be_p(payload + 44, fraction);
        payload_len = 48;
        fprintf(stderr, "C2 NTP reply unix_seconds=%" PRId64 " virtual_us=%" PRId64 "\n",
                unix_seconds, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000);
        fprintf(stderr, "C2 NTP response server=%u\n", server);
    } else if (port == 53) {
        char name[254];
        size_t pos = 12, used = 0;
        if (request_len < 17 || (request[2] & 0x80) ||
            lduw_be_p(request + 4) != 1) return true;
        while (pos < request_len && request[pos]) {
            size_t count = request[pos++];
            if (count > 63 || pos + count >= request_len || used + count + 1 >= sizeof(name)) return true;
            if (used) name[used++] = '.';
            memcpy(name + used, request + pos, count);
            used += count; pos += count;
        }
        name[used] = 0;
        if (pos + 5 > request_len || pos + 5 > sizeof(payload) - 16) return true;
        pos++;
        /* Opt-in DNS only: SLIRP guestfwd carries real guest TCP/HTTP to
         * a loopback fixture. The usual isolated clock profile is unchanged. */
        int service = -1, ntp = -1;
        for (size_t i = 0; s->service_hostnames && s->service_hostnames[i]; i++) {
            if (!g_ascii_strcasecmp(name, s->service_hostnames[i])) {
                service = i;
                break;
            }
        }
        for (size_t i = 0; s->ntp_hostnames && s->ntp_hostnames[i]; i++) {
            if (!g_ascii_strcasecmp(name, s->ntp_hostnames[i])) {
                ntp = i;
                break;
            }
        }
        bool known = service >= 0 || ntp >= 0;
        bool answer = known && lduw_be_p(request + pos) == 1 && lduw_be_p(request + pos + 2) == 1;
        payload_len = pos + 4;
        memcpy(payload, request, payload_len);
        stw_be_p(payload + 2, known ? 0x8180 : 0x8183);
        stw_be_p(payload + 6, answer ? 1 : 0);
        stw_be_p(payload + 8, 0); stw_be_p(payload + 10, 0);
        if (answer) {
            /* Log only the known service class, never a requested hostname or key. */
            /* Retain the old diagnostic only for callers using its legacy flag. */
            fprintf(stderr, "C2 DNS answer service=%s\n", service >= 0 ?
                    (s->weather_services ? "weather" : "http") : "ntp");
            uint8_t *a = payload + payload_len;
            stw_be_p(a, 0xc00c); stw_be_p(a + 2, 1); stw_be_p(a + 4, 1);
            stl_be_p(a + 6, 60); stw_be_p(a + 10, 4);
            memcpy(a + 12, ip + 12, 3);
            a[15] = service >= 0 ? 100 + service : 2;
            if (service < 0 && s->ntp_separate_servers) {
                a[15] = ntp == 1 ? 4 : ntp == 2 ? 5 : 2;
            }
            payload_len += 16;
        }
    } else return false;

    memcpy(reply, data + 6, 6); memcpy(reply + 6, data, 6);
    stw_be_p(reply + 12, 0x0800);
    uint8_t *out_ip = reply + 14, *out_udp = out_ip + 20;
    out_ip[0] = 0x45; out_ip[8] = 64; out_ip[9] = 17;
    stw_be_p(out_ip + 2, 20 + 8 + payload_len);
    memcpy(out_ip + 12, ip + 16, 4); memcpy(out_ip + 16, ip + 12, 4);
    stw_be_p(out_ip + 10, ip_checksum(out_ip, 20));
    memcpy(out_udp, udp + 2, 2); memcpy(out_udp + 2, udp, 2);
    stw_be_p(out_udp + 4, 8 + payload_len); /* IPv4 UDP checksum may be zero */
    memcpy(out_udp + 8, payload, payload_len);
    frame = Esp32_WLAN_create_data_packet(s, reply, 14 + 20 + 8 + payload_len);
    if (frame) {
        Esp32_WLAN_init_ap_frame(s, frame);
        Esp32_WLAN_insert_frame(s, frame);
    }
    return true;
}
