# yoRadio ESP32-C3 QEMU configuration

`run-yoradio-esp32c3.ps1` runs the unmodified ESP32-C3 production flash with
the C3-only Wi-Fi, I2C, SSD1306, and PCM models. It creates a persistent copy of
the flash and adds `/data/wifi.csv` to that copy, just as a configured physical
radio would. The application, bootloader, and partition table are not rebuilt
or patched.

From PowerShell:

```powershell
.\run-yoradio-esp32c3.ps1
```

The default `hardware` network profile uses 4 Mbit/s, 20 ms latency, 5 ms
jitter, a 64-packet queue, and no random loss. Available profiles are `ideal`,
`hardware`, `weak`, `congested`, and `offline`:

```powershell
.\run-yoradio-esp32c3.ps1 -NetworkProfile congested
```

QEMU user networking maps the emulated access point to the host network through
NAT. The virtual AP accepts `yoRadio-QEMU` with an empty password. The Web UI is
forwarded to `http://127.0.0.1:8080/`, and decoded 48 kHz stereo PCM is sent to
DirectSound. Use `-RestrictedNetwork` for an isolated test, `-AudioBackend wav`
to record audio, or `-AudioBackend none` to disable host audio.

The flash copy under `build/yoradio-esp32c3` keeps settings written by the Web
UI. Use `-ResetState` to recreate it and apply new `-Ssid`/`-Password` values.
Pass `-FirmwareBuild` when the production build is outside the adjacent
`yoRadio` checkout.
