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

## Optional profile service URLs

Device-specific endpoints are listed in `network.urls` in the selected device
profile. The default `small-tv` profile is `configs/boards/small-tv-c2.json`;
it also supplies NTP hostnames and setup/station SSIDs. The emulator itself has
no provider-specific DNS defaults. See the [profile format and migration guide](configs/boards/README.md).

`-ServicePort <port>` enables isolated forwarding to a loopback fixture. The
zero default disables HTTP service aliases and forwarding. For example:

```powershell
.\run-small-tv-esp32c2.ps1 -ServicePort 19000 -Display none
.\run-small-tv-esp32c2.ps1 -ServicePort 19000 `
    -ServiceUrl 'http://first.service.test/a','http://second.service.test:8081/b' `
    -Display none
```

Python accepts `--service-port` and repeated `--service-url` options. Hosts from
the enabled URL list resolve to separate subnet addresses starting at `.100`;
multiple paths/ports on one hostname share its address. TCP traffic reaches
`127.0.0.1:<ServicePort>`. The fixture receives the original HTTP Host, path and
query. HTTPS requires a compatible TLS fixture. These settings configure
reachability; firmware must request the same endpoints. `session.json` lists
`service_urls` and `service_endpoints`, without storing URL queries.

NTP/DNS services follow the configured station subnet. Restricted DHCP receives
the local DNS option only when no option was already supplied. Other Internet
access remains restricted and AP/STA localhost port blocks remain separate.
Existing firmware fixture callers using `weather_port` or `-WeatherPort` retain
compatibility; new callers should use the generic service interface.

The Small TV differential firmware fixture remains in
`yoRadio/Small_TV/src/tests/weather_qemu.py`. It checks failure/retry, parsed
positive/negative temperatures and displayed pixels for factory and reconstructed
applications. The factory's unchecked 503 JSON parser is recorded as a vendor
defect. Independent emulator DNS/NTP checks are in
`tests/esp32_wifi_services_test.py`; they exercise real packets through Wi-Fi DMA
without executing guest instructions. Firmware-specific assertions remain in the firmware repository.

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

## Controlled NTP regression fixtures

The existing `clock-services` station fixture has three runtime QOM controls on
`/machine/wifi`. They change only external UDP/DNS service responses. They never
set guest time, alter firmware code or patch guest RAM. Defaults preserve the
ordinary host-UTC clock fixture and are inert when clock services are disabled.

| QOM property | Default | Meaning |
| --- | --- | --- |
| `ntp-unix-seconds` | `-1` | Host UTC, or a fixed nonnegative signed 64-bit UNIX epoch |
| `ntp-drop-mask` | `0` | Bits 0/1/2 drop NTP replies from servers 1/2/3; only 0..7 is accepted |
| `ntp-separate-servers` | `false` | Opt into distinct service addresses for testing real SDK fallback |

With separate servers enabled, `ntp1.aliyun.com`, `cn.pool.ntp.org` and
`pool.ntp.org` resolve to subnet suffixes `.2`, `.4` and `.5`. ARP responds for
these fixture addresses. With the default disabled setting, all NTP names retain
the previous `.2` alias. Weather DNS, the AP endpoint and localhost AP/STA port
assignment are unchanged. A configured numerical `.4` address also reaches the
second fixture server. The service logs indices and timestamps without arbitrary
hostnames or user keys.

For example, issue these JSON commands after QMP capabilities negotiation:

```json
{"execute":"qom-set","arguments":{"path":"/machine/wifi","property":"ntp-separate-servers","value":true}}
{"execute":"qom-set","arguments":{"path":"/machine/wifi","property":"ntp-unix-seconds","value":1709179420}}
{"execute":"qom-set","arguments":{"path":"/machine/wifi","property":"ntp-drop-mask","value":3}}
```

The guest must really contact the first two unavailable servers and then receive
the third server's NTP packet. Changing the mask restores the service; it does not
invoke a guest callback. Fixed epochs remain 64-bit even on Windows, whose
`timeval.tv_sec` uses a 32-bit `long`. NTP wire seconds still wrap at the protocol's
2036 era boundary; the guest SDK is responsible for era interpretation.

`yoRadio/Small_TV/src/tests/time_qemu.py` exercises factory and reconstructed
applications, actual clock updates, dates, reconfiguration and reset recovery.
The factory has intentional recorded defects: invalid-year restarts can starve
fallback, synchronized 2024 is rejected, and the date waits for a minute then
retains outer pixels when shortened. Those failures are retained as observations;
the reconstructed application must show the corrected behavior. The pinned SDK
keeps its last server index across stop/init, so tests use failure masks to force
the intended fallback instead of assuming every reconfiguration starts at index 0.

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


### Diagnosing receive drops

The disabled-by-default `esp32c3_wifi_rx_drop` trace records virtual time, the
current RX descriptor, frame type/sequence/length and the model's drop reason.
Enable only this event with `-d trace:esp32c3_wifi_rx_drop -D wifi-rx.log`, or
the HMP command `log trace:esp32c3_wifi_rx_drop`. It does not change packet
delivery or guest state. Avoid global `guest_errors` tracing for timing tests:
unrelated register warnings can dominate the runtime.

An isolated Small TV diagnostic on the unchanged app `9e4f856a` correlated
slow connection attempts with `no-descriptor` drops. In 120 synthetic settings
POSTs, two replies took about 5.74/5.76 virtual seconds; a separate Ethernet
PCAP records the SYN retransmissions. The trace identifies the model-side loss,
but is not a complete network/concurrency or physical-radio acceptance test.
