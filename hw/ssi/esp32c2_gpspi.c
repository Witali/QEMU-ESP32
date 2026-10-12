/*
 * ESP32-C2 general-purpose SPI2 master: FIFO and GDMA data transactions.
 * Command/address/dummy phases and external MISO; timing is synchronous.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/ssi/esp32c2_gpspi.h"

enum {
    GPSPI_CMD = 0x00,
    GPSPI_USER = 0x10,
    GPSPI_MS_DLEN = 0x1c,
    GPSPI_DMA_CONF = 0x30,
    GPSPI_INT_ENA = 0x34,
    GPSPI_INT_CLR = 0x38,
    GPSPI_INT_RAW = 0x3c,
    GPSPI_INT_ST = 0x40,
    GPSPI_INT_SET = 0x44,
    GPSPI_W0 = 0x98,
};
#define GPSPI_REG(s, offset) ((s)->regs[(offset) / 4])
#define GPSPI_CMD_USR BIT(24)
#define GPSPI_TRANS_DONE BIT(12)

static void gpspi_update_irq(ESP32C2GpspiState *s)
{
    qemu_set_irq(s->irq, (GPSPI_REG(s, GPSPI_INT_RAW) &
                         GPSPI_REG(s, GPSPI_INT_ENA)) != 0);
}

static void gpspi_transaction(ESP32C2GpspiState *s)
{
    size_t length = ((GPSPI_REG(s, GPSPI_MS_DLEN) & 0x3ffff) + 8) / 8;
    g_autofree uint8_t *data = g_malloc(length);
    memset(data,0xff,length);
    bool dma_tx = GPSPI_REG(s, GPSPI_DMA_CONF) & BIT(28);
    bool dma_rx = GPSPI_REG(s, GPSPI_DMA_CONF) & BIT(27);
    bool write = GPSPI_REG(s, GPSPI_USER) & BIT(27);
    bool read = GPSPI_REG(s, GPSPI_USER) & BIT(28);

    if (write) {
        if (dma_tx) {
            if (!esp_gdma_read_channel(s->gdma, 0, data, length)) {
                qemu_log_mask(LOG_GUEST_ERROR, "esp32c2-spi2: invalid TX DMA descriptor\n");
                return;
            }
        } else {
            size_t start = (GPSPI_REG(s, GPSPI_USER) & BIT(25)) ? 32 : 0;
            memcpy(data, ((uint8_t *)&GPSPI_REG(s, GPSPI_W0)) + start,
                   MIN(length, 64 - start));
        }
    }
    uint32_t user = GPSPI_REG(s,GPSPI_USER), misc = GPSPI_REG(s,0x20);
    for (unsigned i=0;i<6;i++) qemu_set_irq(s->cs[i], !!(misc & BIT(i)));
    if (user & BIT(31)) {
        unsigned bits = (GPSPI_REG(s,0x18)>>28)+1;
        for (int shift = (bits-1)/8*8; shift>=0; shift-=8)
            ssi_transfer(s->bus,(GPSPI_REG(s,0x18)>>shift)&0xff);
    }
    if (user & BIT(30)) {
        unsigned bits = (GPSPI_REG(s,0x14)>>27)+1;
        for (unsigned i=0;i<(bits+7)/8;i++) ssi_transfer(s->bus,(GPSPI_REG(s,4)>>(24-i*8))&0xff);
    }
    if (user & BIT(29)) {
        unsigned cycles = (GPSPI_REG(s,0x14)&255)+1;
        for (unsigned i=0;i<(cycles+7)/8;i++) ssi_transfer(s->bus,0xff);
    }
    if (read && write && !(user & BIT(0))) {
        ssi_transfer_buf(s->bus,data,NULL,length);
        ssi_transfer_buf(s->bus,NULL,data,length);
    } else if (read || write) {
        ssi_transfer_buf(s->bus,data,read ? data : NULL,length);
    }
    if (read) {
        if (dma_rx) {
            esp_gdma_write_channel(s->gdma, 0, data, length);
        } else {
            size_t start = (GPSPI_REG(s,GPSPI_USER)&BIT(24)) ? 32 : 0;
            memcpy(((uint8_t *)&GPSPI_REG(s,GPSPI_W0))+start,data,MIN(length,64-start));
        }
    }
    if (!(GPSPI_REG(s,0x20)&BIT(30))) for (unsigned i=0;i<6;i++) qemu_set_irq(s->cs[i],1);
    GPSPI_REG(s, GPSPI_INT_RAW) |= GPSPI_TRANS_DONE;
    gpspi_update_irq(s);
}

static uint64_t gpspi_read(void *opaque, hwaddr address, unsigned size)
{
    ESP32C2GpspiState *s = opaque;
    switch (address) {
    case GPSPI_CMD:
        return 0; /* UPDATE and USR complete synchronously. */
    case GPSPI_INT_ST:
        return GPSPI_REG(s, GPSPI_INT_RAW) & GPSPI_REG(s, GPSPI_INT_ENA);
    case GPSPI_DMA_CONF:
        return GPSPI_REG(s, GPSPI_DMA_CONF) | 1; /* TX FIFO empty. */
    default:
        return s->regs[address / 4];
    }
}

static void gpspi_write(void *opaque, hwaddr address, uint64_t value, unsigned size)
{
    ESP32C2GpspiState *s = opaque;
    if (address == GPSPI_INT_CLR) {
        GPSPI_REG(s, GPSPI_INT_RAW) &= ~value;
    } else if (address == GPSPI_INT_SET) {
        GPSPI_REG(s, GPSPI_INT_RAW) |= value;
    } else {
        s->regs[address / 4] = value;
    }
    if (address == GPSPI_CMD && (value & GPSPI_CMD_USR)) {
        gpspi_transaction(s);
    }
    gpspi_update_irq(s);
}

static const MemoryRegionOps gpspi_ops = {
    .read = gpspi_read,
    .write = gpspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void gpspi_init(Object *object)
{
    ESP32C2GpspiState *s = ESP32C2_GPSPI(object);
    memory_region_init_io(&s->iomem, object, &gpspi_ops, s, TYPE_ESP32C2_GPSPI, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(object), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(object), &s->irq);
    qdev_init_gpio_out_named(DEVICE(object),s->cs,SSI_GPIO_CS,6);
    s->bus = ssi_create_bus(DEVICE(object), "spi");
    ssi_set_idle_value(s->bus,0xff);
}

static void gpspi_reset(DeviceState *device)
{
    ESP32C2GpspiState *s = ESP32C2_GPSPI(device);
    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
    GPSPI_REG(s,0x20) = 0x3f;
    for (unsigned i=0;i<6;i++) qemu_set_irq(s->cs[i],1);
}

static void gpspi_class_init(ObjectClass *klass, void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), gpspi_reset);
}

static const TypeInfo gpspi_info = {
    .name = TYPE_ESP32C2_GPSPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32C2GpspiState),
    .instance_init = gpspi_init,
    .class_init = gpspi_class_init,
};

static void gpspi_register(void)
{
    type_register_static(&gpspi_info);
}
type_init(gpspi_register)
