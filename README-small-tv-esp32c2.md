# Small TV ESP32-C2 QEMU machine

The experimental `esp32c2` machine runs the factory Small TV application and
our reconstructed application with a 240×240 ST7789 display. The default `clock`
network profile emulates an open **yoRadio-QEMU** access point, Wi-Fi packet DMA,
association, DHCP, ARP, DNS and UDP NTP. Both applications receive time from a
virtual server using the computer's UTC clock. Guest instructions are unchanged;
only settings in a disposable flash copy are replaced for the virtual network.

The profile is isolated with SLIRP `restrict=on`. DNS maps the three firmware
time-server names to the current virtual subnet's `.2` service address; other names receive NXDOMAIN. It provides no
Internet weather data. The original firmware may display weather lookup errors;
both seeded fixtures use the synthetic `disabled` key. `-Network offline` retains the
earlier boot/display-only mode, with the factory setup screen and unsynchronized
reconstructed clock. This is a packet-level Wi-Fi model, not RF simulation.

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
and/or `-QemuExecutable`. The VM controller and fixture generator use Python's standard library.
Obtain the ROM ELF through ESP-IDF; this repository does not redistribute it.

Each network profile and variant has its own state directory under `.cache/small-tv-c2/`. Preparation
validates source hashes and C2 image headers, converts the real ROM ELF's physical
load segments into a ROM binary, and creates a 4 MiB working flash image.
The `offline` profile copies factory flash byte for byte. The `clock` profile
erases the working NVS region `0x9000..0xefff`, removing hardware calibration and
stored owner credentials. Both applications import a synthetic raw settings sector at `0x3e7000`:
`clock` starts with saved open-network credentials, while `setup` starts with
no SSID and setup mode. No owner weather key is copied. Application/bootloader instructions
and display assets remain unchanged. There are no guest PC/function patches or
direct time writes: SNTP receives regular UDP packets and sets the RTC offset.

Default `-snapshot` execution discards guest flash writes when QEMU exits.
`-Persistent` instead retains writes in the working copy, including NVS settings.
It never writes the source dump or firmware archive. A persistent copy from
another firmware build or network profile requires a new `-StateDirectory`.

## Localhost web access and initial Wi-Fi setup

HTTP connections are forwarded by SLIRP to the real guest TCP/IP stack and
firmware HTTP handlers. No Python replacement website or host-side guest IP
assignment is needed. Listeners bind only to `127.0.0.1`; the network remains
restricted. `session.json`, launch output and `-Action status` list `web_urls`
and the complete `web_endpoints` mapping.

```powershell
.\run-small-tv-esp32c2.ps1 -Network setup -Firmware reconstructed `
    -HttpBasePort 18080 -Persistent -Display none
```

| Host URL | Guest service |
|---|---|
| `http://127.0.0.1:18080` | Initial setup AP (`192.168.43.1:80`) |
| `http://127.0.0.1:18081` | Station after configuration (`192.168.43.15:80`) |

Open the setup URL, scan/select **yoRadio-QEMU**, leave its password empty,
and save. The firmware writes its own settings and reboots. Open the station
URL after it receives DHCP. `-Persistent` keeps these preferences for a later
launch. Repeat control commands with `-Network setup` or the same explicit
`-StateDirectory`. Both ports remain distinct; their guest IPs are implementation
details for browser users.

`-HttpBasePort 0` (default) chooses the first free consecutive block at/above
18080. Multiple VMs receive separate blocks; an explicitly requested busy block
fails instead of being silently redirected. `-HttpBasePort -1` disables HTTP
forwarding. The usual `-Network clock` profile exposes its station at one port
(`10.0.2.15:80`); `offline` has no network listener.

The setup backend uses `192.168.43.0/24` so the guest AP and its virtual client
share a subnet, and it retains that subnet through AP -> STA reboot. NTP/DNS
services follow this subnet. The opt-in `esp32c3-wifi.softap-client-ssid=Small_TV`
property makes the virtual client use a consistent MAC through association and
DHCP, and allows the guest's idle STA interface to scan other virtual APs.
Default C3 behavior is unchanged. C2 RX LAST tracks the completed DMA descriptor,
separately from NEXT, including when the chain needs replenishment during scan.

```powershell
& C:\Work\yoRadio\.idf\python-3.12.10\tools\python.exe `
    .\tools\test_small_tv_network.py
& C:\Work\yoRadio\.idf\python-3.12.10\tools\python.exe `
    .\tools\small_tv_http_test.py --output C:\Work\yoRadio\.build\small-tv-http-tests
```

Integration checks cover real native scan, setup HTML, configure POST, AP -> STA,
DHCP/SNTP, settings persistence after relaunch/reset and two simultaneous station
VMs with isolated HTTP settings. The original's setup flow uses manual SSID entry:
its `/scan` dereferences `malloc(0)` when no results exist (`0x4200f484`), so that
vendor bug is explicitly excluded, not marked as a passing scan. The reconstructed
handler safely returns `[]` for zero results. Factory JSON requests use its own
browser's compact JSON.stringify format; its naive `/configure` parser does not
recognize spaces after `ssid`/`password` colons. The native JSON parser handles them.
This is an open synthetic network test, not RF/WPA encryption or browser-JS coverage.

## Control and diagnostics

```powershell
.\run-small-tv-esp32c2.ps1 -Firmware reconstructed -Action status
.\run-small-tv-esp32c2.ps1 -Firmware reconstructed -Action screenshot
.\run-small-tv-esp32c2.ps1 -Firmware reconstructed -Action reset
.\run-small-tv-esp32c2.ps1 -Firmware reconstructed -Action stop
```

Repeat these commands with `-Firmware original` for the factory VM. SDL shows the
LCD; `-Display none` runs headless. Add `-Network offline` to both launch and
control commands when addressing an offline VM. Screenshots are PNG files encoded from QEMU's
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
also must complete `app_main` and update its animation. In `clock` mode it must
associate and obtain `10.0.2.15`; both images must receive an NTP reply and store
its timestamp in the RTC boot-time offset. `-Network offline` checks the earlier
AP startup path instead.
Hashes verify that the working base flash and source artifacts were unchanged.
The test stops its own VMs and saves separate JSON reports and screenshots in a
timestamped directory. To shorten a smoke run, use `-TestSeconds 10`.
Stop any VM that uses an explicitly supplied test state directory first.

## Modeled hardware

`configs/devices/esp32c2-small-tv.json` describes the SoC and stock board.
`configs/boards/small-tv-c2.json` is an executable wiring profile; pass it as
`-HardwareProfile` to either launcher/test script. See
[composable peripherals](README-modular-esp.md) for GPIO routing, shared SPI/SD,
other displays, and legacy CYD/C3 compatibility. Changing wiring also requires
matching firmware GPIO assignments.

| Component | Model |
|---|---|
| CPU | One RV32IMC core, 120 MHz cycle counter; C2 fixed PMPADDR3â€“15 |
| ROM / SRAM | 576 KiB genuine ROM; 16 KiB cache SRAM + 256 KiB SRAM, with correct IRAM/DRAM aliases |
| Flash / MMU | 4 MiB Small TV flash; 64 MMU entries, C2 invalid bit, selectable 16/32/64 KiB pages; 64 KiB verified by both images |
| Interrupts | 43 C2 sources and the C2 enable/priority/threshold register layout |
| Timers | SYSTIMER 10.4 MHz, APB 40 MHz, XTAL 26 MHz; RTC scratch/time and reset registers |
| DMA / SPI | One GDMA channel pair; SPI1 flash access; SPI2 FIFO and DMA data transfers |
| LCD | ST7789, RGB565, 240Ã—240; D/C GPIO5, Reset GPIO1; SCK4/MOSI6 described by the direct SSI bus |
| Backlight | GPIO18's active-low LEDC channel 0 duty represented by average screen brightness |
| eFuse | Synthetic C2 v2.0 identity and local MAC `02:00:00:00:00:02`; no owner's device identity copied |
| Wi-Fi | C2 MAC/DMA at `0x60047000`; shared 802.11 AP frontend, isolated DHCP/ARP/DNS/NTP |
| PHY startup | Register readback and synchronous completion flags for analog calibration and MAC reset |

The LCD is always selected because this firmware uses `CS=-1`; its physical CS
wiring has not been established. SPI GPIO routing and waveforms, panel scanout,
RF measurements, Bluetooth, ADC, low-power modes, power consumption,
secure boot and encrypted flash are not validated by this model. SD sector
exchange is covered by the modular MMIO tests; neither stock Small TV guest
uses an SD driver. The average
backlight mapping is based on the firmware, not a new physical polarity test.
Flash snapshots are supported; full CPU/device checkpoints and live migration
are not supported. Compatible C3 UART/GPIO/SPI-flash/SHA/timer building blocks
are reused while C2 memory, PMP, interrupts, MMU, RTC and GDMA are separate.

## Virtual time and verification

The launcher uses `-icount shift=3,align=off,sleep=on`: eight virtual nanoseconds
per instruction, approximately one instruction per 125 MHz cycle. This is not
cycle-accurate. The CPU cycle-counter property still reports 120 MHz. Virtual
seconds may run slower than wall seconds during heavy display work. NTP sets the
clock to host UTC at synchronization; a long run may subsequently drift relative
to the host. The automatic `icount` setting and the former host-clock mode caused
interrupt watchdog panics in these images, so neither is the default. Watchdogs
remain enabled in the guest. Only the fixed instruction-count mode is validated.

Both C2 applications passed Wi-Fi/NTP, LCD, panic and reset/reboot checks in
headless 45-second runs. The test compares the **RTC offset actually stored by
SNTP** with the server timestamp in the same virtual time scale; a populated LCD
alone is not proof of synchronization. The existing C3 production firmware also
passed its separate boot/OLED regression with the final QEMU binary. The earlier
30-second offline evidence remains in the firmware repository for comparison.

Screenshots, sanitized UART logs and JSON reports are in `yoRadio/Small_TV/qemu/`.
These are emulator results. No serial device or physical board was accessed.
The user authorized SDL testing; the visible windows were subsequently closed,
and the final network validation uses `-Display none`.

Hardware specifications can be checked against Espressif's
[ESP8684 datasheet](https://documentation.espressif.com/esp8684_datasheet_en.html)
and the target headers/ROM ELF in the pinned ESP-IDF source archive.
