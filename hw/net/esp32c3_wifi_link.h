/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32C3_WIFI_LINK_H
#define ESP32C3_WIFI_LINK_H
#include "hw/net/esp32c3_wifi.h"
bool esp32_wifi_packet_lost(Esp32C3WifiState *s);
int64_t esp32_wifi_delivery_time(Esp32C3WifiState *s, size_t size,
                                int64_t *serial_ready_ns);
int64_t esp32_wifi_next_delivery(Esp32C3WifiState *s, int64_t nominal_ns,
                                size_t size);
#endif
