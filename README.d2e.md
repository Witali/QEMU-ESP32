# DosToEsp QEMU-ESP32 fork

This working tree is based on Espressif QEMU tag
`esp-develop-9.2.2-20260417`, commit
`40edccac415693c5130f91c01d84176ae6008566`.

The `main` branch contains the CYD board models as regular commits:

- ESP32 SPI3 SDSPI and SPI2 ST7789 devices;
- GPIO input, BOOT and in-process RESET handling;
- accelerated SD bulk reads;
- real-time display, SD and ESP32 DAC audio timing;
- the Windows install fallback;
- ST7789 console keyboard events forwarded to ESP32 UART0 as ASCII/ANSI.

The original imported HLV patches are retained under `patches/hlv` for
provenance. The DosToEsp keyboard change is retained under `patches/d2e`.
These patches are already applied to `main`; do not apply them a second time.

## Windows build

From PowerShell:

```powershell
.\setup-qemu-esp32-windows.ps1
```

The script uses `local_tools\msys2\msys64` when present. During migration it
can also reuse the already installed sibling HLV-codec MSYS2 toolchain, or an
explicit toolchain can be selected with `-MsysRoot`. The runtime is installed
to `bin\qemu-system-xtensa.exe` with ROM data under `share\qemu`.

`upstream` points to the official Espressif repository and is intended only
for fetching future base updates. Configure a separate writable `origin`
before publishing this fork.
