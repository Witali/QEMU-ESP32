/*
 * ESP32 SPI controller
 *
 * Copyright (c) 2019 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "sysemu/sysemu.h"
#include "sysemu/dma.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "hw/ssi/esp32_spi.h"
#include "hw/misc/esp32_flash_enc.h"



enum {
    CMD_RES = 0xab,
    CMD_DP = 0xb9,
    CMD_CE = 0x60,
    CMD_BE = 0xD8,
    CMD_SE = 0x20,
    CMD_PP = 0x02,
    CMD_WRSR = 0x1,
    CMD_RDSR = 0x5,
    CMD_RDID = 0x9f,
    CMD_WRDI = 0x4,
    CMD_WREN = 0x6,
    CMD_READ = 0x03,
};


#define ESP32_SPI_REG_SIZE    0x1000
#define ESP32_SPI_DMA_CONF    0x100
#define ESP32_SPI_DMA_OUT_LINK 0x104
#define ESP32_SPI_DMA_IN_LINK 0x108
#define ESP32_SPI_DMA_LINK_ADDR_MASK 0x000fffff
#define ESP32_SPI_DMA_LINK_START BIT(29)
#define ESP32_SPI_DMA_DRAM_BASE 0x3ff00000
#define ESP32_SPI_DMA_MAX_DESCRIPTORS 128

static void esp32_spi_do_command(Esp32SpiState* state, uint32_t cmd_reg);

static void esp32_spi_update_irq(Esp32SpiState *s)
{
    bool level =
        FIELD_EX32(s->slave_reg, SPI_SLAVE, TRANS_DONE) &&
        FIELD_EX32(s->slave_reg, SPI_SLAVE, TRANS_INTEN);

    qemu_set_irq(s->irq, level);
}

static void esp32_spi_complete(void *opaque)
{
    Esp32SpiState *s = ESP32_SPI(opaque);

    s->slave_reg =
        FIELD_DP32(s->slave_reg, SPI_SLAVE, TRANS_DONE, 1);
    esp32_spi_update_irq(s);
}

static uint64_t esp32_spi_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32SpiState *s = ESP32_SPI(opaque);
    uint64_t r = 0;
    switch (addr) {
    case A_SPI_ADDR:
        r = s->addr_reg;
        break;
    case A_SPI_CTRL:
        r = s->ctrl_reg;
        break;
    case A_SPI_STATUS:
        r = s->status_reg;
        break;
    case A_SPI_CTRL1:
        r = s->ctrl1_reg;
        break;
    case A_SPI_CTRL2:
        r = s->ctrl2_reg;
        break;
    case A_SPI_USER:
        r = s->user_reg;
        break;
    case A_SPI_USER1:
        r = s->user1_reg;
        break;
    case A_SPI_USER2:
        r = s->user2_reg;
        break;
    case A_SPI_MOSI_DLEN:
        r = s->mosi_dlen_reg;
        break;
    case A_SPI_MISO_DLEN:
        r = s->miso_dlen_reg;
        break;
    case A_SPI_PIN:
        r = s->pin_reg;
        break;
    case A_SPI_W0 ... A_SPI_W0 + (ESP32_SPI_BUF_WORDS - 1) * sizeof(uint32_t):
        r = s->data_reg[(addr - A_SPI_W0) / sizeof(uint32_t)];
        break;
    case A_SPI_EXT2:
        r = 0;
        break;
    case ESP32_SPI_DMA_CONF:
        r = s->dma_conf_reg;
        break;
    case ESP32_SPI_DMA_OUT_LINK:
        r = s->dma_out_link_reg;
        break;
    case ESP32_SPI_DMA_IN_LINK:
        r = s->dma_in_link_reg;
        break;
    case A_SPI_SLAVE:
        r = s->slave_reg;
        break;
    }
    return r;
}

static void esp32_spi_write(void *opaque, hwaddr addr,
                       uint64_t value, unsigned int size)
{
    Esp32SpiState *s = ESP32_SPI(opaque);
    switch (addr) {
    case A_SPI_W0 ... A_SPI_W0 + (ESP32_SPI_BUF_WORDS - 1) * sizeof(uint32_t):
        s->data_reg[(addr - A_SPI_W0) / sizeof(uint32_t)] = value;
        break;
    case A_SPI_ADDR:
        s->addr_reg = value;
        break;
    case A_SPI_CTRL:
        s->ctrl_reg = value;
        break;
    case A_SPI_STATUS:
        s->status_reg = value;
        break;
    case A_SPI_CTRL1:
        s->ctrl1_reg = value;
        break;
    case A_SPI_CTRL2:
        s->ctrl2_reg = value;
        break;
    case A_SPI_USER:
        s->user_reg = value;
        break;
    case A_SPI_USER1:
        s->user1_reg = value;
        break;
    case A_SPI_USER2:
        s->user2_reg = value;
        break;
    case A_SPI_MOSI_DLEN:
        s->mosi_dlen_reg = value;
        break;
    case A_SPI_MISO_DLEN:
        s->miso_dlen_reg = value;
        break;
    case A_SPI_PIN:
        s->pin_reg = value;
        break;
    case A_SPI_SLAVE:
        s->slave_reg = value;
        esp32_spi_update_irq(s);
        break;
    case ESP32_SPI_DMA_CONF:
        s->dma_conf_reg = value;
        break;
    case ESP32_SPI_DMA_OUT_LINK:
        s->dma_out_link_reg = value;
        break;
    case ESP32_SPI_DMA_IN_LINK:
        s->dma_in_link_reg = value;
        break;
    case A_SPI_CMD:
        esp32_spi_do_command(s, value);
        break;
    }
}

typedef struct Esp32SpiTransaction {
    int cmd_bytes;
    uint32_t cmd;
    int addr_bytes;
    uint32_t addr;
    int data_tx_bytes;
    int data_rx_bytes;
    uint32_t* data;
} Esp32SpiTransaction;

static void esp32_spi_txrx_buffer(Esp32SpiState *s,
                                  const void *tx_buf, int tx_bytes,
                                  void *rx_buf, int rx_bytes)
{
    const uint8_t *tx = tx_buf;
    uint8_t *rx = rx_buf;
    int common_bytes = MIN(tx_bytes, rx_bytes);

    if (common_bytes) {
        ssi_transfer_buf(s->spi, tx, rx, common_bytes);
    }
    if (tx_bytes > common_bytes) {
        ssi_transfer_buf(s->spi, tx + common_bytes, NULL,
                         tx_bytes - common_bytes);
    } else if (rx_bytes > common_bytes) {
        ssi_transfer_buf(s->spi, NULL, rx + common_bytes,
                         rx_bytes - common_bytes);
    }
}

static size_t esp32_spi_dma_read(uint32_t link_reg,
                                 uint8_t *buffer, size_t bytes)
{
    hwaddr descriptor =
        ESP32_SPI_DMA_DRAM_BASE |
        (link_reg & ESP32_SPI_DMA_LINK_ADDR_MASK);
    size_t copied = 0;

    for (int i = 0;
         i < ESP32_SPI_DMA_MAX_DESCRIPTORS && copied < bytes;
         i++) {
        uint32_t raw[3];
        uint32_t flags;
        uint32_t length;
        uint32_t data;
        uint32_t next;
        size_t chunk;

        cpu_physical_memory_read(descriptor, raw, sizeof(raw));
        flags = le32_to_cpu(raw[0]);
        length = extract32(flags, 12, 12);
        data = le32_to_cpu(raw[1]);
        next = le32_to_cpu(raw[2]);
        if (length == 0 || data == 0) {
            break;
        }
        chunk = MIN((size_t)length, bytes - copied);
        cpu_physical_memory_read(data, buffer + copied, chunk);
        copied += chunk;
        if (extract32(flags, 30, 1) || next == 0) {
            break;
        }
        descriptor = next;
    }
    return copied;
}

static size_t esp32_spi_dma_write(uint32_t link_reg,
                                  const uint8_t *buffer, size_t bytes)
{
    hwaddr descriptor =
        ESP32_SPI_DMA_DRAM_BASE |
        (link_reg & ESP32_SPI_DMA_LINK_ADDR_MASK);
    size_t copied = 0;

    for (int i = 0;
         i < ESP32_SPI_DMA_MAX_DESCRIPTORS && copied < bytes;
         i++) {
        uint32_t raw[3];
        uint32_t flags;
        uint32_t capacity;
        uint32_t data;
        uint32_t next;
        size_t chunk;

        cpu_physical_memory_read(descriptor, raw, sizeof(raw));
        flags = le32_to_cpu(raw[0]);
        capacity = extract32(flags, 0, 12);
        data = le32_to_cpu(raw[1]);
        next = le32_to_cpu(raw[2]);
        if (capacity == 0 || data == 0) {
            break;
        }
        chunk = MIN((size_t)capacity, bytes - copied);
        cpu_physical_memory_write(data, buffer + copied, chunk);
        flags = deposit32(flags, 12, 12, chunk);
        flags = deposit32(flags, 31, 1, 0);
        raw[0] = cpu_to_le32(flags);
        cpu_physical_memory_write(descriptor, raw, sizeof(raw[0]));
        copied += chunk;
        if (extract32(flags, 30, 1) || next == 0) {
            break;
        }
        descriptor = next;
    }
    return copied;
}

static void esp32_spi_cs_set(Esp32SpiState *s, int value)
{
    for (int i = 0; i < ESP32_SPI_CS_COUNT; ++i) {
        qemu_set_irq(s->cs_gpio[i], ((s->pin_reg & (1 << i)) == 0) ? value : 1);
    }
}

static void esp32_spi_transaction(Esp32SpiState *s, Esp32SpiTransaction *t)
{
    uint8_t *dma_tx = NULL;
    uint8_t *dma_rx = NULL;
    const uint8_t *tx = (uint8_t *)t->data;
    uint8_t *rx = (uint8_t *)t->data;

    if (t->data_tx_bytes &&
        (s->dma_out_link_reg & ESP32_SPI_DMA_LINK_START)) {
        dma_tx = g_malloc(t->data_tx_bytes);
        if (esp32_spi_dma_read(s->dma_out_link_reg, dma_tx,
                               t->data_tx_bytes) != t->data_tx_bytes) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "esp32_spi: short TX DMA descriptor chain\n");
        }
        tx = dma_tx;
        s->dma_out_link_reg &= ~ESP32_SPI_DMA_LINK_START;
    }
    if (t->data_rx_bytes &&
        (s->dma_in_link_reg & ESP32_SPI_DMA_LINK_START)) {
        dma_rx = g_malloc0(t->data_rx_bytes);
        rx = dma_rx;
        s->dma_in_link_reg &= ~ESP32_SPI_DMA_LINK_START;
    }

    esp32_spi_cs_set(s, 0);
    esp32_spi_txrx_buffer(s, &t->cmd, t->cmd_bytes, NULL, 0);
    esp32_spi_txrx_buffer(s, &t->addr, t->addr_bytes, NULL, 0);
    esp32_spi_txrx_buffer(s, tx, t->data_tx_bytes,
                          rx, t->data_rx_bytes);
    esp32_spi_cs_set(s, 1);

    if (dma_rx) {
        if (esp32_spi_dma_write(s->dma_in_link_reg, dma_rx,
                                t->data_rx_bytes) != t->data_rx_bytes) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "esp32_spi: short RX DMA descriptor chain\n");
        }
    }
    g_free(dma_tx);
    g_free(dma_rx);
}

/* Convert one of the hardware "bitlen" registers to a byte count */
static inline int bitlen_to_bytes(uint32_t val)
{
    return (val + 1 + 7) / 8; /* bitlen registers hold number of bits, minus one */
}

static void maybe_encrypt_data(Esp32SpiState *s)
{
    Esp32FlashEncryptionState* flash_enc = esp32_flash_encryption_find();
    if (esp32_flash_encryption_enabled(flash_enc)) {
        esp32_flash_encryption_get_result(flash_enc, &s->data_reg[0], 8);
    }
}

static void esp32_spi_do_command(Esp32SpiState* s, uint32_t cmd_reg)
{
    Esp32SpiTransaction t = {
        .cmd_bytes = 1
    };
    switch (cmd_reg) {
    case R_SPI_CMD_READ_MASK:
        t.cmd = CMD_READ;
        t.addr_bytes = bitlen_to_bytes(FIELD_EX32(s->user1_reg, SPI_USER1, ADDR_BITLEN));
        t.addr = bswap32(s->addr_reg) >> (32 - t.addr_bytes * 8);
        t.data = &s->data_reg[0];
        t.data_rx_bytes = bitlen_to_bytes(s->miso_dlen_reg);
        break;

    case R_SPI_CMD_WREN_MASK:
        t.cmd = CMD_WREN;
        break;

    case R_SPI_CMD_WRDI_MASK:
        t.cmd = CMD_WRDI;
        break;

    case R_SPI_CMD_RDID_MASK:
        t.cmd = CMD_RDID;
        t.data = &s->data_reg[0];
        t.data_rx_bytes = 3;
        break;

    case R_SPI_CMD_RDSR_MASK:
        t.cmd = CMD_RDSR;
        t.data = &s->status_reg;
        t.data_rx_bytes = 1;
        break;

    case R_SPI_CMD_WRSR_MASK:
        t.cmd = CMD_WRSR;
        t.data = &s->status_reg;
        t.data_tx_bytes = 1;
        break;

    case R_SPI_CMD_PP_MASK:
        maybe_encrypt_data(s);
        t.cmd = CMD_PP;
        t.data = &s->data_reg[0];
        t.addr_bytes = bitlen_to_bytes(FIELD_EX32(s->user1_reg, SPI_USER1, ADDR_BITLEN));
        t.addr = bswap32(s->addr_reg) >> 8;
        t.data = &s->data_reg[0];
        t.data_tx_bytes = s->addr_reg >> 24;
        break;

    case R_SPI_CMD_SE_MASK:
        t.cmd = CMD_SE;
        t.addr_bytes = bitlen_to_bytes(FIELD_EX32(s->user1_reg, SPI_USER1, ADDR_BITLEN));
        t.addr = bswap32(s->addr_reg) >> (32 - t.addr_bytes * 8);
        break;

    case R_SPI_CMD_BE_MASK:
        t.cmd = CMD_BE;
        t.addr_bytes = bitlen_to_bytes(FIELD_EX32(s->user1_reg, SPI_USER1, ADDR_BITLEN));
        t.addr = bswap32(s->addr_reg) >> (32 - t.addr_bytes * 8);
        break;

    case R_SPI_CMD_CE_MASK:
        t.cmd = CMD_CE;
        break;

    case R_SPI_CMD_DP_MASK:
        t.cmd = CMD_DP;
        break;

    case R_SPI_CMD_RES_MASK:
        t.cmd = CMD_RES;
        t.data = &s->data_reg[0];
        t.data_rx_bytes = 3;
        break;

    case R_SPI_CMD_USR_MASK:
        maybe_encrypt_data(s);
        if (FIELD_EX32(s->user_reg, SPI_USER, COMMAND)) {
            t.cmd = FIELD_EX32(s->user2_reg, SPI_USER2, COMMAND_VALUE);
            t.cmd_bytes = bitlen_to_bytes(FIELD_EX32(s->user2_reg, SPI_USER2, COMMAND_BITLEN));
        } else {
            t.cmd_bytes = 0;
        }
        if (FIELD_EX32(s->user_reg, SPI_USER, ADDR)) {
            t.addr_bytes = bitlen_to_bytes(FIELD_EX32(s->user1_reg, SPI_USER1, ADDR_BITLEN));
            t.addr = bswap32(s->addr_reg);
        }
        if (FIELD_EX32(s->user_reg, SPI_USER, MOSI)) {
            t.data = &s->data_reg[0];
            t.data_tx_bytes = bitlen_to_bytes(s->mosi_dlen_reg);
        }
        if (FIELD_EX32(s->user_reg, SPI_USER, MISO)) {
            t.data = &s->data_reg[0];
            t.data_rx_bytes = bitlen_to_bytes(s->miso_dlen_reg);
        }
        break;
    default:
        return;
    }
    /*
     * Real ESP32 hardware clears TRANS_DONE while a transaction is in
     * progress.  Lower the interrupt before completing the synchronous QEMU
     * transaction so back-to-back queued transfers produce a new IRQ edge.
     */
    s->slave_reg =
        FIELD_DP32(s->slave_reg, SPI_SLAVE, TRANS_DONE, 0);
    esp32_spi_update_irq(s);
    esp32_spi_transaction(s, &t);
    /*
     * The transfer above is synchronous.  Complete it before returning from
     * the MMIO write: ESP-IDF polling transfers immediately spin on
     * SPI_SLAVE.TRANS_DONE, so deferring this to a bottom half deadlocks the
     * virtual CPU and prevents that bottom half from ever running.
     */
    esp32_spi_complete(s);
}


static const MemoryRegionOps esp32_spi_ops = {
    .read =  esp32_spi_read,
    .write = esp32_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32_spi_reset_hold(Object *obj, ResetType type)
{
    Esp32SpiState *s = ESP32_SPI(obj);
    s->pin_reg = 0x6;
    s->user1_reg = FIELD_DP32(0, SPI_USER1, ADDR_BITLEN, 23);
    s->user1_reg = FIELD_DP32(s->user1_reg, SPI_USER1, DUMMY_CYCLELEN, 7);
    s->user2_reg = FIELD_DP32(0, SPI_USER2, COMMAND_BITLEN, 4);
    s->user2_reg = FIELD_DP32(s->user2_reg, SPI_USER2, COMMAND_VALUE, 0);
    s->status_reg = 0;
    s->slave_reg =
        FIELD_DP32(0, SPI_SLAVE, TRANS_DONE, 1);
    s->dma_conf_reg = 0;
    s->dma_out_link_reg = 0;
    s->dma_in_link_reg = 0;
    esp32_spi_update_irq(s);
}

static void esp32_spi_realize(DeviceState *dev, Error **errp)
{
}

static void esp32_spi_init(Object *obj)
{
    Esp32SpiState *s = ESP32_SPI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32_spi_ops, s,
                          TYPE_ESP32_SPI, ESP32_SPI_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->spi = ssi_create_bus(DEVICE(s), "spi");
    qdev_init_gpio_out_named(DEVICE(s), &s->cs_gpio[0], SSI_GPIO_CS, ESP32_SPI_CS_COUNT);
}

static Property esp32_spi_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32_spi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32_spi_reset_hold;
    dc->realize = esp32_spi_realize;
    device_class_set_props(dc, esp32_spi_properties);
}

static const TypeInfo esp32_spi_info = {
    .name = TYPE_ESP32_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32SpiState),
    .instance_init = esp32_spi_init,
    .class_init = esp32_spi_class_init
};

static void esp32_spi_register_types(void)
{
    type_register_static(&esp32_spi_info);
}

type_init(esp32_spi_register_types)
