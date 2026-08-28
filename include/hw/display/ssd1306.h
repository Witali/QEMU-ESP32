/*
 * Solomon Systech SSD1306 monochrome OLED controller.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_SSD1306_H
#define HW_DISPLAY_SSD1306_H

#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_SSD1306 "ssd1306"
OBJECT_DECLARE_SIMPLE_TYPE(SSD1306State, SSD1306)

#endif
