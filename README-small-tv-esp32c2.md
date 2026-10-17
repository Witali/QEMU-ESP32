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
also must render its application and update its animation. Its Wi-Fi/SNTP flags
are read from real RAM objects located in the matching application ELF. In `clock` mode it must
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


### C2 RX DMA backpressure

When a C2 RX burst exhausts its DMA chain, the next frame stays at the head of
the existing bounded injection queue. Writing a new RX base descriptor wakes
delivery immediately, respecting the original delivery time and FIFO order.
There is no retry polling timer. Configured loss and queue-limit controls still
apply. The legacy C3 delivery path is unchanged.

The packet regression includes a C2 QTest case for a five-second descriptor
starvation interval, queue-limit overflow, ordered release and no duplicates.
QTest executes no firmware instructions; its C2 ROM is a size-correct placeholder.
The previous emulator fails this case, while the corrected emulator passes all
seven DNS/NTP/receive tests. A separate real Small TV run completed 120 settings
POSTs with a maximum response time of 0.166 virtual seconds, versus earlier
SYN-retry delays around six seconds. This does not replace a complete firmware
suite or physical RF/concurrency acceptance.

The modular SPI/I2C/SD checks pass. Unchanged CYD and C3 OLED images also pass
boot/display/no-panic checks, including the C3 modular profile. The Xtensa
executable and canonical installed RISC-V executable were not replaced during
these candidate checks.


### External station AP shutdown and recovery

The runtime boolean QOM property `station-ap-online` defaults to true. Setting
it false gracefully shuts down the modeled external AP: queued station traffic
is discarded, an associated guest receives an actual 802.11 deauthentication
frame (reason 3), and further station traffic receives no service. Setting it
true makes the AP available again; the guest performs authentication,
association and DHCP using its own SDK. The guest's setup SoftAP is independent.
The selected external state survives a guest system reset.

For example, use QMP after obtaining the Wi-Fi object's path with `qom-list`:

```json
{"execute":"qom-set","arguments":{"path":"/machine/wifi","property":"station-ap-online","value":false}}
{"execute":"qom-set","arguments":{"path":"/machine/wifi","property":"station-ap-online","value":true}}
```

This models an announced AP shutdown, not a sudden RF cut or beacon-loss
timeout. The existing static `link-down` setting and packet-loss controls remain
separate. No firmware RAM, registers, clock, instructions or PC are assigned.

All eight packet tests pass for candidate RISC-V executable SHA-256
`fde38d5311cf05b7c22e9ed114b2b57f22ce25f0f06d3f31b02bf519cc8d42eb`.
The new C2/C3 case checks the actual DMA deauthentication bytes, idempotent
shutdown, no UDP service while offline, real reassociation and reset retention.
Eleven helper tests, modular SPI/I2C/SD tests, unchanged CYD/C3 OLED boots and
real initial HTTP setup/AP-to-STA/persistence for both Small TV applications
also pass. Those integration runs use genuine ROM and firmware instructions.

A separate read-only GDB observation of reconstructed app `a2b6129b` records
the live SDK connection flag becoming false, the simple clock advancing while
offline, and normal reconnection/HTTP recovery. All three completed 240x240
frames match the declared corrected factory renderer exactly; the retained
station address is `10.0.2.15`. This scoped run does not validate every theme,
long outages, Wi-Fi security or physical radio. Canonical executables were
not replaced during these candidate checks.


### Passive Wi-Fi admission tracing

`esp32c3_wifi_packet_stage` identifies enqueue, enqueue-drop and completed DMA
stages using the MAC sequence, DMA addresses, queue depth and optional TCP
ports/sequence/flags. `esp32c3_wifi_rx_state` observes RX register writes and
interrupt acknowledgement. These events are disabled by default. Enable the
ESP Wi-Fi events with `-d trace:esp32c3_wifi* -D wifi-admission.log`, or use the
HMP `log trace:esp32c3_wifi*` command on an isolated synthetic test VM. The trace
never logs HTTP headers or bodies. Its format conversion lives in
`hw/net/esp32c3_wifi_trace.c` separately from queue and DMA handling.

Data-frame allocation now initializes a default -25 dBm signal level; beacon
and management constructors may override it. Previously the common constructor
left this field uninitialized, so simultaneous allocations could produce RSSI
from unrelated heap contents. The packet regression queues eight replies at
once and checks the metadata on both C2 and C3. Immediate single-frame tests
could hide the issue by reusing an earlier valid management-frame allocation.
This definite metadata fix alone is not proof that every observed TCP retry
or HTTP latency outlier has been eliminated; compare identical quiet workloads
and retain the end-to-end firmware latency checks.


For timing-sensitive failures, enable only `esp32c3_wifi_syn_stage` and
`esp32c3_wifi_rx_drop`. The sparse SYN event records receive enqueue/DMA and
guest transmit before dispatch, with IRQ state and an approximate CPU
translation-block PC. It also reports the 802.11 type/subtype, so a missing
SYN-ACK can be distinguished from a different frame-dispatch path. The PC is
not an exact instruction sample. Use the ordinary packet/MMIO events separately:
frequent synchronous log writes can change host I/O scheduling enough to hide
a timing-sensitive failure.


### Finite transfer time after queue backlog

Link timing and loss now live in `hw/net/esp32c3_wifi_link.c`. RX and TX use the
same transfer-duration calculation when arming or continuing their FIFO timers.
Previously, already overdue nominal deadlines were clamped to `now`, which
could complete several frames in the same virtual instant. Finite bandwidth
now schedules each complete frame after its transfer duration. The existing
latency/jitter/loss controls remain; `bandwidth-bps=0` keeps unlimited mode.

The regression holds two NTP replies without RX descriptors for five seconds,
then rearms DMA at 8 Mbit/s. Previously both replies appeared after one ns;
now each 112-byte frame takes 112 us, with FIFO order and no duplicates.
All ten Wi-Fi packet tests, modular SPI/I2C/SD tests and unchanged CYD/C3 OLED
boot fixtures pass on the candidate. The identical quiet five-route Small TV
workload previously reached 6.187 virtual seconds; with finite dequeue timing,
180 requests completed with a maximum of 0.238274 seconds. These scoped results
do not replace the complete factory/reconstructed firmware regression suite.


### Compile-time diagnostic logging

The Windows installers default to `esp_debug=false`. This removes the ESP Wi-Fi
packet formatter module, RX drop/register traces and C2 DNS/NTP diagnostic
printing. Disabled diagnostic arguments (including virtual-time reads and
formatting) are not evaluated. Guest-error reporting remains available through
the normal QEMU logging controls. Legacy raw frame dumps remain disabled in both
modes; enabling diagnostics does not enable logging HTTP bodies or credentials.

```powershell
# Ordinary build, with ESP diagnostics compiled out:
./setup-qemu-esp32c2-windows.ps1
# Compile diagnostic capability and DNS/NTP event messages:
./setup-qemu-esp32c2-windows.ps1 -DebugLogging
```

The shared C3 installer accepts the same switch. An existing build directory is
reconfigured in either direction; the installed cache marker includes the mode.
`-SkipBuild` rejects a different configured mode. PowerShell's common `-Debug`
parameter is unrelated; use `-DebugLogging`. Direct Meson/configure users set
`-Desp_debug=true` or `-Desp_debug=false`. This option is independent of Meson's
`debug` option for compiler symbols and does not change assertions, optimization
or the network/device model.

The read-only QOM property `/machine/wifi.debug-logging` reports the compiled
capability. Packet events still require a runtime selection such as
`-d trace:esp32c3_wifi* -D wifi-admission.log` in a diagnostic build. In a release
build those event names cannot re-enable code that was excluded at compilation.
The existing extended firmware transport suite parses C2 DNS/NTP event messages
and therefore needs a diagnostic QEMU build. Release behavior is checked using
actual DMA packets, HTTP responses, guest clock and LCD state rather than log
presence. The Wi-Fi packet test accepts `--expect-debug off` or `on` and verifies
identical real DNS/NTP replies, read-only capability and absent/present traces.

Small TV firmware has its own independent `DEBUG` CMake option and
`build.ps1 -DebugLogging`; refer to its source README. Its debug images have a
separate archive variant. Changing emulator verbosity never changes firmware
logging or writes firmware settings.


### Boot checks without diagnostic output

The ordinary firmware omits INFO/DEBUG output, so the boot test observes the
actual `connected`, `ap_active`, `last_station_address` and `time_synced` objects,
plus the SDK's two signed 64-bit timekeeping corrections.
`tools/esp_elf.py` converts bounded ELF32 RISC-V symbol tables; it is a separate
format module. The application header's full ELF SHA-256 must match before any
VM starts. Only named objects with the expected scalar sizes in C2 RAM are read,
through read-only QMP `xp` commands. No guest function, PC or RAM is changed.
The default ELF is `yoRadio/.build/small-tv-esp32c2/small_tv_reconstructed.elf`;
use `-AppElf` (Python `--app-elf`) for a different matching build. The factory
boot checks retain their original ROM/LCD/RTC checks and require no native ELF.

The shared C2/C3 Wi-Fi model exposes read-only `last-ntp-reply` as a JSON string
with `count`, `unix_seconds` and `virtual_us`. The model records these three
fixed-size scalars only when it generates a valid NTP reply, before RX queuing.
Dropped/malformed requests do not update the snapshot; a guest reset retains
this external-service observation. JSON formatting occurs only on QMP reads,
independently of `esp_debug`. No packet content or credentials are retained.

Both boot tests reconstruct SDK UTC from RTC STORE2/3, the guest's own latched
UNIT0 ticks (`ticks * 5 / 52` on C2/26 MHz), `s_microseconds_offset` and
`s_correction_us`. They compare it with `unix_seconds + (now_us - sent_us)/1e6`,
within one second, before and after reset; reset must generate a newer reply.
The signed SDK corrections account for its reset-relative timer origin. Treating
RTC STORE2/3 as an offset from QEMU lifetime time is incorrect after reset.
Comparing later host wall time is also invalid under `icount`, whose virtual
seconds can lag the host during display work. The observer briefly stops and
continues its VM for a coherent read, without latching or writing guest registers.

Factory offsets are bound to capture SHA-256
`5321a6b9016a8fc8335958193b4ab182e0d9ea85cfc3363063f120080c1844f4`:
`esp_time_impl_get_time_since_boot` at `0x420b288c` adds `[0x3fcc3980]`;
`esp_system_get_time` at `0x403802da` adds `[0x3fcc3888]`. The corresponding
RV32 loads/64-bit additions match the SDK source and native disassembly.
A different factory dump is rejected until its own layout is established.
Older diagnostic QEMU builds can supply the same exact timestamp through their
event log. Silent builds predating this property must be rebuilt; a loose date
range or a sync flag alone is not an accepted clock measurement.

`tests/esp_elf_test.py` checks malformed tables, ambiguous/missing objects,
ELF/application mismatch, memory bounds, scalar ABI and read-only observations.
The Wi-Fi DMA suite also checks packet/snapshot equality, read-only access,
drop/malformed handling, monotonic counters and snapshot retention after reset,
with diagnostics either disabled or enabled.


Scoped validation on 2026-10-17 is recorded in
[`docs/esp-quiet-boot-validation-20261017.json`](docs/esp-quiet-boot-validation-20261017.json):
12 ELF/clock unit tests, 29 invalid-report rejection controls, six actual Small TV
boot/reset flows, 12 DMA/network cases in each diagnostic mode, modular SPI/I2C
checks in each mode, and unchanged CYD/C3 OLED image boot checks in each mode.
The quiet and diagnostic executable SHA-256 values are `0b3f6f69...616e6` and
`f3d7b024...8fdc5`. The two previously rejected host-wall/RTC-only measurements
were test-observation errors; neither was relabeled as a passing firmware run.
These scoped results do not replace full firmware parity or a 24-hour soak.


### Wait for reset clock readiness in guest time

Clock-mode reset validation waits for a newer NTP response, actual SDK UTC within
one second of that response, and the native connected/DHCP/SNTP flags. It no
longer assumes that sleeping eight host seconds means the clock has synchronized.
The pinned SDK enables `SNTP_STARTUP_DELAY_FUNC = LWIP_RAND() % 5000`;
`icount` can run fewer guest seconds than host seconds. A debug guest was still
connected but not synchronized at the old fixed checkpoint; a measured run
recovered after 8.25 host / 6.415086 guest seconds.

The wait has independent limits of 15 guest seconds and 60 host seconds. It uses
the read-only low radio microsecond counter, handles one counter wrap, and stops
on either deadline, including when the guest clock stalls. It does not alter
the SDK delay, network state, guest RAM or PC. The existing UTC error bound is
unchanged. `reset_readiness` records both elapsed times and the observed state.
Offline AP validation retains its existing observation path.

The ELF/clock suite now has 19 cases, including delayed readiness beyond eight
host seconds, stale reply rejection, missing notification, incorrect SDK UTC,
stalled guest time, counter wrap and an answer arriving after the deadline.
The previous fixed-wait failure is preserved as a failed attempt; it is not
relabeled as a successful firmware run. The canonical helper used by an active
immutable firmware regression is updated only after that regression finishes.
