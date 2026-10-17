/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32C3_WIFI_TRACE_H
#define ESP32C3_WIFI_TRACE_H
#include "hw/net/esp32c3_wifi.h"
struct mac80211_frame;
void esp32_wifi_trace_packet(Esp32C3WifiState *s, struct mac80211_frame *frame,
                            const char *stage, uint32_t descriptor,
                            uint32_t next_descriptor);
#endif
