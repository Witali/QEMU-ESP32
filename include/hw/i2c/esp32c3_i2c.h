/*
 * ESP32-C3 I2C controller.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I2C_ESP32C3_I2C_H
#define HW_I2C_ESP32C3_I2C_H

#include "hw/i2c/i2c.h"
#include "hw/registerfields.h"
#include "hw/sysbus.h"
#include "qemu/fifo8.h"

#define TYPE_ESP32C3_I2C "esp32c3.i2c"
OBJECT_DECLARE_SIMPLE_TYPE(Esp32C3I2CState, ESP32C3_I2C)

#define ESP32C3_I2C_MEM_SIZE 0x100
#define ESP32C3_I2C_FIFO_LENGTH 32
#define ESP32C3_I2C_CMD_COUNT 8

/*
 * ESP32-C5/C6/C61/H2 use the same FIFO depth and command opcodes. Keep this
 * device C3-specific because their complete register maps and SoC wiring are
 * separate; ESP32-C2 also differs by having a 16-byte FIFO.
 */

struct Esp32C3I2CState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    I2CBus *bus;
    Fifo8 rx_fifo;
    Fifo8 tx_fifo;
    bool trans_ongoing;
    uint32_t fifo_size;

    uint32_t ctr_reg;
    uint32_t timeout_reg;
    uint32_t int_ena_reg;
    uint32_t int_raw_reg;
    uint32_t sda_hold_reg;
    uint32_t sda_sample_reg;
    uint32_t high_period_reg;
    uint32_t low_period_reg;
    uint32_t start_hold_reg;
    uint32_t rstart_setup_reg;
    uint32_t stop_hold_reg;
    uint32_t stop_setup_reg;
    uint32_t cmd_reg[ESP32C3_I2C_CMD_COUNT];
};

REG32(ESP32C3_I2C_CTR, 0x04);
    FIELD(ESP32C3_I2C_CTR, MS_MODE, 4, 1);
    FIELD(ESP32C3_I2C_CTR, TRANS_START, 5, 1);

REG32(ESP32C3_I2C_STATUS, 0x08);
    FIELD(ESP32C3_I2C_STATUS, BUS_BUSY, 4, 1);
    FIELD(ESP32C3_I2C_STATUS, RXFIFO_CNT, 8, 6);
    FIELD(ESP32C3_I2C_STATUS, TXFIFO_CNT, 18, 6);

REG32(ESP32C3_I2C_TIMEOUT, 0x0c);

REG32(ESP32C3_I2C_FIFO_CONF, 0x18);
    FIELD(ESP32C3_I2C_FIFO_CONF, NONFIFO_EN, 10, 1);
    FIELD(ESP32C3_I2C_FIFO_CONF, RX_FIFO_RST, 12, 1);
    FIELD(ESP32C3_I2C_FIFO_CONF, TX_FIFO_RST, 13, 1);

REG32(ESP32C3_I2C_FIFO_DATA, 0x1c);

REG32(ESP32C3_I2C_INT_RAW, 0x20);
    FIELD(ESP32C3_I2C_INT_RAW, END_DETECT, 3, 1);
    FIELD(ESP32C3_I2C_INT_RAW, TRANS_COMPLETE, 7, 1);
    FIELD(ESP32C3_I2C_INT_RAW, ACK_ERR, 10, 1);

REG32(ESP32C3_I2C_INT_CLR, 0x24);
REG32(ESP32C3_I2C_INT_ENA, 0x28);
REG32(ESP32C3_I2C_INT_ST, 0x2c);

REG32(ESP32C3_I2C_SDA_HOLD, 0x30);
REG32(ESP32C3_I2C_SDA_SAMPLE, 0x34);
REG32(ESP32C3_I2C_HIGH_PERIOD, 0x38);
REG32(ESP32C3_I2C_LOW_PERIOD, 0x00);
REG32(ESP32C3_I2C_START_HOLD, 0x40);
REG32(ESP32C3_I2C_RSTART_SETUP, 0x44);
REG32(ESP32C3_I2C_STOP_HOLD, 0x48);
REG32(ESP32C3_I2C_STOP_SETUP, 0x4c);

REG32(ESP32C3_I2C_CMD, 0x58);
    FIELD(ESP32C3_I2C_CMD, BYTE_NUM, 0, 8);
    FIELD(ESP32C3_I2C_CMD, ACK_CHECK_EN, 8, 1);
    FIELD(ESP32C3_I2C_CMD, ACK_EXP, 9, 1);
    FIELD(ESP32C3_I2C_CMD, ACK_VAL, 10, 1);
    FIELD(ESP32C3_I2C_CMD, OPCODE, 11, 3);
    FIELD(ESP32C3_I2C_CMD, DONE, 31, 1);

typedef enum Esp32C3I2COpcode {
    ESP32C3_I2C_OPCODE_WRITE = 1,
    ESP32C3_I2C_OPCODE_STOP = 2,
    ESP32C3_I2C_OPCODE_READ = 3,
    ESP32C3_I2C_OPCODE_END = 4,
    ESP32C3_I2C_OPCODE_RSTART = 6,
} Esp32C3I2COpcode;

#endif
