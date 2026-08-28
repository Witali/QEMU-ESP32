/*
 * Solomon Systech SSD1306 monochrome OLED controller.
 *
 * This model implements the I2C control byte protocol, the 128x64-bit GDDRAM,
 * horizontal/vertical/page addressing, display power and inversion, segment
 * and COM remapping, and the command set used by common SSD1306 drivers.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/display/ssd1306.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "ui/console.h"

#define SSD1306_RAM_WIDTH 128
#define SSD1306_RAM_HEIGHT 64
#define SSD1306_RAM_PAGES (SSD1306_RAM_HEIGHT / 8)
#define SSD1306_MAX_COMMAND_PARAMS 6

typedef enum SSD1306AddressMode {
    SSD1306_ADDR_HORIZONTAL = 0,
    SSD1306_ADDR_VERTICAL = 1,
    SSD1306_ADDR_PAGE = 2,
} SSD1306AddressMode;

struct SSD1306State {
    I2CSlave parent_obj;

    QemuConsole *console;
    uint8_t ram[SSD1306_RAM_WIDTH * SSD1306_RAM_PAGES];

    uint8_t column;
    uint8_t page;
    uint8_t column_start;
    uint8_t column_end;
    uint8_t page_start;
    uint8_t page_end;
    uint8_t address_mode;
    uint8_t start_line;
    uint8_t display_offset;
    uint8_t multiplex;
    uint8_t contrast;

    bool enabled;
    bool inverse;
    bool entire_display_on;
    bool segment_remap;
    bool com_scan_decrement;
    bool charge_pump;
    bool scroll_enabled;

    bool expect_control;
    bool data_mode;
    bool single_byte_control;
    uint8_t pending_command;
    uint8_t parameter_count;
    uint8_t parameter_index;
    uint8_t parameters[SSD1306_MAX_COMMAND_PARAMS];

    uint32_t visible_width;
    uint32_t visible_height;
    uint32_t column_offset;
    uint32_t magnify;
    bool redraw;
};

static void ssd1306_mark_redraw(SSD1306State *s)
{
    s->redraw = true;
}

static void ssd1306_advance_address(SSD1306State *s)
{
    switch (s->address_mode) {
    case SSD1306_ADDR_VERTICAL:
        if (s->page >= s->page_end) {
            s->page = s->page_start;
            s->column = s->column >= s->column_end ? s->column_start
                                                   : s->column + 1;
        } else {
            s->page++;
        }
        break;
    case SSD1306_ADDR_HORIZONTAL:
        if (s->column >= s->column_end) {
            s->column = s->column_start;
            s->page = s->page >= s->page_end ? s->page_start : s->page + 1;
        } else {
            s->column++;
        }
        break;
    case SSD1306_ADDR_PAGE:
    default:
        s->column = s->column >= s->column_end ? s->column_start
                                               : s->column + 1;
        break;
    }
}

static void ssd1306_write_data(SSD1306State *s, uint8_t value)
{
    if (s->column < SSD1306_RAM_WIDTH && s->page < SSD1306_RAM_PAGES) {
        s->ram[s->page * SSD1306_RAM_WIDTH + s->column] = value;
        ssd1306_mark_redraw(s);
    }
    ssd1306_advance_address(s);
}

static void ssd1306_apply_parameters(SSD1306State *s)
{
    switch (s->pending_command) {
    case 0x20: /* Set memory addressing mode. */
        if (s->parameters[0] <= SSD1306_ADDR_PAGE) {
            s->address_mode = s->parameters[0];
        }
        break;
    case 0x21: /* Set column address range. */
        s->column_start = MIN(s->parameters[0], SSD1306_RAM_WIDTH - 1);
        s->column_end = MIN(s->parameters[1], SSD1306_RAM_WIDTH - 1);
        if (s->column_start > s->column_end) {
            s->column_start = s->column_end;
        }
        s->column = s->column_start;
        break;
    case 0x22: /* Set page address range. */
        s->page_start = MIN(s->parameters[0], SSD1306_RAM_PAGES - 1);
        s->page_end = MIN(s->parameters[1], SSD1306_RAM_PAGES - 1);
        if (s->page_start > s->page_end) {
            s->page_start = s->page_end;
        }
        s->page = s->page_start;
        break;
    case 0x81:
        s->contrast = s->parameters[0];
        ssd1306_mark_redraw(s);
        break;
    case 0x8d:
        s->charge_pump = (s->parameters[0] & 0x10) != 0;
        break;
    case 0xa8:
        s->multiplex = MIN(s->parameters[0], SSD1306_RAM_HEIGHT - 1);
        ssd1306_mark_redraw(s);
        break;
    case 0xd3:
        s->display_offset = s->parameters[0] & 0x3f;
        ssd1306_mark_redraw(s);
        break;
    case 0x26: /* Horizontal scroll commands are accepted and tracked. */
    case 0x27:
    case 0x29:
    case 0x2a:
    case 0xad: /* External IREF selection used by some small panels. */
    case 0xd5: /* Display clock divide ratio. */
    case 0xd9: /* Pre-charge period. */
    case 0xda: /* COM pin configuration. */
    case 0xdb: /* VCOMH deselect level. */
        break;
    default:
        break;
    }

    s->pending_command = 0;
    s->parameter_count = 0;
    s->parameter_index = 0;
}

static void ssd1306_expect_parameters(SSD1306State *s, uint8_t command,
                                      uint8_t count)
{
    s->pending_command = command;
    s->parameter_count = count;
    s->parameter_index = 0;
}

static void ssd1306_write_command(SSD1306State *s, uint8_t command)
{
    if (s->parameter_index < s->parameter_count) {
        s->parameters[s->parameter_index++] = command;
        if (s->parameter_index == s->parameter_count) {
            ssd1306_apply_parameters(s);
        }
        return;
    }

    switch (command) {
    case 0x00 ... 0x0f:
        s->column = (s->column & 0xf0) | (command & 0x0f);
        break;
    case 0x10 ... 0x1f:
        s->column = (s->column & 0x0f) | ((command & 0x0f) << 4);
        s->column = MIN(s->column, SSD1306_RAM_WIDTH - 1);
        break;
    case 0x20:
    case 0x81:
    case 0x8d:
    case 0xa8:
    case 0xad:
    case 0xd3:
    case 0xd5:
    case 0xd9:
    case 0xda:
    case 0xdb:
        ssd1306_expect_parameters(s, command, 1);
        break;
    case 0x21:
    case 0x22:
        ssd1306_expect_parameters(s, command, 2);
        break;
    case 0x26:
    case 0x27:
        ssd1306_expect_parameters(s, command, 6);
        break;
    case 0x29:
    case 0x2a:
        ssd1306_expect_parameters(s, command, 5);
        break;
    case 0x2e:
        s->scroll_enabled = false;
        break;
    case 0x2f:
        s->scroll_enabled = true;
        break;
    case 0x40 ... 0x7f:
        s->start_line = command & 0x3f;
        ssd1306_mark_redraw(s);
        break;
    case 0xa0:
    case 0xa1:
        s->segment_remap = (command & 1) != 0;
        ssd1306_mark_redraw(s);
        break;
    case 0xa4:
    case 0xa5:
        s->entire_display_on = (command & 1) != 0;
        ssd1306_mark_redraw(s);
        break;
    case 0xa6:
    case 0xa7:
        s->inverse = (command & 1) != 0;
        ssd1306_mark_redraw(s);
        break;
    case 0xae:
    case 0xaf:
        s->enabled = (command & 1) != 0;
        ssd1306_mark_redraw(s);
        break;
    case 0xb0 ... 0xb7:
        s->page = command & 7;
        break;
    case 0xc0:
        s->com_scan_decrement = false;
        ssd1306_mark_redraw(s);
        break;
    case 0xc8:
        s->com_scan_decrement = true;
        ssd1306_mark_redraw(s);
        break;
    case 0xe3: /* NOP */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ssd1306: unsupported command 0x%02x\n", command);
        break;
    }
}

static uint8_t ssd1306_recv(I2CSlave *i2c)
{
    return 0xff;
}

static int ssd1306_send(I2CSlave *i2c, uint8_t value)
{
    SSD1306State *s = SSD1306(i2c);

    if (s->expect_control) {
        if (value & 0x3f) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ssd1306: invalid I2C control byte 0x%02x\n", value);
        }
        s->single_byte_control = (value & 0x80) != 0;
        s->data_mode = (value & 0x40) != 0;
        s->expect_control = false;
        return 0;
    }

    if (s->data_mode) {
        ssd1306_write_data(s, value);
    } else {
        ssd1306_write_command(s, value);
    }
    if (s->single_byte_control) {
        s->expect_control = true;
    }
    return 0;
}

static int ssd1306_event(I2CSlave *i2c, enum i2c_event event)
{
    SSD1306State *s = SSD1306(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->expect_control = true;
        s->single_byte_control = false;
        break;
    case I2C_FINISH:
        s->expect_control = true;
        break;
    case I2C_START_RECV:
    case I2C_NACK:
        break;
    default:
        return -1;
    }
    return 0;
}

static bool ssd1306_pixel(SSD1306State *s, uint32_t x, uint32_t y)
{
    uint32_t segment = s->column_offset + x;
    uint32_t ram_column = s->segment_remap ? segment
                                           : SSD1306_RAM_WIDTH - 1 - segment;
    uint32_t com = s->com_scan_decrement ? y : s->visible_height - 1 - y;
    uint32_t line = (com + s->start_line + s->display_offset) & 0x3f;
    bool on;

    if (ram_column >= SSD1306_RAM_WIDTH || line > s->multiplex) {
        on = false;
    } else {
        on = (s->ram[(line >> 3) * SSD1306_RAM_WIDTH + ram_column] &
              (1U << (line & 7))) != 0;
    }
    if (s->entire_display_on) {
        on = true;
    }
    return s->enabled && (on ^ s->inverse);
}

static void ssd1306_update_display(void *opaque)
{
    SSD1306State *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->console);
    uint8_t *data;
    int bytes_per_pixel;
    int stride;

    if (!s->redraw) {
        return;
    }

    bytes_per_pixel = surface_bits_per_pixel(surface) / 8;
    if (bytes_per_pixel < 2 || bytes_per_pixel > 4) {
        return;
    }
    data = surface_data(surface);
    stride = surface_stride(surface);

    for (uint32_t y = 0; y < s->visible_height; y++) {
        for (uint32_t sy = 0; sy < s->magnify; sy++) {
            uint8_t *row = data + (y * s->magnify + sy) * stride;
            for (uint32_t x = 0; x < s->visible_width; x++) {
                bool on = ssd1306_pixel(s, x, y);
                for (uint32_t sx = 0; sx < s->magnify; sx++) {
                    memset(row, on ? 0xff : 0x00, bytes_per_pixel);
                    row += bytes_per_pixel;
                }
            }
        }
    }

    s->redraw = false;
    dpy_gfx_update(s->console, 0, 0, s->visible_width * s->magnify,
                   s->visible_height * s->magnify);
}

static void ssd1306_invalidate_display(void *opaque)
{
    ssd1306_mark_redraw(opaque);
}

static const GraphicHwOps ssd1306_display_ops = {
    .invalidate = ssd1306_invalidate_display,
    .gfx_update = ssd1306_update_display,
};

static void ssd1306_reset(DeviceState *dev)
{
    SSD1306State *s = SSD1306(dev);

    memset(s->ram, 0, sizeof(s->ram));
    s->column = 0;
    s->page = 0;
    s->column_start = 0;
    s->column_end = SSD1306_RAM_WIDTH - 1;
    s->page_start = 0;
    s->page_end = SSD1306_RAM_PAGES - 1;
    s->address_mode = SSD1306_ADDR_PAGE;
    s->start_line = 0;
    s->display_offset = 0;
    s->multiplex = SSD1306_RAM_HEIGHT - 1;
    s->contrast = 0x7f;
    s->enabled = false;
    s->inverse = false;
    s->entire_display_on = false;
    s->segment_remap = false;
    s->com_scan_decrement = false;
    s->charge_pump = false;
    s->scroll_enabled = false;
    s->expect_control = true;
    s->data_mode = false;
    s->single_byte_control = false;
    s->pending_command = 0;
    s->parameter_count = 0;
    s->parameter_index = 0;
    ssd1306_mark_redraw(s);
}

static void ssd1306_realize(DeviceState *dev, Error **errp)
{
    SSD1306State *s = SSD1306(dev);

    if (!s->visible_width || s->visible_width > SSD1306_RAM_WIDTH ||
        !s->visible_height || s->visible_height > SSD1306_RAM_HEIGHT ||
        s->column_offset + s->visible_width > SSD1306_RAM_WIDTH ||
        !s->magnify || s->magnify > 16) {
        error_setg(errp, "Invalid SSD1306 panel geometry");
        return;
    }

    s->console = graphic_console_init(dev, 0, &ssd1306_display_ops, s);
    qemu_console_resize(s->console, s->visible_width * s->magnify,
                        s->visible_height * s->magnify);
}

static const VMStateDescription vmstate_ssd1306 = {
    .name = TYPE_SSD1306,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER(ram, SSD1306State),
        VMSTATE_UINT8(column, SSD1306State),
        VMSTATE_UINT8(page, SSD1306State),
        VMSTATE_UINT8(column_start, SSD1306State),
        VMSTATE_UINT8(column_end, SSD1306State),
        VMSTATE_UINT8(page_start, SSD1306State),
        VMSTATE_UINT8(page_end, SSD1306State),
        VMSTATE_UINT8(address_mode, SSD1306State),
        VMSTATE_UINT8(start_line, SSD1306State),
        VMSTATE_UINT8(display_offset, SSD1306State),
        VMSTATE_UINT8(multiplex, SSD1306State),
        VMSTATE_UINT8(contrast, SSD1306State),
        VMSTATE_BOOL(enabled, SSD1306State),
        VMSTATE_BOOL(inverse, SSD1306State),
        VMSTATE_BOOL(entire_display_on, SSD1306State),
        VMSTATE_BOOL(segment_remap, SSD1306State),
        VMSTATE_BOOL(com_scan_decrement, SSD1306State),
        VMSTATE_BOOL(charge_pump, SSD1306State),
        VMSTATE_BOOL(scroll_enabled, SSD1306State),
        VMSTATE_I2C_SLAVE(parent_obj, SSD1306State),
        VMSTATE_END_OF_LIST()
    },
};

static const Property ssd1306_properties[] = {
    DEFINE_PROP_UINT32("visible-width", SSD1306State, visible_width, 128),
    DEFINE_PROP_UINT32("visible-height", SSD1306State, visible_height, 64),
    DEFINE_PROP_UINT32("column-offset", SSD1306State, column_offset, 0),
    DEFINE_PROP_UINT32("magnify", SSD1306State, magnify, 4),
    DEFINE_PROP_END_OF_LIST(),
};

static void ssd1306_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *isc = I2C_SLAVE_CLASS(klass);

    dc->realize = ssd1306_realize;
    dc->vmsd = &vmstate_ssd1306;
    device_class_set_legacy_reset(dc, ssd1306_reset);
    device_class_set_props(dc, ssd1306_properties);
    isc->event = ssd1306_event;
    isc->recv = ssd1306_recv;
    isc->send = ssd1306_send;
}

static const TypeInfo ssd1306_info = {
    .name = TYPE_SSD1306,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(SSD1306State),
    .class_init = ssd1306_class_init,
};

static void ssd1306_register_types(void)
{
    type_register_static(&ssd1306_info);
}

type_init(ssd1306_register_types)
