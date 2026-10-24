/* Virtual client DHCP only; no guest network stack bypass.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "esp32c3_wifi_dhcp.h"
#include <string.h>
#include <assert.h>

/* A new acquisition has a new caller-owned transaction; retries retain it. */
void esp32_wifi_dhcp_begin(Esp32WifiDhcpLease *lease, const uint8_t xid[4])
{
    uint8_t id[4];
    memcpy(id, xid, 4);
    memset(lease, 0, sizeof(*lease));
    memcpy(lease->xid, id, 4);
}

/* RFC 2131 4.1: randomized exponential retry, 4/8/16/32/64 seconds.
 * The caller supplies uniform jitter in 0..2000 ms. The actual wait is capped
 * at 64 s, and never implemented as a guest delay or host busy wait.
 */
uint32_t esp32_wifi_dhcp_backoff_ms(uint32_t *nominal_seconds, uint32_t jitter_ms)
{
    assert(jitter_ms <= 2000);
    uint32_t previous = *nominal_seconds;
    *nominal_seconds = previous < 4 ? 4 : previous >= 32 ? 64 : previous * 2;
    uint32_t delay = *nominal_seconds * 1000 + jitter_ms - 1000;
    return delay > 64000 ? 64000 : delay;
}

/* Read unaligned network fields without host-endian structure casts. */
static unsigned read_be16(const uint8_t *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

typedef struct DhcpOptions {
    uint8_t type, overload, server[4];
    bool has_server;
} DhcpOptions;

/* Parse each bounded option region, including PAD, END and option overload. */
static bool read_options(const uint8_t *p, size_t size, bool main,
                         DhcpOptions *options)
{
    while (size) {
        uint8_t tag = *p++;
        size--;
        if (tag == 255) {
            return true;
        }
        if (!tag) {
            continue;
        }
        if (!size) {
            return false;
        }
        size_t len = *p++;
        if (len > --size) {
            return false;
        }
        switch (tag) {
        case 53:
            if (len != 1 || options->type || !p[0]) {
                return false;
            }
            options->type = p[0];
            break;
        case 54:
            if (len != 4 || options->has_server) {
                return false;
            }
            options->has_server = true;
            memcpy(options->server, p, 4);
            break;
        case 52:
            if (!main || len != 1 || options->overload || !p[0] || p[0] > 3) {
                return false;
            }
            options->overload = p[0];
            break;
        }
        p += len;
        size -= len;
    }
    return false;
}

/* Validate encapsulation and lengths before touching BOOTP or DHCP options.
 * A transaction is identified by both xid and chaddr later, never option order.
 * IPv4 fragments require reassembly and are deliberately ignored by this peer.
 */
bool esp32_wifi_dhcp_reply(const uint8_t *llc, size_t size,
                          Esp32WifiDhcpReply *reply)
{
    static const uint8_t snap[] = { 0xaa, 0xaa, 3, 0, 0, 0, 8, 0 };
    static const uint8_t cookie[] = { 99, 130, 83, 99 };
    Esp32WifiDhcpReply result;
    DhcpOptions options = { 0 };
    if (size < 8 + 20 || memcmp(llc, snap, sizeof(snap))) {
        return false;
    }
    const uint8_t *ip = llc + 8;
    unsigned ihl = (ip[0] & 15) * 4;
    unsigned total = read_be16(ip + 2);
    if (ip[0] >> 4 != 4 || ihl < 20 || total < ihl + 8 + 240 ||
        total > size - 8 || ip[9] != 17 || (read_be16(ip + 6) & 0x3fff)) {
        return false;
    }
    const uint8_t *udp = ip + ihl;
    unsigned length = read_be16(udp + 4);
    if (read_be16(udp) != 67 || read_be16(udp + 2) != 68 ||
        length < 8 + 240 || length > total - ihl) {
        return false;
    }
    const uint8_t *bootp = udp + 8;
    if (bootp[0] != 2 || bootp[1] != 1 || bootp[2] != 6 ||
        memcmp(bootp + 236, cookie, sizeof(cookie)) ||
        !read_options(bootp + 240, length - 8 - 240, true, &options)) {
        return false;
    }
    if ((options.overload & 1) && !read_options(bootp + 108, 128, false, &options)) {
        return false;
    }
    if ((options.overload & 2) && !read_options(bootp + 44, 64, false, &options)) {
        return false;
    }
    if (!options.has_server ||
        (options.type != 2 && options.type != 5 && options.type != 6)) {
        return false;
    }
    result.type = options.type;
    memcpy(result.mac, bootp + 28, 6);
    memcpy(result.xid, bootp + 4, 4);
    memcpy(result.address, bootp + 16, 4);
    memcpy(result.server, options.server, 4);
    *reply = result;
    return true;
}

/* Do not expose a usable lease until the selected server confirms our request.
 * Keep unrelated peers, transactions and late/duplicate offers out of state.
 * This models initial acquisition; lease renewal/rebinding remain unsupported.
 */
enum Esp32WifiDhcpAction esp32_wifi_dhcp_accept(Esp32WifiDhcpLease *lease,
    const Esp32WifiDhcpReply *reply, const uint8_t mac[6])
{
    static const uint8_t zero[4];
    if (memcmp(reply->mac, mac, 6) || memcmp(reply->xid, lease->xid, 4) ||
        !memcmp(reply->server, zero, 4)) {
        return ESP32_DHCP_IGNORE;
    }
    if (lease->phase == ESP32_DHCP_SELECTING && reply->type == 2 &&
        memcmp(reply->address, zero, 4)) {
        memcpy(lease->address, reply->address, 4);
        memcpy(lease->server, reply->server, 4);
        lease->phase = ESP32_DHCP_REQUESTING;
        return ESP32_DHCP_SEND_REQUEST;
    }
    if (lease->phase != ESP32_DHCP_REQUESTING ||
        memcmp(reply->server, lease->server, 4)) {
        return ESP32_DHCP_IGNORE;
    }
    if (reply->type == 5 && !memcmp(reply->address, lease->address, 4)) {
        lease->phase = ESP32_DHCP_BOUND;
        return ESP32_DHCP_LEASE_BOUND;
    }
    if (reply->type == 6) {
        memset(lease, 0, sizeof(*lease));
        return ESP32_DHCP_RESTART;
    }
    return ESP32_DHCP_IGNORE;
}
