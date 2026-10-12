/*
 * ESP32-C3 I2C controller.
 *
 * This is intentionally independent of the Xtensa ESP32 I2C model. The C3
 * has eight command registers and uses different command opcode values.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/esp32c3_i2c.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"

static void esp32c3_i2c_update_irq(Esp32C3I2CState *s)
{
    qemu_set_irq(s->irq, !!(s->int_raw_reg & s->int_ena_reg));
}

static void esp32c3_i2c_finish_transfer(Esp32C3I2CState *s)
{
    if (s->trans_ongoing) {
        i2c_end_transfer(s->bus);
        s->trans_ongoing = false;
    }
}

static bool esp32c3_i2c_pop_tx(Esp32C3I2CState *s, uint8_t *value)
{
    if (!fifo8_num_used(&s->tx_fifo)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32c3_i2c: TX FIFO underflow\n");
        return false;
    }
    *value = fifo8_pop(&s->tx_fifo);
    return true;
}

static void esp32c3_i2c_set_ack_error(Esp32C3I2CState *s, uint32_t command)
{
    if (FIELD_EX32(command, ESP32C3_I2C_CMD, ACK_CHECK_EN) &&
        !FIELD_EX32(command, ESP32C3_I2C_CMD, ACK_EXP)) {
        s->int_raw_reg = FIELD_DP32(s->int_raw_reg,
                                    ESP32C3_I2C_INT_RAW, ACK_ERR, 1);
    }
}

static bool esp32c3_i2c_execute_write(Esp32C3I2CState *s, uint32_t command)
{
    size_t length = FIELD_EX32(command, ESP32C3_I2C_CMD, BYTE_NUM);
    uint8_t value;

    if (!s->trans_ongoing) {
        if (!length || !esp32c3_i2c_pop_tx(s, &value)) {
            esp32c3_i2c_set_ack_error(s, command);
            return false;
        }

        if (i2c_start_transfer(s->bus, value >> 1, value & 1)) {
            esp32c3_i2c_set_ack_error(s, command);
            return false;
        }
        s->trans_ongoing = true;
        s->int_raw_reg = FIELD_DP32(s->int_raw_reg,
                                    ESP32C3_I2C_INT_RAW, ACK_ERR, 0);
        length--;
    }

    while (length--) {
        if (!esp32c3_i2c_pop_tx(s, &value)) {
            return false;
        }
        if (i2c_send(s->bus, value)) {
            esp32c3_i2c_set_ack_error(s, command);
            return false;
        }
    }
    return true;
}

static void esp32c3_i2c_execute_read(Esp32C3I2CState *s, uint32_t command)
{
    size_t length = FIELD_EX32(command, ESP32C3_I2C_CMD, BYTE_NUM);

    while (length--) {
        if (!fifo8_num_free(&s->rx_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "esp32c3_i2c: RX FIFO overflow\n");
            break;
        }
        fifo8_push(&s->rx_fifo, i2c_recv(s->bus));
    }
}

static void esp32c3_i2c_do_transaction(Esp32C3I2CState *s)
{
    bool finished = false;

    for (int index = 0; index < ESP32C3_I2C_CMD_COUNT && !finished; index++) {
        uint32_t command = s->cmd_reg[index];
        unsigned opcode = FIELD_EX32(command, ESP32C3_I2C_CMD, OPCODE);

        switch (opcode) {
        case ESP32C3_I2C_OPCODE_RSTART:
            esp32c3_i2c_finish_transfer(s);
            break;
        case ESP32C3_I2C_OPCODE_WRITE:
            if (!esp32c3_i2c_execute_write(s, command)) {
                esp32c3_i2c_finish_transfer(s);
                finished = true;
            }
            break;
        case ESP32C3_I2C_OPCODE_READ:
            esp32c3_i2c_execute_read(s, command);
            break;
        case ESP32C3_I2C_OPCODE_STOP:
            esp32c3_i2c_finish_transfer(s);
            s->int_raw_reg = FIELD_DP32(s->int_raw_reg,
                                        ESP32C3_I2C_INT_RAW,
                                        TRANS_COMPLETE, 1);
            finished = true;
            break;
        case ESP32C3_I2C_OPCODE_END:
            s->int_raw_reg = FIELD_DP32(s->int_raw_reg,
                                        ESP32C3_I2C_INT_RAW, END_DETECT, 1);
            finished = true;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "esp32c3_i2c: invalid command %d opcode %u\n",
                          index, opcode);
            finished = true;
            break;
        }

        s->cmd_reg[index] = FIELD_DP32(s->cmd_reg[index],
                                       ESP32C3_I2C_CMD, DONE, 1);
    }
    esp32c3_i2c_update_irq(s);
}

static uint32_t esp32c3_i2c_status(Esp32C3I2CState *s)
{
    uint32_t value = 0;

    value = FIELD_DP32(value, ESP32C3_I2C_STATUS, BUS_BUSY,
                       s->trans_ongoing);
    value = FIELD_DP32(value, ESP32C3_I2C_STATUS, RXFIFO_CNT,
                       fifo8_num_used(&s->rx_fifo));
    value = FIELD_DP32(value, ESP32C3_I2C_STATUS, TXFIFO_CNT,
                       fifo8_num_used(&s->tx_fifo));
    return value;
}

static uint64_t esp32c3_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    Esp32C3I2CState *s = ESP32C3_I2C(opaque);

    switch (addr) {
    case A_ESP32C3_I2C_CTR:
        return s->ctr_reg;
    case A_ESP32C3_I2C_STATUS:
        return esp32c3_i2c_status(s);
    case A_ESP32C3_I2C_FIFO_DATA:
        if (!fifo8_num_used(&s->rx_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "esp32c3_i2c: RX FIFO underflow\n");
            return 0;
        }
        return fifo8_pop(&s->rx_fifo);
    case A_ESP32C3_I2C_INT_RAW:
        return s->int_raw_reg;
    case A_ESP32C3_I2C_INT_ENA:
        return s->int_ena_reg;
    case A_ESP32C3_I2C_INT_ST:
        return s->int_raw_reg & s->int_ena_reg;
    case A_ESP32C3_I2C_CMD ...
         (A_ESP32C3_I2C_CMD + (ESP32C3_I2C_CMD_COUNT - 1) * 4):
        return s->cmd_reg[(addr - A_ESP32C3_I2C_CMD) / 4];
    case A_ESP32C3_I2C_TIMEOUT:
        return s->timeout_reg;
    case A_ESP32C3_I2C_SDA_HOLD:
        return s->sda_hold_reg;
    case A_ESP32C3_I2C_SDA_SAMPLE:
        return s->sda_sample_reg;
    case A_ESP32C3_I2C_HIGH_PERIOD:
        return s->high_period_reg;
    case A_ESP32C3_I2C_LOW_PERIOD:
        return s->low_period_reg;
    case A_ESP32C3_I2C_START_HOLD:
        return s->start_hold_reg;
    case A_ESP32C3_I2C_RSTART_SETUP:
        return s->rstart_setup_reg;
    case A_ESP32C3_I2C_STOP_HOLD:
        return s->stop_hold_reg;
    case A_ESP32C3_I2C_STOP_SETUP:
        return s->stop_setup_reg;
    default:
        return 0;
    }
}

static void esp32c3_i2c_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    Esp32C3I2CState *s = ESP32C3_I2C(opaque);

    switch (addr) {
    case A_ESP32C3_I2C_CTR:
        if (!FIELD_EX32(value, ESP32C3_I2C_CTR, MS_MODE)) {
            qemu_log_mask(LOG_UNIMP,
                          "esp32c3_i2c: slave mode is not implemented\n");
        }
        if (FIELD_EX32(value, ESP32C3_I2C_CTR, TRANS_START)) {
            esp32c3_i2c_do_transaction(s);
            value &= ~R_ESP32C3_I2C_CTR_TRANS_START_MASK;
        }
        s->ctr_reg = value;
        break;
    case A_ESP32C3_I2C_FIFO_CONF:
        if (FIELD_EX32(value, ESP32C3_I2C_FIFO_CONF, NONFIFO_EN)) {
            qemu_log_mask(LOG_UNIMP,
                          "esp32c3_i2c: APB mode is not implemented\n");
        }
        if (FIELD_EX32(value, ESP32C3_I2C_FIFO_CONF, RX_FIFO_RST)) {
            fifo8_reset(&s->rx_fifo);
        }
        if (FIELD_EX32(value, ESP32C3_I2C_FIFO_CONF, TX_FIFO_RST)) {
            fifo8_reset(&s->tx_fifo);
        }
        break;
    case A_ESP32C3_I2C_FIFO_DATA:
        if (!fifo8_num_free(&s->tx_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "esp32c3_i2c: TX FIFO overflow\n");
        } else {
            fifo8_push(&s->tx_fifo, value);
        }
        break;
    case A_ESP32C3_I2C_INT_CLR:
        s->int_raw_reg &= ~value;
        esp32c3_i2c_update_irq(s);
        break;
    case A_ESP32C3_I2C_INT_ENA:
        s->int_ena_reg = value;
        esp32c3_i2c_update_irq(s);
        break;
    case A_ESP32C3_I2C_CMD ...
         (A_ESP32C3_I2C_CMD + (ESP32C3_I2C_CMD_COUNT - 1) * 4):
        s->cmd_reg[(addr - A_ESP32C3_I2C_CMD) / 4] = value;
        break;
    case A_ESP32C3_I2C_TIMEOUT:
        s->timeout_reg = value;
        break;
    case A_ESP32C3_I2C_SDA_HOLD:
        s->sda_hold_reg = value;
        break;
    case A_ESP32C3_I2C_SDA_SAMPLE:
        s->sda_sample_reg = value;
        break;
    case A_ESP32C3_I2C_HIGH_PERIOD:
        s->high_period_reg = value;
        break;
    case A_ESP32C3_I2C_LOW_PERIOD:
        s->low_period_reg = value;
        break;
    case A_ESP32C3_I2C_START_HOLD:
        s->start_hold_reg = value;
        break;
    case A_ESP32C3_I2C_RSTART_SETUP:
        s->rstart_setup_reg = value;
        break;
    case A_ESP32C3_I2C_STOP_HOLD:
        s->stop_hold_reg = value;
        break;
    case A_ESP32C3_I2C_STOP_SETUP:
        s->stop_setup_reg = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps esp32c3_i2c_ops = {
    .read = esp32c3_i2c_read,
    .write = esp32c3_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32c3_i2c_reset_hold(Object *obj, ResetType type)
{
    Esp32C3I2CState *s = ESP32C3_I2C(obj);

    esp32c3_i2c_finish_transfer(s);
    fifo8_reset(&s->rx_fifo);
    fifo8_reset(&s->tx_fifo);
    s->ctr_reg = 0;
    s->timeout_reg = 0;
    s->int_ena_reg = 0;
    s->int_raw_reg = 0;
    s->sda_hold_reg = 0;
    s->sda_sample_reg = 0;
    s->high_period_reg = 0;
    s->low_period_reg = 0;
    s->start_hold_reg = 0;
    s->rstart_setup_reg = 0;
    s->stop_hold_reg = 0;
    s->stop_setup_reg = 0;
    memset(s->cmd_reg, 0, sizeof(s->cmd_reg));
    esp32c3_i2c_update_irq(s);
}

static void esp32c3_i2c_init(Object *obj)
{
    Esp32C3I2CState *s = ESP32C3_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32c3_i2c_ops, s,
                          TYPE_ESP32C3_I2C, ESP32C3_I2C_MEM_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->bus = i2c_init_bus(DEVICE(s), "i2c");
    fifo8_create(&s->tx_fifo, ESP32C3_I2C_FIFO_LENGTH);
    fifo8_create(&s->rx_fifo, ESP32C3_I2C_FIFO_LENGTH);
}

static void esp32c3_i2c_finalize(Object *obj)
{
    Esp32C3I2CState *s = ESP32C3_I2C(obj);

    fifo8_destroy(&s->tx_fifo);
    fifo8_destroy(&s->rx_fifo);
}

static const VMStateDescription vmstate_esp32c3_i2c = {
    .name = TYPE_ESP32C3_I2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_FIFO8(rx_fifo, Esp32C3I2CState),
        VMSTATE_FIFO8(tx_fifo, Esp32C3I2CState),
        VMSTATE_BOOL(trans_ongoing, Esp32C3I2CState),
        VMSTATE_UINT32(ctr_reg, Esp32C3I2CState),
        VMSTATE_UINT32(timeout_reg, Esp32C3I2CState),
        VMSTATE_UINT32(int_ena_reg, Esp32C3I2CState),
        VMSTATE_UINT32(int_raw_reg, Esp32C3I2CState),
        VMSTATE_UINT32(sda_hold_reg, Esp32C3I2CState),
        VMSTATE_UINT32(sda_sample_reg, Esp32C3I2CState),
        VMSTATE_UINT32(high_period_reg, Esp32C3I2CState),
        VMSTATE_UINT32(low_period_reg, Esp32C3I2CState),
        VMSTATE_UINT32(start_hold_reg, Esp32C3I2CState),
        VMSTATE_UINT32(rstart_setup_reg, Esp32C3I2CState),
        VMSTATE_UINT32(stop_hold_reg, Esp32C3I2CState),
        VMSTATE_UINT32(stop_setup_reg, Esp32C3I2CState),
        VMSTATE_UINT32_ARRAY(cmd_reg, Esp32C3I2CState,
                             ESP32C3_I2C_CMD_COUNT),
        VMSTATE_END_OF_LIST()
    },
};

static void esp32c3_i2c_realize(DeviceState *dev, Error **errp)
{
    Esp32C3I2CState *s = ESP32C3_I2C(dev);
    if (s->fifo_size != 16 && s->fifo_size != 32) {
        error_setg(errp, "ESP I2C FIFO size must be 16 or 32");
        return;
    }
    if (s->fifo_size != ESP32C3_I2C_FIFO_LENGTH) {
        fifo8_destroy(&s->tx_fifo);
        fifo8_destroy(&s->rx_fifo);
        fifo8_create(&s->tx_fifo, s->fifo_size);
        fifo8_create(&s->rx_fifo, s->fifo_size);
    }
}

static const Property esp32c3_i2c_properties[] = {
    DEFINE_PROP_UINT32("fifo-size", Esp32C3I2CState, fifo_size,
                       ESP32C3_I2C_FIFO_LENGTH),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32c3_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = esp32c3_i2c_realize;
    device_class_set_props(dc, esp32c3_i2c_properties);
    dc->vmsd = &vmstate_esp32c3_i2c;
    rc->phases.hold = esp32c3_i2c_reset_hold;
}

static const TypeInfo esp32c3_i2c_info = {
    .name = TYPE_ESP32C3_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32C3I2CState),
    .instance_init = esp32c3_i2c_init,
    .instance_finalize = esp32c3_i2c_finalize,
    .class_init = esp32c3_i2c_class_init,
};

static void esp32c3_i2c_register_types(void)
{
    type_register_static(&esp32c3_i2c_info);
}

type_init(esp32c3_i2c_register_types)
