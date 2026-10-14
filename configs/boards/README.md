# Device profiles: format and migration

Read this file before creating or updating a device profile. Keep application
names, provider endpoints and Wi-Fi names in profile data. The emulator models
hardware and protocols; it must not select a particular application or provider.

## Schema 1

Profiles are UTF-8 JSON objects. The network block is an additive extension of
schema 1; existing hardware-only profiles remain valid and default to offline.

```json
{
  "schema": 1,
  "name": "example-clock",
  "soc": "esp32c3",
  "devices": [],
  "network": {
    "station_ssid": "Example-Lab",
    "softap_client_ssid": "Example-Setup",
    "ntp_hosts": ["first.ntp.test", "second.ntp.test", "third.ntp.test"],
    "urls": [
      "http://first.service.test/status",
      "http://second.service.test:8081/api",
      "http://first.service.test/another-path"
    ]
  }
}
```

| Field | Meaning and validation |
| --- | --- |
| `schema` | Required integer `1`. |
| `name` | Optional descriptive device/profile name; an application name such as `small-tv` belongs here. |
| `soc` | Required: `esp32`, `esp32c2`, or `esp32c3`. Must match the selected machine. |
| `devices` | Required array of external peripherals. An empty array is valid. |
| `network` | Optional object. Missing fields do not enable network services. |
| `network.station_ssid` | Optional advertised station AP name, 1–32 UTF-8 bytes. The generic emulator default is `QEMU`. |
| `network.softap_client_ssid` | Optional guest AP name used by the virtual setup client, 1–32 UTF-8 bytes. |
| `network.ntp_hosts` | Optional list of zero to three distinct DNS names. Their order identifies NTP servers 1–3. |
| `network.urls` | Optional list of HTTP(S) URLs. Up to 155 distinct DNS hostnames; DNS labels are 1–63 bytes and the complete name is at most 253 bytes. |

URLs use DNS names, not IP literals. The compiler normalizes case and converts
international names to IDNA. Explicit TCP ports must be 1–65535; HTTP defaults
to 80 and HTTPS to 443. Credentials and fragments are rejected. NTP names must
not also appear as HTTP service names. URL queries are excluded from generated
metadata so that credentials in queries are not recorded.

## Peripheral entries

Each entry in `devices` contains a unique identifier `id`, `bus`, `model`, and
`pins`. SPI buses are `spi2` for C2/C3 and `spi2`/`spi3` for ESP32; I2C buses are
`i2c0` for C2/C3 and `i2c0`/`i2c1` for ESP32. Valid GPIOs depend on the SoC.

SPI pin roles are `sck`, `mosi`, `miso`, `cs`, `dc`, `reset`, and `backlight`.
I2C pin roles are `scl` and `sda`. Omitted optional pins use `-1`. An always
selected SPI peripheral cannot share its bus with another peripheral. I2C
addresses must be unique on a bus and lie between 1 and 127.

Model-specific fields include `properties` (a JSON object), `address` for I2C,
`image` for SD, and `backlight_active_low`. SD images are resolved relative to
the profile file. See the existing profiles and
[peripheral wiring guide](../../README-modular-esp.md) for complete examples.

## Compiling and launching

Validate and inspect generated arguments without running firmware:

```powershell
python tools/esp_board.py configs/boards/small-tv-c2.json --soc esp32c2
python tools/esp_board.py configs/boards/small-tv-c2.json --network clock --service-port 19000
```

Use `run-esp-board.ps1 -HardwareProfile <file> -FlashImage <image>` for generic
board launches. `-Network offline` is the default. The optional `clock` and
`setup` service fixtures currently support C2/C3. Supply `-Network clock
-ServicePort 19000` to route the profile URL list to a fixture listening on
`127.0.0.1:19000`. The fixture must dispatch requests using their original Host
headers and paths. Repeat `--service-url` in Python, or pass the `-ServiceUrl`
PowerShell array, to override the profile list for a particular run.

The compiler returns separate `arguments` for peripherals and
`network_arguments` for network configuration. A launcher must consume both,
and must not add `-nic none` when `network_arguments` already defines a NIC.
`service_urls` contains the enabled URL list. The Small TV controller also
records `service_urls` and complete `service_endpoints` in `session.json`.

Each distinct HTTP hostname receives one virtual subnet address, starting at
`.100`; multiple ports or paths for the same hostname share that address. TCP
forwards are deduplicated by address and port. NTP names share `.2` by default;
the runtime `ntp-separate-servers` control maps servers 1–3 to `.2`, `.4`, `.5`.
No DNS alias is enabled merely by running a bare QEMU machine.

The URL list configures DNS and TCP reachability. It does not filter individual
HTTP paths or rewrite firmware, Host headers, paths, or queries. Firmware must
request the configured endpoints. HTTPS requires a TLS-capable fixture and
compatible guest certificates; QEMU forwards the existing TCP bytes.

## Migrating a profile

1. Keep the existing `soc`, `devices`, wiring, display properties and image paths.
2. Add a descriptive `name` if useful. Put application/provider URLs in
   `network.urls`, configured NTP names in `network.ntp_hosts`, and device SSIDs
   in the corresponding network fields.
3. Replace provider-specific launcher settings with `-ServicePort` /
   `--service-port` and the profile URL list. The previous `WeatherPort` spelling
   remains a compatibility alias; new integrations should use the generic name.
4. Rebuild QEMU before using the new properties. Raw invocations can pass
   semicolon-separated `esp32c3-wifi.service-hosts` and `ntp-hosts`, together with
   `station-ssid`. HTTP aliases are supplied only when a local fixture is enabled.
5. Match the firmware's SSID, URL hostname/port and wiring to the profile. Use a
   new persistent state directory when changing saved station credentials.
6. Run the checks below. Commit only the profile and changes needed to support it.

Do not add provider names to `hw/`, introduce a replacement firmware HTTP server,
patch guest code/RAM to simulate success, or publish private API keys in profiles.

## Required checks

```powershell
python -B -m unittest discover -s tools -p 'test_*.py' -v
python tools/esp_board.py configs/boards/example.json --soc esp32c3
```

For emulator/network changes, run the firmware-independent packet regression:

```text
python tests/esp32_wifi_services_test.py --qemu <new-qemu> --bios pc-bios --output <services-results.json>
```

For Flash changes, run `tests/esp32c3_flash_test.py` with the same arguments on
Linux/WSL. CI runs both suites for native Linux riscv32 builds. Wiring changes
also need the relevant `tools/test_esp_modules.py` checks; firmware-specific
Small TV tests remain in `yoRadio/Small_TV/src/tests/` and must be run for changes
that affect application behavior. A packet test is not a full firmware boot test.
