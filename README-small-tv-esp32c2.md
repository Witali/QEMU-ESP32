# Small TV ESP32-C2 QEMU machine

The experimental `esp32c2` machine runs the unchanged factory Small TV dump and
our reconstructed ESP32-C2 application with a 240×240 ST7789 display. This is an
offline display and application-startup simulator. The factory firmware first draws **Connecting to WiFi / Ver:0.0.2**, then
shows the **Small_TV / 192.168.43.1** setup instructions; our firmware starts its
clock UI and animation, with **Waiting for time sync**.

C2 Wi-Fi packet transport is not implemented. Its guest Wi-Fi driver and AP
startup execute, but the host cannot open `192.168.43.1` through this machine.
SNTP, live weather and HTTP traffic have not been tested here. The C3 virtual AP
and NAT backend cannot be reused unchanged: C2 uses different MAC/DMA registers.

## Build and run on Windows

```powershell
Set-Location C:\Work\QEMU-ESP32
.\setup-qemu-esp32c2-windows.ps1
.\run-small-tv-esp32c2.ps1 -Firmware original
.\run-small-tv-esp32c2.ps1 -Firmware reconstructed
```

The setup script shares the existing MinGW64/MSYS2 `riscv32-softmmu` installer
with C3 and defaults to a separate `build-esp32c2` directory. To install an
already-built executable, use `-BuildDirectory build-riscv32 -SkipBuild`.
The executable is installed under `bin/`. C3 remains available in that binary.

The launcher defaults to the sibling `yoRadio` repository. It locates the genuine
`esp32c2_rev200_rom.elf` in that repository's ESP-IDF `esp-rom-elfs` package.
For another layout, pass `-YoRadioRepository`, `-RomElf`, `-PythonExecutable`
and/or `-QemuExecutable`. The Python helper needs only Python's standard library.
Obtain the ROM ELF through ESP-IDF; this repository does not redistribute it.

Each variant has its own state directory under `.cache/small-tv-c2/`. Preparation
validates source hashes and C2 image headers, converts the real ROM ELF's physical
load segments into a ROM binary, and creates a 4 MiB working flash image.
Factory flash is copied byte for byte; reconstructed bootloader, partition table,
app and assets are placed at their archived offsets. NVS starts erased for the
reconstructed image. Neither application's instructions are patched, and there
are no PC-specific function replacements or synthetic network replies.

Default `-snapshot` execution discards guest flash writes when QEMU exits.
`-Persistent` instead retains writes in the working copy, including NVS settings.
It never writes the source dump or firmware archive. A persistent copy from
another firmware build requires a new `-StateDirectory`.

## Control and diagnostics

```powershell
.\run-small-tv-esp32c2.ps1 -Firmware reconstructed -Action status
.\run-small-tv-esp32c2.ps1 -Firmware reconstructed -Action screenshot
.\run-small-tv-esp32c2.ps1 -Firmware reconstructed -Action reset
.\run-small-tv-esp32c2.ps1 -Firmware reconstructed -Action stop
```

Repeat these commands with `-Firmware original` for the factory VM. SDL shows the
LCD; `-Display none` runs headless. Screenshots are PNG files encoded from QEMU's
PPM framebuffer, so they also work in builds without libpng. `uart.log`,
`qemu.log`, `prepared.json` and `session.json` stay in the selected state directory.
QMP binds to a dynamically selected localhost port and checks the VM's unique
name before executing a control command.

For source-level debugging, start with `-Paused -GdbPort 3333` and connect
`riscv32-esp-elf-gdb` to `127.0.0.1:3333`. The reconstructed application ELF is in
`yoRadio/.build/small-tv-esp32c2/small_tv_reconstructed.elf`. GDB is separate from
the simulated UART; no serial device or physical board is accessed.

```powershell
.\test-small-tv-esp32c2.ps1
```

This runs both firmware images for 30 seconds each, captures the LCD, checks for
panics/watchdogs, resets each VM and verifies a second boot. The reconstruction
also must complete `app_main`, start its AP DHCP server and update its animation.
Hashes verify that the working base flash and source artifacts were unchanged.
The test stops its own VMs and saves separate JSON reports and screenshots in a
timestamped directory. To shorten a smoke run, use `-TestSeconds 10`.
Stop any VM that uses an explicitly supplied test state directory first.

## Modeled hardware

`configs/devices/esp32c2-small-tv.json` describes the profile. Its fixed wiring is
implemented by `hw/riscv/esp32c2.c`; changing the JSON GPIO labels does not rewire
the compiled machine.

| Component | Model |
|---|---|
| CPU | One RV32IMC core, 120 MHz cycle counter; C2 fixed PMPADDR3–15 |
| ROM / SRAM | 576 KiB genuine ROM; 16 KiB cache SRAM + 256 KiB SRAM, with correct IRAM/DRAM aliases |
| Flash / MMU | 4 MiB Small TV flash; 64 MMU entries, C2 invalid bit, selectable 16/32/64 KiB pages; 64 KiB verified by both images |
| Interrupts | 43 C2 sources and the C2 enable/priority/threshold register layout |
| Timers | SYSTIMER 10.4 MHz, APB 40 MHz, XTAL 26 MHz; RTC scratch/time and reset registers |
| DMA / SPI | One GDMA channel pair; SPI1 flash access; SPI2 FIFO and DMA data transfers |
| LCD | ST7789, RGB565, 240×240; D/C GPIO5, Reset GPIO1; SCK4/MOSI6 described by the direct SSI bus |
| Backlight | GPIO18's active-low LEDC channel 0 duty represented by average screen brightness |
| eFuse | Synthetic C2 v2.0 identity and local MAC `02:00:00:00:00:02`; no owner's device identity copied |
| PHY startup | Register readback and synchronous completion flags for analog calibration and MAC reset |

The LCD is always selected because this firmware uses `CS=-1`; its physical CS
wiring has not been established. SPI GPIO routing and waveforms, panel scanout,
RF measurements, Bluetooth, SD cards, ADC, low-power modes, power consumption,
secure boot and encrypted flash are not validated by this model. The average
backlight mapping is based on the firmware, not a new physical polarity test.
Flash snapshots are supported; full CPU/device checkpoints and live migration
are not supported. Compatible C3 UART/GPIO/SPI-flash/SHA/timer building blocks
are reused while C2 memory, PMP, interrupts, MMU, RTC and GDMA are separate.

## Verification

Both unchanged C2 images passed 30-second offline runs and reset/reboot checks on
2026-10-12. The reconstructed UI rendered and animated; the factory image progressed from its connection screen to Wi-Fi setup
instructions. A separate C3 production-image regression run
rendered its existing OLED radio UI. The C3 production build disables UART
application logging, so its OLED frame is the startup evidence.

The firmware repository contains screenshots, UART logs and machine-readable
reports under `yoRadio/Small_TV/qemu/`. These are emulator results, not a physical
board test or confirmation of networking. Hardware specifications are cross-
checked against Espressif's [ESP8684 datasheet](https://documentation.espressif.com/esp8684_datasheet_en.html)
and the target-specific headers in the pinned ESP-IDF source archive.
