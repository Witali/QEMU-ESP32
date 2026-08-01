/*
 * Sitronix ST7789 SPI LCD controller.
 *
 * This model implements the command subset used by the ESP-IDF ST7789
 * panel driver and a 320x240 landscape console for the ESP32-2432S028.
 *
 * Copyright (c) 2026 HLV codec contributors
 *
 * This code is licensed under the GPL version 2 or later.
 */

#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/pixel_ops.h"
#include "qom/object.h"

#define ST7789_WIDTH  320
#define ST7789_HEIGHT 240
#define ST7789_CONTROL_HEIGHT 44
#define ST7789_SURFACE_HEIGHT (ST7789_HEIGHT + ST7789_CONTROL_HEIGHT)

#define ST7789_BUTTON_Y      248
#define ST7789_BUTTON_HEIGHT 28
#define ST7789_RESET_X       16
#define ST7789_BOOT_X        118
#define ST7789_BUTTON_WIDTH  90
#define ST7789_HOLD_X        222
#define ST7789_HOLD_Y        255
#define ST7789_CHECK_SIZE    14

#define ST7789_CMD_SWRESET 0x01
#define ST7789_CMD_SLPIN   0x10
#define ST7789_CMD_SLPOUT  0x11
#define ST7789_CMD_INVOFF  0x20
#define ST7789_CMD_INVON   0x21
#define ST7789_CMD_DISPOFF 0x28
#define ST7789_CMD_DISPON  0x29
#define ST7789_CMD_CASET   0x2a
#define ST7789_CMD_RASET   0x2b
#define ST7789_CMD_RAMWR   0x2c
#define ST7789_CMD_MADCTL  0x36
#define ST7789_CMD_COLMOD  0x3a
#define ST7789_CMD_RAMCTRL 0xb0

#define ST7789_MADCTL_BGR  0x08
#define ST7789_RAMCTRL_LITTLE_ENDIAN 0x08

#define TYPE_ST7789 "st7789"
OBJECT_DECLARE_SIMPLE_TYPE(ST7789State, ST7789)

struct ST7789State {
    SSIPeripheral parent_obj;
    QemuConsole *con;
    QemuInputHandlerState *input;
    qemu_irq reset_button;
    qemu_irq boot_button;

    uint8_t command;
    uint8_t argument[4];
    uint8_t argument_index;
    uint8_t pixel_byte;
    uint8_t madctl;
    uint8_t colmod;
    uint8_t ramctrl;
    bool dc;
    bool backlight;
    bool display_on;
    bool sleeping;
    bool inverted;
    bool little_endian;
    bool redraw;
    bool pointer_left;
    bool reset_pressed;
    bool boot_pressed;
    bool emulate_boot_hold;
    int pointer_x;
    int pointer_y;

    uint16_t column;
    uint16_t row;
    uint16_t column_start;
    uint16_t column_end;
    uint16_t row_start;
    uint16_t row_end;
    uint16_t framebuffer[ST7789_WIDTH * ST7789_HEIGHT];
};

static void st7789_reset_state(ST7789State *s, bool clear_framebuffer)
{
    s->command = 0;
    s->argument_index = 0;
    s->pixel_byte = 0;
    s->madctl = 0;
    s->colmod = 0x55;
    s->ramctrl = 0xf0;
    s->display_on = false;
    s->sleeping = true;
    s->inverted = false;
    s->little_endian = false;
    s->column = 0;
    s->row = 0;
    s->column_start = 0;
    s->column_end = ST7789_WIDTH - 1;
    s->row_start = 0;
    s->row_end = ST7789_HEIGHT - 1;
    if (clear_framebuffer) {
        memset(s->framebuffer, 0, sizeof(s->framebuffer));
    }
    s->redraw = true;
}

static void st7789_advance_address(ST7789State *s)
{
    s->column++;
    if (s->column > s->column_end) {
        s->column = s->column_start;
        s->row++;
        if (s->row > s->row_end) {
            s->row = s->row_start;
        }
    }
}

static void st7789_write_pixel_byte(ST7789State *s, uint8_t data)
{
    uint16_t pixel;

    if (s->pixel_byte == 0) {
        s->argument[0] = data;
        s->pixel_byte = 1;
        return;
    }

    if (s->little_endian) {
        pixel = s->argument[0] | ((uint16_t)data << 8);
    } else {
        pixel = ((uint16_t)s->argument[0] << 8) | data;
    }
    s->pixel_byte = 0;

    if (s->column < ST7789_WIDTH && s->row < ST7789_HEIGHT) {
        s->framebuffer[s->row * ST7789_WIDTH + s->column] = pixel;
        s->redraw = true;
    }
    st7789_advance_address(s);
}

static unsigned st7789_argument_count(uint8_t command)
{
    switch (command) {
    case ST7789_CMD_CASET:
    case ST7789_CMD_RASET:
        return 4;
    case ST7789_CMD_MADCTL:
    case ST7789_CMD_COLMOD:
        return 1;
    case ST7789_CMD_RAMCTRL:
        return 2;
    default:
        return 0;
    }
}

static void st7789_apply_arguments(ST7789State *s)
{
    switch (s->command) {
    case ST7789_CMD_CASET:
        s->column_start = ((uint16_t)s->argument[0] << 8) | s->argument[1];
        s->column_end = ((uint16_t)s->argument[2] << 8) | s->argument[3];
        s->column = s->column_start;
        break;
    case ST7789_CMD_RASET:
        s->row_start = ((uint16_t)s->argument[0] << 8) | s->argument[1];
        s->row_end = ((uint16_t)s->argument[2] << 8) | s->argument[3];
        s->row = s->row_start;
        break;
    case ST7789_CMD_MADCTL:
        s->madctl = s->argument[0];
        s->redraw = true;
        break;
    case ST7789_CMD_COLMOD:
        s->colmod = s->argument[0];
        break;
    case ST7789_CMD_RAMCTRL:
        s->ramctrl = s->argument[1];
        s->little_endian =
            (s->ramctrl & ST7789_RAMCTRL_LITTLE_ENDIAN) != 0;
        break;
    default:
        break;
    }
}

static void st7789_command(ST7789State *s, uint8_t command)
{
    s->command = command;
    s->argument_index = 0;
    s->pixel_byte = 0;

    switch (command) {
    case ST7789_CMD_SWRESET:
        st7789_reset_state(s, true);
        break;
    case ST7789_CMD_SLPIN:
        s->sleeping = true;
        s->redraw = true;
        break;
    case ST7789_CMD_SLPOUT:
        s->sleeping = false;
        s->redraw = true;
        break;
    case ST7789_CMD_INVOFF:
        s->inverted = false;
        s->redraw = true;
        break;
    case ST7789_CMD_INVON:
        s->inverted = true;
        s->redraw = true;
        break;
    case ST7789_CMD_DISPOFF:
        s->display_on = false;
        s->redraw = true;
        break;
    case ST7789_CMD_DISPON:
        s->display_on = true;
        s->redraw = true;
        break;
    default:
        break;
    }
}

static uint32_t st7789_transfer(SSIPeripheral *dev, uint32_t data)
{
    ST7789State *s = ST7789(dev);
    uint8_t byte = data;
    unsigned argument_count;

    if (!s->dc) {
        st7789_command(s, byte);
        return 0;
    }

    if (s->command == ST7789_CMD_RAMWR) {
        if ((s->colmod & 0x77) == 0x55) {
            st7789_write_pixel_byte(s, byte);
        }
        return 0;
    }

    argument_count = st7789_argument_count(s->command);
    if (s->argument_index < argument_count) {
        s->argument[s->argument_index++] = byte;
        if (s->argument_index == argument_count) {
            st7789_apply_arguments(s);
        }
    }
    return 0;
}

static void st7789_transfer_buf(SSIPeripheral *dev, const uint8_t *tx,
                                uint8_t *rx, size_t len)
{
    ST7789State *s = ST7789(dev);
    size_t offset = 0;

    if (rx) {
        memset(rx, 0, len);
    }

    /*
     * Commands and short argument transfers are rare, so preserve their
     * byte-at-a-time behavior.  Pixel DMA is the hot path: a CIF frame sends
     * 153600 bytes, which must not become that many QOM callback calls.
     */
    if (!tx || !s->dc || s->command != ST7789_CMD_RAMWR ||
        (s->colmod & 0x77) != 0x55) {
        while (offset < len) {
            st7789_transfer(dev, tx ? tx[offset] : 0xff);
            offset++;
        }
        return;
    }

    if (s->pixel_byte && offset < len) {
        st7789_write_pixel_byte(s, tx[offset++]);
    }

    while (offset + 1 < len) {
        size_t pixels = (len - offset) / 2;
        size_t row_pixels;

        if (s->column > s->column_end) {
            s->column = s->column_start;
        }
        if (s->row > s->row_end) {
            s->row = s->row_start;
        }

        row_pixels = s->column_end - s->column + 1;
        row_pixels = MIN(row_pixels, pixels);

        if (s->little_endian && s->column < ST7789_WIDTH &&
            s->row < ST7789_HEIGHT &&
            row_pixels <= ST7789_WIDTH - s->column) {
            memcpy(&s->framebuffer[s->row * ST7789_WIDTH + s->column],
                   tx + offset, row_pixels * sizeof(uint16_t));
            s->redraw = true;
            offset += row_pixels * sizeof(uint16_t);
            s->column += row_pixels;
            if (s->column > s->column_end) {
                s->column = s->column_start;
                s->row++;
                if (s->row > s->row_end) {
                    s->row = s->row_start;
                }
            }
        } else {
            st7789_write_pixel_byte(s, tx[offset++]);
            st7789_write_pixel_byte(s, tx[offset++]);
        }
    }

    if (offset < len) {
        st7789_write_pixel_byte(s, tx[offset]);
    }
}

static uint32_t st7789_rgb_to_surface(DisplaySurface *surface,
                                      uint8_t red, uint8_t green,
                                      uint8_t blue)
{
    switch (surface_bits_per_pixel(surface)) {
    case 8:
        return rgb_to_pixel8(red, green, blue);
    case 15:
        return rgb_to_pixel15(red, green, blue);
    case 16:
        return rgb_to_pixel16(red, green, blue);
    case 24:
        return rgb_to_pixel24(red, green, blue);
    case 32:
        return rgb_to_pixel32(red, green, blue);
    default:
        return 0;
    }
}

static void st7789_store_surface_pixel(uint8_t *dest, int bytes_per_pixel,
                                       uint32_t pixel)
{
    switch (bytes_per_pixel) {
    case 1:
        *dest = pixel;
        break;
    case 2:
        *(uint16_t *)dest = pixel;
        break;
    case 3:
        memcpy(dest, &pixel, 3);
        break;
    case 4:
        *(uint32_t *)dest = pixel;
        break;
    default:
        break;
    }
}

static void st7789_put_surface_rgb(DisplaySurface *surface, int x, int y,
                                    uint8_t red, uint8_t green, uint8_t blue)
{
    int bytes_per_pixel = (surface_bits_per_pixel(surface) + 7) >> 3;
    uint8_t *dest = surface_data(surface) + y * surface_stride(surface) +
                    x * bytes_per_pixel;
    uint32_t pixel =
        st7789_rgb_to_surface(surface, red, green, blue);

    st7789_store_surface_pixel(dest, bytes_per_pixel, pixel);
}

static const uint8_t *st7789_glyph(char character)
{
    static const uint8_t b[7] = {
        0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e
    };
    static const uint8_t e[7] = {
        0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f
    };
    static const uint8_t d[7] = {
        0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e
    };
    static const uint8_t h[7] = {
        0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11
    };
    static const uint8_t l[7] = {
        0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f
    };
    static const uint8_t o[7] = {
        0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e
    };
    static const uint8_t r[7] = {
        0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11
    };
    static const uint8_t s[7] = {
        0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e
    };
    static const uint8_t t[7] = {
        0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04
    };

    switch (character) {
    case 'B':
        return b;
    case 'D':
        return d;
    case 'E':
        return e;
    case 'H':
        return h;
    case 'L':
        return l;
    case 'O':
        return o;
    case 'R':
        return r;
    case 'S':
        return s;
    case 'T':
        return t;
    default:
        return NULL;
    }
}

static void st7789_draw_text(DisplaySurface *surface, int x, int y,
                              const char *text)
{
    const int scale = 2;

    while (*text) {
        const uint8_t *glyph = st7789_glyph(*text++);
        int row;

        if (glyph) {
            for (row = 0; row < 7; row++) {
                int column;

                for (column = 0; column < 5; column++) {
                    if (glyph[row] & (1 << (4 - column))) {
                        int dx;
                        int dy;

                        for (dy = 0; dy < scale; dy++) {
                            for (dx = 0; dx < scale; dx++) {
                                st7789_put_surface_rgb(
                                    surface, x + column * scale + dx,
                                    y + row * scale + dy,
                                    0xf4, 0xf4, 0xf4);
                            }
                        }
                    }
                }
            }
        }
        x += 6 * scale;
    }
}

static void st7789_draw_button(DisplaySurface *surface, int button_x,
                                bool active, const char *label)
{
    int label_width = strlen(label) * 12 - 2;
    int x;
    int y;

    for (y = ST7789_BUTTON_Y;
         y < ST7789_BUTTON_Y + ST7789_BUTTON_HEIGHT; y++) {
        for (x = button_x; x < button_x + ST7789_BUTTON_WIDTH; x++) {
            bool border = x == button_x ||
                          x == button_x + ST7789_BUTTON_WIDTH - 1 ||
                          y == ST7789_BUTTON_Y ||
                          y == ST7789_BUTTON_Y + ST7789_BUTTON_HEIGHT - 1;

            if (border) {
                st7789_put_surface_rgb(surface, x, y,
                                       0x9a, 0xa0, 0xaa);
            } else if (active) {
                st7789_put_surface_rgb(surface, x, y,
                                       0x9b, 0x35, 0x35);
            } else {
                st7789_put_surface_rgb(surface, x, y,
                                       0x3f, 0x47, 0x53);
            }
        }
    }

    st7789_draw_text(surface,
                     button_x + (ST7789_BUTTON_WIDTH - label_width) / 2,
                     ST7789_BUTTON_Y + 7, label);
}

static void st7789_draw_controls(ST7789State *s, DisplaySurface *surface)
{
    int x;
    int y;

    for (y = ST7789_HEIGHT; y < ST7789_SURFACE_HEIGHT; y++) {
        for (x = 0; x < ST7789_WIDTH; x++) {
            st7789_put_surface_rgb(surface, x, y, 0x20, 0x24, 0x2a);
        }
    }

    st7789_draw_button(surface, ST7789_RESET_X, s->reset_pressed, "RESET");
    st7789_draw_button(surface, ST7789_BOOT_X, s->boot_pressed, "BOOT");

    for (y = ST7789_HOLD_Y; y < ST7789_HOLD_Y + ST7789_CHECK_SIZE; y++) {
        for (x = ST7789_HOLD_X; x < ST7789_HOLD_X + ST7789_CHECK_SIZE; x++) {
            bool border = x == ST7789_HOLD_X ||
                          x == ST7789_HOLD_X + ST7789_CHECK_SIZE - 1 ||
                          y == ST7789_HOLD_Y ||
                          y == ST7789_HOLD_Y + ST7789_CHECK_SIZE - 1;

            if (border) {
                st7789_put_surface_rgb(surface, x, y,
                                       0x9a, 0xa0, 0xaa);
            } else if (s->emulate_boot_hold) {
                st7789_put_surface_rgb(surface, x, y,
                                       0x9b, 0x35, 0x35);
            } else {
                st7789_put_surface_rgb(surface, x, y,
                                       0x20, 0x24, 0x2a);
            }
        }
    }
    st7789_draw_text(surface, ST7789_HOLD_X + ST7789_CHECK_SIZE + 6,
                     ST7789_HOLD_Y, "HOLD");
}

static void st7789_update_display(void *opaque)
{
    ST7789State *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    int bytes_per_pixel;
    bool blanked;
    int x;
    int y;

    if (!s->redraw || surface_bits_per_pixel(surface) == 0) {
        return;
    }

    bytes_per_pixel = (surface_bits_per_pixel(surface) + 7) >> 3;
    if (bytes_per_pixel < 1 || bytes_per_pixel > 4) {
        return;
    }
    blanked = !s->backlight || !s->display_on || s->sleeping;

    for (y = 0; y < ST7789_HEIGHT; y++) {
        uint8_t *dest =
            surface_data(surface) + y * surface_stride(surface);

        for (x = 0; x < ST7789_WIDTH; x++) {
            uint16_t color =
                blanked ? 0 : s->framebuffer[y * ST7789_WIDTH + x];
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            uint32_t pixel;

            if (s->inverted && !blanked) {
                color ^= 0xffff;
            }
            red = ((color >> 11) & 0x1f) * 255 / 31;
            green = ((color >> 5) & 0x3f) * 255 / 63;
            blue = (color & 0x1f) * 255 / 31;
            if (s->madctl & ST7789_MADCTL_BGR) {
                uint8_t swap = red;
                red = blue;
                blue = swap;
            }
            pixel = st7789_rgb_to_surface(surface, red, green, blue);
            st7789_store_surface_pixel(dest, bytes_per_pixel, pixel);
            dest += bytes_per_pixel;
        }
    }

    st7789_draw_controls(s, surface);
    s->redraw = false;
    dpy_gfx_update_full(s->con);
}

static void st7789_invalidate_display(void *opaque)
{
    ST7789State *s = opaque;

    s->redraw = true;
}

static void st7789_dc(void *opaque, int line, int level)
{
    ST7789State *s = opaque;

    s->dc = level != 0;
}

static void st7789_backlight(void *opaque, int line, int level)
{
    ST7789State *s = opaque;
    bool enabled = level != 0;

    if (s->backlight != enabled) {
        s->backlight = enabled;
        s->redraw = true;
    }
}

static bool st7789_button_contains(int x, int y, int button_x)
{
    return x >= button_x && x < button_x + ST7789_BUTTON_WIDTH &&
           y >= ST7789_BUTTON_Y &&
           y < ST7789_BUTTON_Y + ST7789_BUTTON_HEIGHT;
}

static bool st7789_hold_contains(int x, int y)
{
    return x >= ST7789_HOLD_X &&
           x < ST7789_HOLD_X + ST7789_CHECK_SIZE + 6 + 4 * 12 &&
           y >= ST7789_HOLD_Y && y < ST7789_HOLD_Y + ST7789_CHECK_SIZE;
}

static void st7789_input_event(DeviceState *device, QemuConsole *src,
                                InputEvent *event)
{
    ST7789State *s = ST7789(device);

    if (src && src != s->con) {
        return;
    }

    switch (event->type) {
    case INPUT_EVENT_KIND_ABS: {
        InputMoveEvent *move = event->u.abs.data;

        if (move->axis == INPUT_AXIS_X) {
            s->pointer_x = move->value * (ST7789_WIDTH - 1) /
                           INPUT_EVENT_ABS_MAX;
        } else if (move->axis == INPUT_AXIS_Y) {
            s->pointer_y = move->value * (ST7789_SURFACE_HEIGHT - 1) /
                           INPUT_EVENT_ABS_MAX;
        }
        break;
    }
    case INPUT_EVENT_KIND_BTN: {
        InputBtnEvent *button = event->u.btn.data;

        if (button->button != INPUT_BUTTON_LEFT ||
            button->down == s->pointer_left) {
            break;
        }

        s->pointer_left = button->down;
        if (button->down &&
            st7789_button_contains(s->pointer_x, s->pointer_y,
                                   ST7789_RESET_X)) {
            s->reset_pressed = true;
            s->redraw = true;
            qemu_set_irq(s->reset_button, 1);
        } else if (button->down &&
                   st7789_button_contains(s->pointer_x, s->pointer_y,
                                          ST7789_BOOT_X)) {
            s->boot_pressed = true;
            s->redraw = true;
            qemu_set_irq(s->boot_button, 1);
        } else if (button->down &&
                   st7789_hold_contains(s->pointer_x, s->pointer_y)) {
            s->emulate_boot_hold = !s->emulate_boot_hold;
            s->redraw = true;
            qemu_set_irq(s->boot_button,
                         s->boot_pressed || s->emulate_boot_hold);
        } else if (!button->down) {
            if (s->reset_pressed) {
                s->reset_pressed = false;
                qemu_set_irq(s->reset_button, 0);
                s->redraw = true;
            }
            if (s->boot_pressed) {
                s->boot_pressed = false;
                qemu_set_irq(s->boot_button, s->emulate_boot_hold);
                s->redraw = true;
            }
        }
        break;
    }
    default:
        break;
    }
}

static const QemuInputHandler st7789_input_handler = {
    .name = "ESP32 ST7789 controls",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = st7789_input_event,
};

static int st7789_post_load(void *opaque, int version_id)
{
    st7789_invalidate_display(opaque);
    return 0;
}

static const VMStateDescription vmstate_st7789 = {
    .name = "st7789",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = st7789_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(command, ST7789State),
        VMSTATE_UINT8_ARRAY(argument, ST7789State, 4),
        VMSTATE_UINT8(argument_index, ST7789State),
        VMSTATE_UINT8(pixel_byte, ST7789State),
        VMSTATE_UINT8(madctl, ST7789State),
        VMSTATE_UINT8(colmod, ST7789State),
        VMSTATE_UINT8(ramctrl, ST7789State),
        VMSTATE_BOOL(dc, ST7789State),
        VMSTATE_BOOL(backlight, ST7789State),
        VMSTATE_BOOL(display_on, ST7789State),
        VMSTATE_BOOL(sleeping, ST7789State),
        VMSTATE_BOOL(inverted, ST7789State),
        VMSTATE_BOOL(little_endian, ST7789State),
        VMSTATE_UINT16(column, ST7789State),
        VMSTATE_UINT16(row, ST7789State),
        VMSTATE_UINT16(column_start, ST7789State),
        VMSTATE_UINT16(column_end, ST7789State),
        VMSTATE_UINT16(row_start, ST7789State),
        VMSTATE_UINT16(row_end, ST7789State),
        VMSTATE_UINT16_ARRAY(framebuffer, ST7789State,
                             ST7789_WIDTH * ST7789_HEIGHT),
        VMSTATE_SSI_PERIPHERAL(parent_obj, ST7789State),
        VMSTATE_END_OF_LIST()
    },
};

static const GraphicHwOps st7789_ops = {
    .invalidate = st7789_invalidate_display,
    .gfx_update = st7789_update_display,
};

static void st7789_realize(SSIPeripheral *dev, Error **errp)
{
    DeviceState *device = DEVICE(dev);
    ST7789State *s = ST7789(dev);

    s->con = graphic_console_init(device, 0, &st7789_ops, s);
    qemu_console_resize(s->con, ST7789_WIDTH, ST7789_SURFACE_HEIGHT);
    qdev_init_gpio_in_named(device, st7789_dc, "dc", 1);
    qdev_init_gpio_in_named(device, st7789_backlight, "backlight", 1);
    qdev_init_gpio_out_named(device, &s->reset_button, "reset-button", 1);
    qdev_init_gpio_out_named(device, &s->boot_button, "boot-button", 1);
    s->input = qemu_input_handler_register(device, &st7789_input_handler);
    st7789_reset_state(s, true);
}

static void st7789_unrealize(DeviceState *device)
{
    ST7789State *s = ST7789(device);

    qemu_input_handler_unregister(s->input);
    s->input = NULL;
}

static void st7789_reset(DeviceState *device)
{
    ST7789State *s = ST7789(device);

    st7789_reset_state(s, true);
}

static void st7789_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *ssi = SSI_PERIPHERAL_CLASS(klass);

    ssi->realize = st7789_realize;
    ssi->transfer = st7789_transfer;
    ssi->transfer_buf = st7789_transfer_buf;
    ssi->cs_polarity = SSI_CS_LOW;
    dc->vmsd = &vmstate_st7789;
    dc->unrealize = st7789_unrealize;
    device_class_set_legacy_reset(dc, st7789_reset);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo st7789_info = {
    .name = TYPE_ST7789,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(ST7789State),
    .class_init = st7789_class_init,
};

static void st7789_register_types(void)
{
    type_register_static(&st7789_info);
}

type_init(st7789_register_types)
