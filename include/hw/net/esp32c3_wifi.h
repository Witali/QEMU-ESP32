/*
 * ESP32-C3 Wi-Fi MAC register and DMA model
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */
#pragma once

#include "hw/sysbus.h"
#include "net/net.h"
#include "qemu/timer.h"
#include "hw/misc/esp32c3_radio.h"

struct mac80211_frame;
struct Esp32C3WifiEthernetPacket;

enum Esp32C3WifiMode {
    Esp32_Mode_Station,
    Esp32_Mode_SoftAP,
};

#define TYPE_ESP32C3_WIFI "esp32c3-wifi"
OBJECT_DECLARE_SIMPLE_TYPE(Esp32C3WifiState, ESP32C3_WIFI)

#define ESP32C3_WIFI_MMIO_SIZE 0x2000
#define ESP32C3_WIFI_TX_SLOT_COUNT 5

/* RX DMA and filter registers. */
#define ESP32C3_WIFI_RX_CTRL          0x084
#define ESP32C3_WIFI_RX_BASE_DESC     0x088
#define ESP32C3_WIFI_RX_NEXT_DESC     0x08c
#define ESP32C3_WIFI_RX_LAST_DESC     0x090

/* MAC interrupt and TX queue state registers. */
#define ESP32C3_WIFI_INT_STATUS       0xc3c
#define ESP32C3_WIFI_INT_CLEAR        0xc40
#define ESP32C3_WIFI_TX_ERROR_CLEAR   0xca4
#define ESP32C3_WIFI_TX_ERROR         0xca8
#define ESP32C3_WIFI_MAC_CTRL         0xca0
#define ESP32C3_WIFI_TX_DONE_CLEAR    0xcac
#define ESP32C3_WIFI_TX_DONE          0xcb0

/* PLCP0 contains a 20-bit DMA descriptor address and the TX trigger. */
#define ESP32C3_WIFI_TX_PLCP0_BASE    0xd08
#define ESP32C3_WIFI_TX_DMA_STATUS    0xd14
#define ESP32C3_WIFI_TX_SLOT_STRIDE   0x008
#define ESP32C3_WIFI_TX_TRIGGER_MASK  0xc0000000u
#define ESP32C3_WIFI_DMA_ADDR_MASK    0x000fffffu

/* Interrupt causes observed in the ESP32-C3 binary Wi-Fi driver. */
#define ESP32C3_WIFI_INT_TX_DONE      0x00000080u
#define ESP32C3_WIFI_INT_RX_DONE      0x01000024u

typedef struct Esp32C3WifiDmaDesc {
    uint32_t size : 12;
    uint32_t length : 12;
    uint32_t reserved : 6;
    uint32_t eof : 1;
    uint32_t owner : 1;
    uint32_t address;
    uint32_t next;
} QEMU_PACKED Esp32C3WifiDmaDesc;

struct Esp32C3WifiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[ESP32C3_WIFI_MMIO_SIZE / sizeof(uint32_t)];
    uint32_t interrupt_status;
    uint32_t tx_done;
    uint32_t tx_error;
    uint32_t rx_desc;
    uint32_t rx_last_desc;

    bool c2_layout;
    bool clock_services;
    bool weather_services;
    char *service_hosts;
    char **service_hostnames;
    char *ntp_hosts;
    char **ntp_hostnames;
    char *station_ssid;
    bool ntp_separate_servers;
    uint32_t ntp_drop_mask;
    int64_t ntp_unix_seconds;
    /* Last generated external-service response; retained across guest reset.
     * No packet contents or traffic-time formatting are retained. */
    int64_t ntp_reply_unix_seconds;
    int64_t ntp_reply_virtual_us;
    uint64_t ntp_reply_count;
    char *softap_client_ssid;
    NICConf conf;
    NICState *nic;
    Esp32C3RadioState *radio;

    uint32_t ap_state;
    int inject_queue_size;
    struct mac80211_frame *inject_queue;
    bool inject_timer_running;
    unsigned int inject_sequence_number;
    int beacon_ap;
    QEMUTimer *beacon_timer;
    QEMUTimer *inject_timer;
    QEMUTimer *wait_ack_timer;
    QEMUTimer *ethernet_tx_timer;

    struct Esp32C3WifiEthernetPacket *ethernet_tx_queue;
    struct Esp32C3WifiEthernetPacket *ethernet_tx_tail;
    uint32_t ethernet_tx_queue_size;
    int64_t rx_serial_ready_ns;
    int64_t tx_serial_ready_ns;

    uint64_t bandwidth_bps;
    uint32_t latency_ms;
    uint32_t jitter_ms;
    uint32_t loss_ppm;
    uint32_t queue_limit;
    bool link_down;
    bool station_ap_online;

    uint8_t ipaddr[4];
    uint8_t macaddr[6];
    uint8_t ap_ipaddr[4];
    uint8_t ap_macaddr[6];
    uint8_t associated_ap_macaddr[6];
    uint8_t softap_macaddr[6];
    uint8_t mode;
    uint8_t channel;
};

int esp32c3_wifi_channel(Esp32C3WifiState *s);
void esp32c3_wifi_set_packet_status(Esp32C3WifiState *s, uint32_t status);

void Esp32_WLAN_handle_frame(Esp32C3WifiState *s,
                             struct mac80211_frame *frame);
void Esp32_WLAN_setup_ap(DeviceState *dev, Esp32C3WifiState *s);
void Esp32_WLAN_reset_ap(Esp32C3WifiState *s);
void Esp32_sendFrame(Esp32C3WifiState *s, struct mac80211_frame *frame,
                     int length, int signal_strength);
void Esp32_WLAN_frame_delivered(Esp32C3WifiState *s);
void Esp32_WLAN_rx_ready(Esp32C3WifiState *s);
void Esp32_WLAN_set_station_ap_online(Esp32C3WifiState *s, bool online);

bool esp32c2_clock_service(Esp32C3WifiState *s, const uint8_t *data, size_t size);

size_t esp32c2_clock_prepare_dhcp(Esp32C3WifiState *s, const uint8_t *data,
                                size_t size, uint8_t *out, size_t capacity);
