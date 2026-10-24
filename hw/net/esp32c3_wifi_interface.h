/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32C3_WIFI_INTERFACE_H
#define ESP32C3_WIFI_INTERFACE_H
#include "hw/net/esp32c3_wifi.h"
void esp32_wifi_reset_interfaces(Esp32C3WifiState *s);
bool esp32_wifi_select_guest_interface(Esp32C3WifiState *s, const struct mac80211_frame *frame);
void esp32_wifi_select_ethernet_interface(Esp32C3WifiState *s, const uint8_t *data, size_t size);
void esp32_wifi_learn_guest_ip(Esp32C3WifiState *s, const struct mac80211_frame *frame);
bool esp32_wifi_read_ssid(const struct mac80211_frame *frame, char *ssid, size_t capacity);
Esp32C3WifiInterface *esp32_wifi_softap_client(Esp32C3WifiState *s, unsigned index);
uint8_t esp32_wifi_interface_id(Esp32C3WifiState *s, Esp32C3WifiInterface *interface);
Esp32C3WifiInterface *esp32_wifi_interface_by_id(Esp32C3WifiState *s, uint8_t id);
bool esp32_wifi_client_online(Esp32C3WifiState *s, Esp32C3WifiInterface *interface);
Esp32C3WifiInterface *esp32_wifi_softap_relay(Esp32C3WifiState *s);
#endif
