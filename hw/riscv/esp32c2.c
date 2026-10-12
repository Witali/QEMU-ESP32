/*
 * ESP32-C2 SoC and Small TV machine
 *
 * Copyright (c) 2019-2022 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "hw/qdev-properties.h"
#include "qemu/units.h"
#include "qemu/datadir.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/riscv/riscv_hart.h"
#include "target/riscv/esp_cpu.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/numa.h"
#include "sysemu/device_tree.h"
#include "sysemu/sysemu.h"
#include "sysemu/kvm.h"
#include "sysemu/runstate.h"
#include "sysemu/reset.h"
#include "net/net.h"
#include "elf.h"
#include "hw/misc/esp32c3_reg.h"
#include "hw/misc/esp32c2_rtc_cntl.h"
#include "hw/misc/esp32c2_cache.h"
#include "hw/char/esp32c3_uart.h"
#include "hw/gpio/esp32c3_gpio.h"
#include "hw/nvram/esp32c3_efuse.h"
#include "hw/riscv/esp32c3_clk.h"
#include "hw/riscv/esp32c2_intmatrix.h"
#include "hw/misc/esp32c3_sha.h"
#include "hw/timer/esp32c3_timg.h"
#include "hw/timer/esp32c3_systimer.h"
#include "hw/ssi/esp32c3_spi.h"
#include "hw/misc/esp32c2_rtc_cntl.h"
#include "hw/misc/esp32c3_xts_aes.h"
#include "hw/dma/esp32c2_gdma.h"
#include "hw/ssi/ssi.h"
#include "hw/ssi/esp32c2_gpspi.h"
#include "hw/misc/esp32c3_radio.h"

#define ESP32C2_IO_START_ADDR 0x60000000
#define ESP32C2_IO_WARNING          0

#define ESP32C2_RESET_ADDRESS       0x40000000
#define ESP32C2_RESET_GPIO_NAME     "esp32c2.machine.reset_gpio"
#define MB (1024*1024)


/* Define a new "class" which derivates from "MachineState" */
struct Esp32C2MachineState {
    MachineState parent;

    /* Attributes specific to our class */
    EspRISCVCPU soc;
    BusState periph_bus;
    MemoryRegion iomem;

    qemu_irq cpu_reset;

    ESP32C2IntMatrixState intmatrix;
    ESP32C3UARTState uart[ESP32C3_UART_COUNT];
    ESP32C3GPIOState gpio;
    ESP32C2CacheState cache;
    ESP32C3EfuseState efuse;
    ESP32C3ClockState clock;
    ESP32C2GdmaState gdma;
    ESP32C3ShaState sha;
    ESP32C3XtsAesState xts_aes;
    ESP32C3TimgState timg[1];
    ESP32C3SysTimerState systimer;
    ESP32C3SpiState spi1;
    ESP32C2GpspiState spi2;
    ESP32C2RtcCntlState rtccntl;
    Esp32C3RadioState radio;

    DeviceState *panel;

    uint32_t efuse_regs[0x200 / 4];
    uint32_t ledc_regs[0x200 / 4];
    uint32_t analog_regs[0x100 / 4];
    uint32_t radio_regs[0xe800 / 4];
    uint32_t syscon_wifi_clk_en;
};

/* Fake register used by ESP-IDF application to determine whether the code is running on real hardware or on QEMU */
#define A_SYSCON_ORIGIN_REG     0x3F8
#define A_SYSCON_WIFI_CLK_EN_REG 0x014
#define SYSCON_WIFI_CLK_EN_RESET 0xfffce030
/* Temporary macro for generating a random value from register SYSCON_RND_DATA_REG */
#define A_SYSCON_RND_DATA_REG   0x0B0

/* Temporary macro to mark the CPU as in non-debugging mode */
#define A_ASSIST_DEBUG_CORE_0_DEBUG_MODE_REG    0x098

/* Create a macro which defines the name of our new machine class */
#define TYPE_ESP32C2_MACHINE MACHINE_TYPE_NAME("esp32c2")

/* This will create a macro ESP32_MACHINE, which can be used to check and cast a generic MachineClass
 * to the specific class we defined above: Esp32C2MachineState. */
OBJECT_DECLARE_SIMPLE_TYPE(Esp32C2MachineState, ESP32C2_MACHINE)

/* ESP8684 v2.0 memory map. */
enum MemoryRegions {
    ESP32C2_MEMREGION_IROM,
    ESP32C2_MEMREGION_DROM,
    ESP32C2_MEMREGION_DRAM,
    ESP32C2_MEMREGION_IRAM,
    ESP32C2_MEMREGION_DCACHE,
    ESP32C2_MEMREGION_ICACHE,
};

#define ESP32C2_INTERNAL_SRAM0_SIZE (16*1024)

static const struct MemmapEntry {
    hwaddr base;
    hwaddr size;
} esp32c2_memmap[] = {
    [ESP32C2_MEMREGION_IROM]    = { 0x40000000,  0x90000 },
    [ESP32C2_MEMREGION_DROM]    = { 0x3ff00000,  0x50000 },
    [ESP32C2_MEMREGION_DRAM]    = { 0x3fca0000,  0x40000 },
    /* Merge SRAM0 and SRAM1 into a single entry */
    [ESP32C2_MEMREGION_IRAM]    = { 0x4037c000,  0x40000 + ESP32C2_INTERNAL_SRAM0_SIZE },
    [ESP32C2_MEMREGION_DCACHE]  = { 0x3c000000, 0x400000 },
    [ESP32C2_MEMREGION_ICACHE]  = { 0x42000000, 0x400000 },
};


static bool addr_in_range(hwaddr addr, hwaddr start, hwaddr end)
{
    return addr >= start && addr < end;
}

static uint64_t esp32c2_io_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32C2MachineState *ms = ESP32C2_MACHINE(opaque);
    hwaddr absolute_addr = addr + ESP32C2_IO_START_ADDR;

    if (addr_in_range(absolute_addr, 0x60040000, 0x6004e800)) {
        if (absolute_addr == 0x60047cf8) return 1; /* MAC reset complete. */
        if (absolute_addr == 0x6004d000) {
            return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000; /* Radio microsecond clock. */
        }
        return ms->radio_regs[(absolute_addr - 0x60040000) / 4];
    }
    if (addr_in_range(absolute_addr, 0x6004e800, 0x6004e900)) {
        unsigned offset = absolute_addr - 0x6004e800;
        uint32_t value = ms->analog_regs[offset / 4];
        if ((offset == 0 || offset == 4) && (value & BIT(26))) {
            value = (value & ~0xff0000u) | 0xff0000u; /* Analog calibration/readback compatibility. */
        }
        if (offset == 0x50) value |= 7u << 24; /* RF tone sampling done. */
        if (offset == 0x40 || offset == 0x4c) value |= BIT(24); /* BBPLL calibration done. */
        return (offset == 0 || offset == 4) ? value & ~BIT(25) : value;
    }
    if (addr_in_range(absolute_addr, DR_REG_EFUSE_BASE, DR_REG_EFUSE_BASE + 0x200)) {
        unsigned offset = absolute_addr - DR_REG_EFUSE_BASE;
        if (offset == 0x94 || offset == 0x90) return 0;
        if (offset == 0x44) return (2u << 20) | 0x0200; /* C2 v2.0, eFuse layout v0.0. */
        if (offset == 0x40) return 0x00000002;
        if (offset == 0x04) return 0;
        return ms->efuse_regs[offset / 4];
    } else if (addr_in_range(absolute_addr, DR_REG_LEDC_BASE, DR_REG_LEDC_BASE + 0x200)) {
        return ms->ledc_regs[(absolute_addr - DR_REG_LEDC_BASE) / 4];
    } else if (addr_in_range(addr + ESP32C2_IO_START_ADDR, DR_REG_RTC_I2C_BASE, DR_REG_RTC_I2C_BASE + 0x100)) {
        return (uint32_t) 0xffffff;
    } else if (absolute_addr == DR_REG_SYSCON_BASE +
                                A_SYSCON_WIFI_CLK_EN_REG) {
        return ms->syscon_wifi_clk_en;
    } else if (addr + ESP32C2_IO_START_ADDR == DR_REG_SYSCON_BASE + A_SYSCON_ORIGIN_REG) {
        /* Return "QEMU" as a 32-bit value */
        return 0x51454d55;
    } else if (addr + ESP32C2_IO_START_ADDR == DR_REG_SYSCON_BASE + A_SYSCON_RND_DATA_REG) {
        /* Return a random 32-bit value */
        static bool init = false;
        if (!init) {
            srand(time(NULL));
            init = true;
        }
        return rand();
    } else if (addr + ESP32C2_IO_START_ADDR == DR_REG_ASSIST_DEBUG_BASE + A_ASSIST_DEBUG_CORE_0_DEBUG_MODE_REG) {
        return 0;
    } else {
#if ESP32C2_IO_WARNING
        warn_report("[ESP32-C2] Unsupported read to $%08lx\n", ESP32C2_IO_START_ADDR + addr);
#endif
    }
    return 0;
}

static void esp32c2_io_write(void *opaque, hwaddr addr, uint64_t value, unsigned int size)
{
    Esp32C2MachineState *ms = ESP32C2_MACHINE(opaque);

    hwaddr absolute_addr = addr + ESP32C2_IO_START_ADDR;
    if (addr_in_range(absolute_addr, 0x60040000, 0x6004e800)) {
        ms->radio_regs[(absolute_addr - 0x60040000) / 4] = value;
        return;
    }
    if (addr_in_range(absolute_addr, 0x6004e800, 0x6004e900)) {
        ms->analog_regs[(absolute_addr - 0x6004e800) / 4] = value;
        return;
    }
    if (addr_in_range(absolute_addr, DR_REG_EFUSE_BASE, DR_REG_EFUSE_BASE + 0x200)) {
        unsigned offset = absolute_addr - DR_REG_EFUSE_BASE;
        ms->efuse_regs[offset / 4] = value;
        if (offset == 0x94) ms->efuse_regs[0x98 / 4] |= 1;
        return;
    }
    if (addr_in_range(absolute_addr, DR_REG_LEDC_BASE, DR_REG_LEDC_BASE + 0x200)) {
        unsigned offset = absolute_addr - DR_REG_LEDC_BASE;
        ms->ledc_regs[offset / 4] = value;
        ms->ledc_regs[0x10 / 4] = ms->ledc_regs[0x08 / 4];
        /* Small TV GPIO18: average active-low LEDC channel 0 duty. */
        unsigned bits = ms->ledc_regs[0xa0 / 4] & 15;
        unsigned high = (ms->ledc_regs[0x08 / 4] >> 4);
        unsigned period = 1u << bits;
        unsigned brightness = 1000 - MIN(high, period) * 1000 / period;
        if (!(ms->ledc_regs[0] & BIT(2))) {
            brightness = (ms->ledc_regs[0] & BIT(3)) ? 0 : 1000;
        }
        if (ms->panel) qemu_set_irq(qdev_get_gpio_in_named(ms->panel, "brightness", 0), brightness);
        return;
    }
    if (addr + ESP32C2_IO_START_ADDR == DR_REG_SYSCON_BASE +
                                             A_SYSCON_WIFI_CLK_EN_REG) {
        ms->syscon_wifi_clk_en = value;
        return;
    }
#if ESP32C2_IO_WARNING
        warn_report("[ESP32-C2] Unsupported write $%08lx = %08lx\n", ESP32C2_IO_START_ADDR + addr, value);
#endif
}


/* Define operations for I/OS */
static const MemoryRegionOps esp32c2_io_ops = {
    .read =  esp32c2_io_read,
    .write = esp32c2_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};


/**
 * @brief Callback invoked when SoC's ESP32C2_RESET_GPIO_NAME pin is toggled
 */
static void esp32c2_reset_request(void* opaque, int n, int level)
{
    if (level) {
        ShutdownCause cause = SHUTDOWN_CAUSE_GUEST_RESET;
        qemu_system_reset_request(cause);
    }
}


static void esp32c2_init_spi_flash(Esp32C2MachineState *ms, BlockBackend* blk)
{
    DeviceState *spi_master = DEVICE(&ms->spi1);
    BusState* spi_bus = qdev_get_child_bus(spi_master, "spi");
    const char* flash_model = NULL;
    int64_t image_size = blk_getlength(blk);

    switch (image_size) {
        case 2 * MB:
            flash_model = "w25x16";
            break;
        case 4 * MB:
            flash_model = "gd25q32";
            break;
        default:
            error_report("ESP32-C2 supports 2 or 4 MiB flash images");
            exit(1);
    }

    /* Create the SPI flash model */
    DeviceState *flash_dev = qdev_new(flash_model);
    qdev_prop_set_drive(flash_dev, "drive", blk);
    qdev_prop_set_uint8(flash_dev, "cs", 1);

    /* Realize the SPI flash, its "drive" (blk) property must already be set! */
    qdev_realize(flash_dev, spi_bus, &error_fatal);
    qdev_connect_gpio_out_named(spi_master, SSI_GPIO_CS, 0,
                                qdev_get_gpio_in_named(flash_dev, SSI_GPIO_CS, 0));
}


static void esp32c2_load_firmware(MachineState *machine)
{
    Esp32C2MachineState *ms = ESP32C2_MACHINE(machine);
    g_autofree char *rom = machine->firmware ? g_strdup(machine->firmware) :
        qemu_find_file(QEMU_FILE_TYPE_BIOS, "esp32c2-rev200-rom.bin");
    if (!rom) {
        error_report("ESP32-C2 rev2 ROM missing; use the Small TV preparation tool or -bios ROM.bin");
        exit(1);
    }
    int size = load_image_targphys_as(rom, ESP32C2_RESET_ADDRESS,
                                    0x90000, CPU(&ms->soc)->as);
    if (size != 0x90000) {
        error_report("ESP32-C2 ROM must contain 0x90000 bytes: %s", rom);
        exit(1);
    }
    if (machine->kernel_filename) {
        uint64_t entry = 0;
        int loaded = load_elf_ram_sym(machine->kernel_filename, NULL, NULL, NULL,
                                     &entry, NULL, NULL, NULL, 0,
                                     EM_RISCV, 1, 0, NULL, false, NULL);
        if (loaded < 0) {
            error_report("ESP32-C2 -kernel requires a RISC-V ELF file");
            exit(1);
        }
        qdev_prop_set_uint64(DEVICE(&ms->soc), "resetvec", entry);
    }
}


static void esp32c2_machine_init(MachineState *machine)
{
    /* First thing to do is to check if a drive format and a file ahve been passed through the command line.
     * In fact, we will emulate the SPI flash if `if=mtd` was given. To know this, we will need to use the
     * Global API's function `driver_get`. */
    BlockBackend* blk = NULL;
    DriveInfo *dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        /* MTD was given! We need to initialize and emulate SPI flash */
        qemu_log("Adding SPI flash device\n");
        blk = blk_by_legacy_dinfo(dinfo);
    } else {
        qemu_log("Not initializing SPI Flash\n");
    }

    /* Re-use the macro that checks and casts any generic/parent class to the real child instance */
    Esp32C2MachineState *ms = ESP32C2_MACHINE(machine);

    /* Initialize SoC */
    object_initialize_child(OBJECT(ms), "soc", &ms->soc, TYPE_ESP_RISCV_CPU);
    qdev_prop_set_uint32(DEVICE(&ms->soc), "clock-frequency", 120000000);
    qdev_prop_set_bit(DEVICE(&ms->soc), "c2-fixed-pmp", true);
    qdev_prop_set_uint64(DEVICE(&ms->soc), "resetvec", ESP32C2_RESET_ADDRESS);

    /* Initialize the memory mapping */
    const struct MemmapEntry *memmap = esp32c2_memmap;
    MemoryRegion *sys_mem = get_system_memory();

    /* Initialize the IROM */
    MemoryRegion *irom = g_new(MemoryRegion, 1);
    memory_region_init_rom(irom, NULL, "esp32c2.irom", memmap[ESP32C2_MEMREGION_IROM].size, &error_fatal);
    memory_region_add_subregion(sys_mem, memmap[ESP32C2_MEMREGION_IROM].base, irom);

    /* Initialize the DROM as an alias to IROM. */
    MemoryRegion *drom = g_new(MemoryRegion, 1);
    const hwaddr offset_in_orig = 0x40000;
    memory_region_init_alias(drom, NULL, "esp32c2.drom", irom, offset_in_orig, memmap[ESP32C2_MEMREGION_DROM].size);
    memory_region_add_subregion(sys_mem, memmap[ESP32C2_MEMREGION_DROM].base, drom);

    /* Initialize the IRAM */
    MemoryRegion *iram = g_new(MemoryRegion, 1);
    memory_region_init_ram(iram, NULL, "esp32c2.iram", memmap[ESP32C2_MEMREGION_IRAM].size, &error_fatal);
    memory_region_add_subregion(sys_mem, memmap[ESP32C2_MEMREGION_IRAM].base, iram);

    /* Initialize DRAM as an alias to IRAM (not including Internal SRAM 0) */
    MemoryRegion *dram = g_new(MemoryRegion, 1);
    /* DRAM mirrors IRAM for SRAM 1, skip the SRAM 0 area */
    memory_region_init_alias(dram, NULL, "esp32c2.dram", iram,
                             ESP32C2_INTERNAL_SRAM0_SIZE, memmap[ESP32C2_MEMREGION_DRAM].size);
    memory_region_add_subregion(sys_mem, memmap[ESP32C2_MEMREGION_DRAM].base, dram);

    esp32c2_load_firmware(machine);

    qdev_realize(DEVICE(&ms->soc), NULL, &error_fatal);

    ms->syscon_wifi_clk_en = SYSCON_WIFI_CLK_EN_RESET;
    memory_region_init_io(&ms->iomem, OBJECT(&ms->soc), &esp32c2_io_ops,
                          ms, "esp32c2.iomem", 0xd1000);
    memory_region_add_subregion(sys_mem, ESP32C2_IO_START_ADDR, &ms->iomem);


    /* Initialize the peripheral bus */
    qbus_init(&ms->periph_bus, sizeof(ms->periph_bus),
              TYPE_SYSTEM_BUS, DEVICE(&ms->soc), "esp32c2-periph-bus");

    /* Initialize the main I/O of the CPU that waits for "reset" requests */
    qdev_init_gpio_in_named(DEVICE(&ms->soc), esp32c2_reset_request, ESP32C2_RESET_GPIO_NAME, 1);

    /* Initialize the I/O peripherals */
    for (int i = 0; i < ESP32C3_UART_COUNT; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "uart%d", i);
        object_initialize_child(OBJECT(machine), name, &ms->uart[i], TYPE_ESP32C3_UART);

        snprintf(name, sizeof(name), "serial%d", i);
        object_property_add_alias(OBJECT(machine), name, OBJECT(&ms->uart[i]), "chardev");
        qdev_prop_set_chr(DEVICE(&ms->uart[i]), "chardev", serial_hd(i));
    }

    object_initialize_child(OBJECT(machine), "intmatrix", &ms->intmatrix, TYPE_ESP32C2_INTMATRIX);
    object_initialize_child(OBJECT(machine), "gpio", &ms->gpio, TYPE_ESP32C3_GPIO);
    object_initialize_child(OBJECT(machine), "extmem", &ms->cache, TYPE_ESP32C2_CACHE);
    object_initialize_child(OBJECT(machine), "efuse", &ms->efuse, TYPE_ESP32C3_EFUSE);
    object_initialize_child(OBJECT(machine), "clock", &ms->clock, TYPE_ESP32C3_CLOCK);
    object_initialize_child(OBJECT(machine), "sha", &ms->sha, TYPE_ESP32C3_SHA);
    object_initialize_child(OBJECT(machine), "gdma", &ms->gdma, TYPE_ESP32C2_GDMA);
    object_initialize_child(OBJECT(machine), "xts_aes", &ms->xts_aes, TYPE_ESP32C3_XTS_AES);
    object_initialize_child(OBJECT(machine), "timg0", &ms->timg[0], TYPE_ESP32C3_TIMG);
    object_initialize_child(OBJECT(machine), "systimer", &ms->systimer, TYPE_ESP32C3_SYSTIMER);
    object_initialize_child(OBJECT(machine), "spi1", &ms->spi1, TYPE_ESP32C3_SPI);
    object_initialize_child(OBJECT(machine), "rtccntl", &ms->rtccntl, TYPE_ESP32C2_RTC_CNTL);
    object_initialize_child(OBJECT(machine), "radio", &ms->radio,
                            TYPE_ESP32C3_RADIO);

    qdev_prop_set_uint32(DEVICE(&ms->clock), "xtal-mhz", 26);
    qdev_prop_set_uint32(DEVICE(&ms->clock), "period-sel", 1);
    qdev_prop_set_bit(DEVICE(&ms->clock), "writable-clocks", true);
    qdev_prop_set_uint32(DEVICE(&ms->systimer), "clock-frequency", 10400000);
    qdev_prop_set_uint32(DEVICE(&ms->timg[0]), "xtal-frequency", 26000000);
    qdev_prop_set_uint32(DEVICE(&ms->timg[0]), "apb-frequency", 40000000);

    /* Realize all the I/O peripherals we depend on */

    /* Interrupt matrix realization */
    DeviceState* intmatrix_dev = DEVICE(&ms->intmatrix);
    {
        /* Store the current Machine CPU in the interrupt matrix */
        object_property_set_link(OBJECT(&ms->intmatrix), "cpu", OBJECT(&ms->soc), &error_abort);
        sysbus_realize(SYS_BUS_DEVICE(&ms->intmatrix), &error_fatal);
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ms->intmatrix), 0);
        memory_region_add_subregion_overlap(sys_mem, DR_REG_INTERRUPT_BASE, mr, 0);

        /* Connect all the interrupt matrix 31 output lines to the CPU 31 input IRQ lines.
         * The lines are indexed starting at 1.
         */
        for (int i = 0; i <= ESP32C2_CPU_INT_COUNT; i++) {
            qemu_irq cpu_input = qdev_get_gpio_in_named(DEVICE(&ms->soc), ESP_CPU_IRQ_LINES_NAME, i);
            qdev_connect_gpio_out_named(intmatrix_dev, ESP32C2_INT_MATRIX_OUTPUT_NAME, i, cpu_input);
        }
    }

    /* PHY startup compatibility; C2 packet DMA is not implemented. */
    {
        static const hwaddr base[ESP32C3_RADIO_REGION_COUNT] = {
            [ESP32C3_RADIO_REGION_ANA] = DR_REG_RTC_I2C_BASE,
            [ESP32C3_RADIO_REGION_PHYA] = 0x6004a000,
            [ESP32C3_RADIO_REGION_FE] = 0x60044000,
            [ESP32C3_RADIO_REGION_PWR] = DR_REG_PWR_MANAGER_BASE,
        };
        int i;

        sysbus_realize(SYS_BUS_DEVICE(&ms->radio), &error_fatal);
        for (i = 0; i < ESP32C3_RADIO_REGION_COUNT; i++) {
            MemoryRegion *mr = sysbus_mmio_get_region(
                SYS_BUS_DEVICE(&ms->radio), i);

            memory_region_add_subregion_overlap(sys_mem, base[i], mr, 0);
        }
    }

    /* RTC CNTL realization */
    {
        sysbus_realize(SYS_BUS_DEVICE(&ms->rtccntl), &error_fatal);
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ms->rtccntl), 0);
        memory_region_add_subregion_overlap(sys_mem, DR_REG_RTCCNTL_BASE, mr, 0);
        /* Connect CNTL's reset-request GPIO to the SoC's reset GPIO */
        qdev_connect_gpio_out_named(DEVICE(&ms->rtccntl), ESP32C2_RTC_CPU_RESET_GPIO, 0,
                                    qdev_get_gpio_in_named(DEVICE(&ms->soc), ESP32C2_RESET_GPIO_NAME, 0));
    }

    /* SPI1 controller (SPI Flash) */
    {
        ms->spi1.xts_aes = &ms->xts_aes;
        sysbus_realize(SYS_BUS_DEVICE(&ms->spi1), &error_fatal);
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ms->spi1), 0);
        memory_region_add_subregion_overlap(sys_mem, DR_REG_SPI1_BASE, mr, 0);
        if (blk) {
            esp32c2_init_spi_flash(ms, blk);
        }
    }

    for (int i = 0; i < ESP32C3_UART_COUNT; ++i) {
        const hwaddr uart_base[] = { DR_REG_UART_BASE, DR_REG_UART1_BASE };
        sysbus_realize(SYS_BUS_DEVICE(&ms->uart[i]), &error_fatal);
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ms->uart[i]), 0);
        memory_region_add_subregion_overlap(sys_mem, uart_base[i], mr, 0);
        sysbus_connect_irq(SYS_BUS_DEVICE(&ms->uart[i]), 0,
                           qdev_get_gpio_in(intmatrix_dev, ETS_UART0_INTR_SOURCE + i));
    }

    /* GPIO realization */
    {
        sysbus_realize(SYS_BUS_DEVICE(&ms->gpio), &error_fatal);
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ms->gpio), 0);
        memory_region_add_subregion_overlap(sys_mem, DR_REG_GPIO_BASE, mr, 0);
    }

    /* (Extmem) Cache realization */
    {
        if (blk) {
            ms->cache.flash_blk = blk;
        }
        ms->cache.xts_aes = &ms->xts_aes;
        sysbus_realize(SYS_BUS_DEVICE(&ms->cache), &error_fatal);
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ms->cache), 0);
        memory_region_add_subregion_overlap(sys_mem, DR_REG_EXTMEM_BASE, mr, 0);

        memory_region_add_subregion_overlap(sys_mem, ms->cache.dcache_base, &ms->cache.dcache, 0);
        memory_region_add_subregion_overlap(sys_mem, ms->cache.icache_base, &ms->cache.icache, 0);
    }

    /* System clock realization */
    {
        sysbus_realize(SYS_BUS_DEVICE(&ms->clock), &error_fatal);
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ms->clock), 0);
        memory_region_add_subregion_overlap(sys_mem, DR_REG_SYSTEM_BASE, mr, 0);
        /* Connect the IRQ lines to the interrupt matrix */
        for (int i = 0; i < ESP32C3_SYSTEM_CPU_INTR_COUNT; i++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(&ms->clock), i,
                           qdev_get_gpio_in(intmatrix_dev, ETS_FROM_CPU_INTR0_SOURCE + i));
        }
    }

    /* Timer Groups realization */
    {
        sysbus_realize(SYS_BUS_DEVICE(&ms->timg[0]), &error_fatal);
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ms->timg[0]), 0);
        memory_region_add_subregion_overlap(sys_mem, DR_REG_TIMERGROUP0_BASE, mr, 0);
        /* Connect the T0 interrupt line to the interrupt matrix */
        qdev_connect_gpio_out_named(DEVICE(&ms->timg[0]), ESP32C3_T0_IRQ_INTERRUPT, 0,
                                    qdev_get_gpio_in(intmatrix_dev, ETS_TG0_T0_LEVEL_INTR_SOURCE));
        /* Connect the Watchdog interrupt line to the interrupt matrix */
        qdev_connect_gpio_out_named(DEVICE(&ms->timg[0]), ESP32C3_WDT_IRQ_INTERRUPT, 0,
                                    qdev_get_gpio_in(intmatrix_dev, ETS_TG0_WDT_LEVEL_INTR_SOURCE));
        /* Connect the Watchdog reset request to the CNTL's WDT0 line */
        qdev_connect_gpio_out_named(DEVICE(&ms->timg[0]), ESP32C3_WDT_IRQ_RESET, 0,
                                    qdev_get_gpio_in(DEVICE(&ms->rtccntl), ESP32C2_TG0WDT_SYS_RESET));

    }
    /* System timer */
    {
        sysbus_realize(SYS_BUS_DEVICE(&ms->systimer), &error_fatal);
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ms->systimer), 0);
        memory_region_add_subregion_overlap(sys_mem, DR_REG_SYSTIMER_BASE, mr, 0);
        for (int i = 0; i < ESP_SYSTIMER_IRQ_COUNT; i++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(&ms->systimer), i,
                           qdev_get_gpio_in(intmatrix_dev, ETS_SYSTIMER_TARGET0_EDGE_INTR_SOURCE + i));
        }
    }

    /* GDMA Realization */
    {
        object_property_set_link(OBJECT(&ms->gdma), "soc_mr", OBJECT(dram), &error_abort);
        sysbus_realize(SYS_BUS_DEVICE(&ms->gdma), &error_fatal);
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ms->gdma), 0);
        memory_region_add_subregion_overlap(sys_mem, DR_REG_GDMA_BASE, mr, 0);
        /* On the ESP32-C3, both IN and OUT channels are connected to the same Connect the IRQs to the Interrupt Matrix */
        for (int i = 0; i < ESP32C2_GDMA_CHANNEL_COUNT; i++) {
            qdev_connect_gpio_out_named(DEVICE(&ms->gdma), ESP_GDMA_IRQ_IN_NAME, i,
                                        qdev_get_gpio_in(intmatrix_dev, ETS_DMA_CH0_INTR_SOURCE + i));
            qdev_connect_gpio_out_named(DEVICE(&ms->gdma), ESP_GDMA_IRQ_OUT_NAME, i,
                                        qdev_get_gpio_in(intmatrix_dev, ETS_DMA_CH0_INTR_SOURCE + i));
        }

    }

    /* Internal unencrypted-flash helper, not a C3 eFuse MMIO bank. */
    sysbus_realize(SYS_BUS_DEVICE(&ms->efuse), &error_fatal);

    /* SHA realization */
    {
        ms->sha.parent.gdma = ESP_GDMA(&ms->gdma);
        sysbus_realize(SYS_BUS_DEVICE(&ms->sha), &error_fatal);
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ms->sha), 0);
        memory_region_add_subregion_overlap(sys_mem, DR_REG_SHA_BASE, mr, 0);
        sysbus_connect_irq(SYS_BUS_DEVICE(&ms->sha), 0,
                           qdev_get_gpio_in(intmatrix_dev, ETS_SHA_INTR_SOURCE));
    }

    /* XTS-AES realization */
    {
        ms->xts_aes.efuse = ESP_EFUSE(&ms->efuse);
        ms->xts_aes.clock = &ms->clock;
        qdev_realize(DEVICE(&ms->xts_aes), &ms->periph_bus, &error_fatal);
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ms->xts_aes), 0);
        memory_region_add_subregion_overlap(sys_mem, DR_REG_AES_XTS_BASE, mr, 0);
    }

    /* Small TV: hardware SPI2, GPIO5 D/C, GPIO1 reset, active-low backlight. */
    object_initialize_child(OBJECT(machine), "spi2", &ms->spi2, TYPE_ESP32C2_GPSPI);
    ms->spi2.gdma = ESP_GDMA(&ms->gdma);
    sysbus_realize(SYS_BUS_DEVICE(&ms->spi2), &error_fatal);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_SPI2_BASE,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&ms->spi2), 0), 0);
    sysbus_connect_irq(SYS_BUS_DEVICE(&ms->spi2), 0,
        qdev_get_gpio_in(intmatrix_dev, ETS_SPI2_INTR_SOURCE));
    DeviceState *panel = ms->panel = qdev_new("st7789");
    qdev_prop_set_uint32(panel, "visible-width", 240);
    qdev_prop_set_bit(panel, "controls", false);
    qdev_prop_set_bit(panel, "panel-inverted", true);
    ssi_realize_and_unref(panel, ms->spi2.bus, &error_fatal);
    qemu_set_irq(qdev_get_gpio_in_named(panel, SSI_GPIO_CS, 0), 0); /* CS=-1: always selected. */
    qemu_set_irq(qdev_get_gpio_in_named(panel, "backlight", 0), 1);
    qdev_connect_gpio_out_named(DEVICE(&ms->gpio), "gpio-out", 5,
        qdev_get_gpio_in_named(panel, "dc", 0));
    qdev_connect_gpio_out_named(DEVICE(&ms->gpio), "gpio-out", 1,
        qdev_get_gpio_in_named(panel, "reset-pin", 0));
}


/* Initialize machine type */
static void esp32c2_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "Espressif ESP32-C2 Small TV (experimental)";
    mc->default_cpu_type = TYPE_ESP_RISCV_CPU;
    mc->init = esp32c2_machine_init;
    mc->max_cpus = 1;
    mc->default_cpus = 1;
    mc->default_ram_size = 272 * 1024;
}

/* Create a new type of machine ("child class") */
static const TypeInfo esp32c2_info = {
    .name = TYPE_ESP32C2_MACHINE,
    /* Specify the parent class, i.e. the class we derivate from */
    .parent = TYPE_MACHINE,
    /* Real size in bytes of our machine instance */
    .instance_size = sizeof(Esp32C2MachineState),
    /* Override the init function to one we defined above */
    .class_init = esp32c2_machine_class_init,
};

static void esp32c2_machine_type_init(void)
{
    type_register_static(&esp32c2_info);
}

type_init(esp32c2_machine_type_init);
