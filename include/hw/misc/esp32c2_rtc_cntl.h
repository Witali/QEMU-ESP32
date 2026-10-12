#pragma once

#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"

#define TYPE_ESP32C2_RTC_CNTL "misc.esp32c2.rtc_cntl"
#define ESP32C2_RTC_CNTL(obj) OBJECT_CHECK(ESP32C2RtcCntlState, (obj), TYPE_ESP32C2_RTC_CNTL)

#define BIT_SET(reg, bit)   ((reg) & BIT(bit))
#define CLEAR_BIT(reg, bit) do { (reg) &= ~BIT(bit); } while(0)
#define SET_BIT(reg, bit)   do { (reg) |= BIT(bit); } while(0)

#define ESP32C2_RTC_CPU_RESET_GPIO    "cpu-reset"

/**
 * Size of the I/O space for the RTC CTNL.
 */
#define ESP32C2_RTC_CNTL_IO_SIZE    (A_RTC_CNTL_DATE + 4)

typedef enum ESP32C2ResetReason {
    ESP32C2_NO_MEAN                =  0,
    ESP32C2_POWERON_RESET          =  1,    /**<1, Vbat power on reset*/
    ESP32C2_RTC_SW_SYS_RESET       =  3,    /**<3, Software reset digital core*/
    ESP32C2_DEEPSLEEP_RESET        =  5,    /**<5, Deep Sleep reset digital core*/
    ESP32C2_TG0WDT_SYS_RESET       =  7,    /**<7, Timer Group0 Watch dog reset digital core*/
    ESP32C2_TG1WDT_SYS_RESET       =  8,    /**<8, Timer Group1 Watch dog reset digital core*/
    ESP32C2_RTCWDT_SYS_RESET       =  9,    /**<9, RTC Watch dog Reset digital core*/
    ESP32C2_INTRUSION_RESET        = 10,    /**<10, Instrusion tested to reset CPU*/
    ESP32C2_TG0WDT_CPU_RESET       = 11,    /**<11, Time Group0 reset CPU*/
    ESP32C2_RTC_SW_CPU_RESET       = 12,    /**<12, Software reset CPU*/
    ESP32C2_RTCWDT_CPU_RESET       = 13,    /**<13, RTC Watch dog Reset CPU*/
    ESP32C2_RTCWDT_BROWN_OUT_RESET = 15,    /**<15, Reset when the vdd voltage is not stable*/
    ESP32C2_RTCWDT_RTC_RESET       = 16,    /**<16, RTC Watch dog reset digital core and rtc module*/
    ESP32C2_TG1WDT_CPU_RESET       = 17,    /**<17, Time Group1 reset CPU*/
    ESP32C2_SUPER_WDT_RESET        = 18,    /**<18, super watchdog reset digital core and rtc module*/
    ESP32C2_GLITCH_RTC_RESET       = 19,    /**<19, glitch reset digital core and rtc module*/
    ESP32C2_EFUSE_RESET            = 20,    /**<20, efuse reset digital core*/
    ESP32C2_USB_UART_CHIP_RESET    = 21,    /**<21, usb uart reset digital core and rtc module */
    ESP32C2_USB_JTAG_CHIP_RESET    = 22,    /**<22, usb jtag reset digital core and rtc module*/
    ESP32C2_POWER_GLITCH_RESET     = 23,    /**<23, power glitch reset digital core and rtc module*/
    ESP32C2_COUNT_RESET            = 24
} ESP32C2ResetReason;

#define ESP32C2_RTC_CNTL_SCRATCH_REG_COUNT 8

typedef struct ESP32C2RtcCntlState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t options0;
    uint32_t regs[0x200 / 4];
    uint64_t time_latched;
    bool has_booted;
    uint32_t scratch_reg[ESP32C2_RTC_CNTL_SCRATCH_REG_COUNT];

    ESP32C2ResetReason reason;
    /* IRQ used to notify the machine that we need a reset */
    qemu_irq cpu_reset;

} ESP32C2RtcCntlState;


REG32(RTC_CNTL_RTC_OPTIONS0, 0x00)
    FIELD(RTC_CNTL_RTC_OPTIONS0, SW_SYS_RST, 31, 1)
    FIELD(RTC_CNTL_RTC_OPTIONS0, SW_PROCPU_RST, 5, 1)
REG32(RTC_CNTL_RTC_TIME_UPDATE, 0x0c)
REG32(RTC_CNTL_RTC_TIME_LOW0, 0x10)
REG32(RTC_CNTL_RTC_TIME_HIGH0, 0x14)
REG32(RTC_CNTL_RTC_RESET_STATE, 0x30)
REG32(RTC_CNTL_RTC_STORE0, 0x48)
REG32(RTC_CNTL_RTC_STORE1, 0x4c)
REG32(RTC_CNTL_RTC_STORE2, 0x50)
REG32(RTC_CNTL_RTC_STORE3, 0x54)
REG32(RTC_CNTL_RTC_STORE4, 0xac)
REG32(RTC_CNTL_RTC_STORE5, 0xb0)
REG32(RTC_CNTL_RTC_STORE6, 0xb4)
REG32(RTC_CNTL_RTC_STORE7, 0xb8)
REG32(RTC_CNTL_DATE, 0x1fc)
