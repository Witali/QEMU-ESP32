# Composable ESP board peripherals

Board wiring lives in `configs/boards/*.json`. A profile instantiates native
QEMU devices on the SoC's existing SPI/I2C controllers. SPI transactions reach a
module only when the guest GPIO matrix or IO_MUX connects the corresponding
SCK/MOSI signals to its pins and its CS is asserted. MISO requires the matching
input route. I2C modules check SCL/SDA output routes before acknowledging.
Changing the profile requires corresponding guest firmware pin assignments.

The historical machines remain available:

- `-M esp32,st7789=on,sdspi=on` retains CYD GPIO CS15/D/C2/backlight21 and SD CS5,
  its controls, UART keyboard integration, and the original direct attachments.
- `-M esp32c3` retains the original 72x40 SSD1306 attachment and legacy GPIO behavior.
- `-M esp32c2` retains the Small TV default panel and wiring.

The profile compiler explicitly enables modular mode: `esp32,modular=on` or
`esp32c2/esp32c3,default-peripherals=off`. GPIO routing checks apply to those
profiles. Devices are composed at startup; live hotplug is unsupported.

## Available buses and modules

| SoC | External hardware SPI | I2C |
|---|---|---|
| ESP32-C2 | SPI2 | I2C0, 16-byte FIFO |
| ESP32-C3 | SPI2 | I2C0, 32-byte FIFO |
| ESP32 | SPI2 (HSPI), SPI3 (VSPI) | I2C0, I2C1 |

SPI0/SPI1 belong to flash/cache and cannot be selected as external buses.
The compiler rejects nonexistent buses, flash-reserved GPIOs, conflicting pin
roles, duplicate IDs, and duplicate I2C addresses. Original ESP32 input-only
GPIO34–39 are accepted for MISO, not outputs.

`esp-spi-module` wraps any compiled SSI peripheral. The supplied examples use
ST7789 and `sd` (an SSI SD adapter plus an SD card). `esp-i2c-module` wraps a
compiled I2C slave; the OLED examples use SSD1306. Child `properties` are checked
by QEMU's existing device model. Adding a controller model still requires its
QEMU implementation; a JSON name alone does not emulate new hardware.

SPI nodes use `sck`, `mosi`, optional `miso`, `cs`, `dc`, `reset`, `backlight`.
`cs: -1` means physically always selected and is allowed only on an exclusive
bus. Devices sharing SPI require separate CS GPIOs; SCK/MOSI/MISO may be shared.
CS may be driven by ordinary GPIO or routed hardware CS. D/C and reset connect
to the child model's named inputs. ST7789 brightness accepts GPIO levels and
the modeled C2 LEDC average duty, with `backlight_active_low` when needed.
`wired-bytes` and `dropped-bytes` are read-only QOM diagnostics on SPI sockets.

I2C nodes use `scl`, `sda`, and a 7-bit `address`. Relative SD `image` paths are
resolved relative to the profile file. SD backends always use `snapshot=on`;
guest sector writes work but do not change the backing image.

## Example profiles

| Profile | Wiring |
|---|---|
| `small-tv-c2.json` | Stock ST7789: SPI2 SCK4/MOSI6/D-C5/reset1/backlight18, CS=-1 |
| `small-tv-c2-sd.json` | SPI2 shared by LCD CS3 and SD CS10/MISO2 |
| `c3-oled.json` | SSD1306 72x40 at 0x3c, SDA5/SCL6 |
| `esp32-dual-spi.json` | ST7789 on SPI2 14/13/CS15; SD on SPI3 18/23/19/CS5 |

The stock Small TV firmware does **not** drive LCD CS3 or use an SD card. To
run the shared profile, modify the firmware and physical LCD CS wiring.

```powershell
# From C:\Work\QEMU-ESP32; Python may also be supplied explicitly.
.\run-small-tv-esp32c2.ps1 -Firmware reconstructed `
  -HardwareProfile .\configs\boards\small-tv-c2.json
.\test-small-tv-esp32c2.ps1 -TestSeconds 60 `
  -HardwareProfile .\configs\boards\small-tv-c2.json

# Generic launcher: flash is a complete image, SD uses its own snapshot.
.\run-esp-board.ps1 -HardwareProfile .\configs\boards\c3-oled.json `
  -FlashImage C:\path\c3-flash.bin -Display sdl
```

For SD examples, create `.cache/esp-boards/sd.img` or set `image` to an existing
raw SD image. For generic C2 launches, pass `-RomDirectory` with a genuine
`esp32c2-rev200-rom.bin`. `run-small-tv-esp32c2.ps1 -Action prepare` creates this
ROM from the official ESP-IDF ELF. C2 clock guests need the fixed `icount`
arguments and synthetic network settings supplied by the Small TV launcher;
the generic launcher defaults to no network. No physical serial port is used.

## Regression checks

```powershell
python tools/test_esp_modules.py `
  --c2-rom-dir .cache/small-tv-c2/clock-original
```

This starts paused, headless VMs and drives real controller MMIO through QTest.
It checks shared LCD/SD on C2 and C3, separate SPI2/SPI3 on ESP32, hardware CS
isolation, wrong SCK/MISO rejection, command/address phases, SD initialization
and 512-byte sector write/read, snapshot isolation, and OLED on alternate
GPIO8/9 with NACK for an incorrect SCL route. It also tests the historical CYD
SPI LCD/SD attachments and SD idle-high behavior. Pass `--qemu-riscv` and
`--qemu-xtensa` to test build executables before installation. Its state directory
contains disposable test disks, flash images, QTest logs, and a JSON report;
all test VMs are stopped.

These are transaction-level controller models. SPI bit banging, electrical
timing, signal integrity, arbitrary peripheral IRQ wiring, power consumption,
RF, and full machine migration are outside their scope. The unchanged CYD/C3
guest boot regressions and Small TV NTP tests are recorded in the sibling
yoRadio repository under `Small_TV/qemu/`.

To repeat guest boot checks with the unchanged existing images:

```powershell
python tools/test_esp_legacy.py `
  --cyd-build ..\yoRadio\idf\esp32-cyd2usb-minimal\build `
  --c3-build ..\yoRadio\idf\esp32c3-oled-native\build-production `
  --c3-profile configs/boards/c3-oled.json
```

The CYD snapshot used here has no provisioned NVS. Its existing NVS/SPIFFS,
Wi-Fi initialization and DAC warnings remain possible in this isolated boot
test. The minimal image has a black LCD area in both the previous and new
binaries; console controls contribute its nonzero pixels. Boot/console
comparison and separate LCD/SD MMIO checks establish compatibility of those
paths; this is not a successful CYD application-screen or radio-playback test.
To compare screenshots with a previous binary, add `--baseline-xtensa` before
installing the replacement. Network/audio validation remains separate.
