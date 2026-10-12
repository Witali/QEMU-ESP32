#pragma once

#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/registerfields.h"

#define TYPE_ESP32_GPIO "esp32.gpio"
#define ESP32_GPIO(obj)             OBJECT_CHECK(Esp32GpioState, (obj), TYPE_ESP32_GPIO)
#define ESP32_GPIO_GET_CLASS(obj)   OBJECT_GET_CLASS(Esp32GpioClass, obj, TYPE_ESP32_GPIO)
#define ESP32_GPIO_CLASS(klass)     OBJECT_CLASS_CHECK(Esp32GpioClass, klass, TYPE_ESP32_GPIO)

REG32(GPIO_STRAP, 0x0038)
REG32(GPIO_IN, 0x003c)
REG32(GPIO_IN1, 0x0040)

#define ESP32_STRAP_MODE_FLASH_BOOT 0x12
#define ESP32_STRAP_MODE_UART_BOOT  0x0f
#define ESP32_GPIO_PIN_COUNT 40

typedef struct Esp32GpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem, iomux;
    bool matrix_enabled, matrix_c3;
    uint32_t pin_count;
    uint64_t enabled;
    uint32_t func_in[256], func_out[40], iomux_regs[256];
    unsigned signal_duty[256];
    qemu_irq duty_gpio[40];
    struct EspGpioSink *gpio_sinks[40], *duty_sinks[40];
    qemu_irq irq;
    qemu_irq out_gpio[ESP32_GPIO_PIN_COUNT];
    uint32_t out;
    uint32_t out1;
    uint32_t strap_mode;
    uint32_t in;
    uint32_t in1;
} Esp32GpioState;

typedef struct Esp32GpioClass {
    SysBusDeviceClass parent_class;
} Esp32GpioClass;

void esp32_gpio_set_input(Esp32GpioState *state, unsigned pin, int level);

bool esp_gpio_has_signal(Esp32GpioState *, unsigned pin, unsigned signal, bool input);
bool esp_gpio_output_enabled(Esp32GpioState *, unsigned pin);
void esp_gpio_set_signal_duty(Esp32GpioState *, unsigned signal, unsigned duty);
void esp_gpio_connect(Esp32GpioState *, unsigned pin, qemu_irq sink, bool pwm);
