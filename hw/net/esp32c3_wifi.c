/*
 * ESP32-C3 Wi-Fi MAC register and DMA model
 *
 * The register subset is based on the independently reverse-engineered
 * ESP32-C3 Open MAC driver and the PICSimLab ESP32-C3 QEMU model.
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/net/esp32c3_wifi.h"
#include "hw/irq.h"
#include "exec/address-spaces.h"
#include "qemu/log.h"

static void esp32c3_wifi_update_irq(Esp32C3WifiState *s)
{
    qemu_set_irq(s->irq, s->interrupt_status != 0);
}

static int esp32c3_wifi_tx_slot(hwaddr addr)
{
    hwaddr distance;

    if (addr > ESP32C3_WIFI_TX_PLCP0_BASE) {
        return -1;
    }

    distance = ESP32C3_WIFI_TX_PLCP0_BASE - addr;
    if (distance % ESP32C3_WIFI_TX_SLOT_STRIDE) {
        return -1;
    }

    distance /= ESP32C3_WIFI_TX_SLOT_STRIDE;
    return distance < ESP32C3_WIFI_TX_SLOT_COUNT ? distance : -1;
}

static void esp32c3_wifi_complete_tx(Esp32C3WifiState *s, int slot,
                                     uint32_t value)
{
    Esp32C3WifiDmaDesc desc;
    hwaddr desc_addr;
    MemTxResult result;

    desc_addr = 0x3fc00000u | (value & ESP32C3_WIFI_DMA_ADDR_MASK);
    result = address_space_read(&address_space_memory, desc_addr,
                                MEMTXATTRS_UNSPECIFIED, &desc,
                                sizeof(desc));
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32c3-wifi: failed to read TX descriptor at "
                      "0x%08" HWADDR_PRIx "\n", desc_addr);
        s->tx_error |= 1u << slot;
    } else {
        s->tx_done |= 1u << slot;
    }

    s->interrupt_status |= ESP32C3_WIFI_INT_TX_DONE;
    esp32c3_wifi_update_irq(s);
}

static uint64_t esp32c3_wifi_read(void *opaque, hwaddr addr,
                                  unsigned size)
{
    Esp32C3WifiState *s = ESP32C3_WIFI(opaque);

    switch (addr) {
    case ESP32C3_WIFI_INT_STATUS:
        return s->interrupt_status;
    case ESP32C3_WIFI_TX_ERROR:
        return s->tx_error;
    case ESP32C3_WIFI_TX_DONE:
        return s->tx_done;
    case ESP32C3_WIFI_RX_NEXT_DESC:
        return s->rx_desc;
    case ESP32C3_WIFI_RX_LAST_DESC:
        return s->rx_desc;
    default:
        return s->regs[addr / sizeof(uint32_t)];
    }
}

static void esp32c3_wifi_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    Esp32C3WifiState *s = ESP32C3_WIFI(opaque);
    int slot;

    switch (addr) {
    case ESP32C3_WIFI_RX_BASE_DESC:
        s->rx_desc = value;
        break;
    case ESP32C3_WIFI_RX_CTRL:
        /* Hardware clears the RX descriptor update request immediately. */
        value &= ~1u;
        break;
    case ESP32C3_WIFI_INT_CLEAR:
        s->interrupt_status &= ~value;
        esp32c3_wifi_update_irq(s);
        break;
    case ESP32C3_WIFI_TX_ERROR_CLEAR:
        s->tx_error &= ~value;
        break;
    case ESP32C3_WIFI_TX_DONE_CLEAR:
        s->tx_done &= ~value;
        break;
    default:
        slot = esp32c3_wifi_tx_slot(addr);
        if (slot >= 0 && (value & ESP32C3_WIFI_TX_TRIGGER_MASK)) {
            esp32c3_wifi_complete_tx(s, slot, value);
        }
        break;
    }

    s->regs[addr / sizeof(uint32_t)] = value;
}

static const MemoryRegionOps esp32c3_wifi_ops = {
    .read = esp32c3_wifi_read,
    .write = esp32c3_wifi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void esp32c3_wifi_reset(DeviceState *dev)
{
    Esp32C3WifiState *s = ESP32C3_WIFI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->interrupt_status = 0;
    s->tx_done = 0;
    s->tx_error = 0;
    s->rx_desc = 0;
    esp32c3_wifi_update_irq(s);
}

static void esp32c3_wifi_init(Object *obj)
{
    Esp32C3WifiState *s = ESP32C3_WIFI(obj);

    memory_region_init_io(&s->iomem, obj, &esp32c3_wifi_ops, s,
                          TYPE_ESP32C3_WIFI, ESP32C3_WIFI_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void esp32c3_wifi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "ESP32-C3 Wi-Fi MAC";
    device_class_set_legacy_reset(dc, esp32c3_wifi_reset);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo esp32c3_wifi_info = {
    .name = TYPE_ESP32C3_WIFI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32C3WifiState),
    .instance_init = esp32c3_wifi_init,
    .class_init = esp32c3_wifi_class_init,
};

static void esp32c3_wifi_register_types(void)
{
    type_register_static(&esp32c3_wifi_info);
}

type_init(esp32c3_wifi_register_types)
