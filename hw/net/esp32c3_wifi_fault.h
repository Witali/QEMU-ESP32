/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32C3_WIFI_FAULT_H
#define ESP32C3_WIFI_FAULT_H
#include "hw/net/esp32c3_wifi.h"
/* One bit per stage and peer: probe, auth, assoc, discover, request, offer, ACK. */
#define ESP32_WIFI_FAULT_STAGES 7
#define ESP32_WIFI_FAULT_MASK ((1u << (ESP32_WIFI_SOFTAP_CLIENTS * ESP32_WIFI_FAULT_STAGES)) - 1)
bool esp32_wifi_fault_drop(Esp32C3WifiState *s, const struct mac80211_frame *frame,
                           bool to_guest);
#endif
