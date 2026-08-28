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
};
