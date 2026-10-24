/* Bounded virtual DHCP client protocol; SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32C3_WIFI_DHCP_H
#define ESP32C3_WIFI_DHCP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Esp32WifiDhcpReply {
    uint8_t type, mac[6], xid[4], address[4], server[4];
} Esp32WifiDhcpReply;

enum Esp32WifiDhcpPhase {
    ESP32_DHCP_SELECTING,
    ESP32_DHCP_REQUESTING,
    ESP32_DHCP_BOUND,
};

typedef struct Esp32WifiDhcpLease {
    uint8_t phase, address[4], server[4];
} Esp32WifiDhcpLease;

enum Esp32WifiDhcpAction {
    ESP32_DHCP_IGNORE,
    ESP32_DHCP_SEND_REQUEST,
    ESP32_DHCP_LEASE_BOUND,
    ESP32_DHCP_RESTART,
};

extern const uint8_t esp32_wifi_dhcp_xid[4];
bool esp32_wifi_dhcp_reply(const uint8_t *llc, size_t size,
                          Esp32WifiDhcpReply *reply);
enum Esp32WifiDhcpAction esp32_wifi_dhcp_accept(Esp32WifiDhcpLease *lease,
    const Esp32WifiDhcpReply *reply, const uint8_t mac[6]);
#endif
