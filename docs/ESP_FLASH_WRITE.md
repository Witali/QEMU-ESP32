# ESP SPI NOR flash: write model and verification

The shared chip implementation is `hw/block/m25p80.c`. ESP32, ESP32-C2 and
ESP32-C3 instantiate it with a part selected from its chip table. The SPI
controller transports commands; the chip owns its contents and write protection.
C2/C3 memory-mapped flash reads are a separate cache/MMU layer.

## Specification used

The 4 MiB ESP32-C2 machine instantiates `gd25q32`, JEDEC `c8 40 16`.
This identifies the emulated part, not a measurement of the physical Small TV.
The reference is the manufacturer's GD25Q32C datasheet, sections 5, 6,
7.1/7.14/7.17–7.20 and the AC characteristics table:

- [GigaDevice download](https://download.gigadevice.com/Datasheet/DS-00088-GD25Q32C-Rev4.1.pdf)
- [Manufacturer datasheet mirrored by TME](https://www.tme.eu/Document/4ba9367e2d662f55232f34bcacc537e6/gd25q32c.pdf)

The TME document was accessible during the audit. Its typical cycle times are
used below; these are not maximum timings or an analog model of a particular die.

## Required write sequence

1. Read the status register (`05h`) and wait for WIP bit0 to clear.
2. Erase the containing sector when any stored bit must change from 0 to 1:
   WREN (`06h`), sector erase (`20h`) with a 24-bit address, then poll WIP.
   Sector size is 4096 bytes; any address within that sector selects it.
3. Before **each** page program, issue a new WREN and verify WEL bit1 as needed.
4. Send PP (`02h`), a 24-bit address and the data. End the command by raising CS.
   A page is 256 bytes; split application data at page boundaries. A transfer
   crossing the boundary wraps its column address inside that same page.
5. Poll WIP until zero; WEL is consumed by program/erase. Read back and compare
   the result before relying on the settings or performing a cold restart.

Programming clears bits (`old & data`), never restores a 1. Erasure sets all
bits in the selected unit to 1 (`ff`). There is no requirement to erase before
every PP if the requested change only clears bits. Protected regions reject
both programming and erasing. Chip erase must also respect that protection.

The Small TV native raw settings record is 444 bytes at `0x3e7000`, inside
one 4 KiB sector. ESP-IDF splits its write into controller/page-sized transfers.
The application keeps its dirty flag set until NVS commit, erase and the whole
`esp_flash_write` have returned success. A reader of the host image may observe
the legitimate erased interval between those operations.

## Implemented corrections

- PP wraps inside the page; it no longer silently writes the following page.
- PP and erase consume WEL; WRDI continues to disable modification.
- Sector/block erase aligns to the containing unit, including the final unit
  of the chip. This also prevents an unaligned end-of-chip erase from overrunning
  the storage buffer. Program and erase share one protected-interval predicate.
- C2 now selects byte-based dummy phases, like the shared SPI1 controller and
  C3. GigaDevice/Winbond quad reads require QE and use the proper wire-byte count.
- C2/C3 cache invalidation refreshes mapped aliases, drains outstanding chip
  backend writes, and invalidates translated instructions after updating bytes.
  Initial MMU fills use the same path and handle backend read errors explicitly.
- `gd25q32.operation-timing=on` supplies WIP for PP (0.6 ms), 4 KiB erase
  (50 ms), 32/64 KiB erase (150/250 ms), chip erase (15 s) and status write
  (5 ms). The cycle starts on CS deassertion and uses QEMU's virtual clock.
  Normal reads, WREN and modifying commands are ignored while busy.

C2 enables that timing property for its 4 MiB part. Other machines retain
their prior timing selection; a C3 `gd25q32` may opt in with
`-global gd25q32.operation-timing=on`. Unsupported part names reject this
timing property instead of borrowing GD25Q32C timings. Timing deadlines have
a migration subsection; old untimed streams remain loadable.

## Files, snapshots and limits

The flash chip has an in-memory working copy. Its backing block writes are
asynchronous, so reading `flash.bin` from the host is not an atomic settings
transaction. A completed guest operation does not promise physical host-disk
fsync durability. Cache fills drain queued block writes before reading them.

`-snapshot` writes an overlay: guest reads see the new data, the original input
file stays unchanged, and another VM starts with the original contents. Without
that option the disposable writable image receives changes and preserves them
between cold VM launches. Never test writes against the only factory dump.

This is a functional SPI NOR model. It does not simulate wear, analog power
loss, torn internal programming, OTP/security registers, program/erase suspend,
or every manufacturer's complete status/protection matrix. In particular the
legacy protected-interval implementation is not a full GD25Q32C BP4/CMP decoder.
The SPI1 tests use transfers of at most 64 bytes, including a page-edge crossing;
they do not establish oversized (>256-byte) single-command program behavior.

## Reproducible tests

`tests/esp_flash_write_test.py` uses CPU-stopped disposable QTest VMs and TCP
on loopback. It checks NOR bit semantics, WREN/WRDI/WEL, exact virtual WIP
boundaries, protected erase, page wrap, erase-unit boundaries, FAST/DIO/QIO
readback, mapped cache refresh, the 444-byte settings record, cold persistence
and snapshot isolation. Use `--socs esp32c2 esp32c3 esp32` with `--qemu`,
`--qemu-xtensa`, `--bios` and `--output` paths. This runs no guest firmware.

`tests/esp32c3_flash_test.py` separately covers the existing 2/4/8/16 MiB
GigaDevice/Winbond/legacy read-mode matrix and invalid machine configurations;
its transport is now portable TCP QTest. Real firmware setup/cold-persistence
and modular peripheral/legacy boot checks are separate acceptance records.

The Small TV regional observer treats erased/partial ASCII as pending with
the existing bounded timeout; permanent corruption still fails. Native tests
also observe the ELF-bound dirty flag before accepting persisted bytes, so
matching an early page cannot stop a VM before the remaining write finishes.

## Validation checkpoint — 2026-10-18

The installed candidate is RISC-V `20b133d9…` and Xtensa `1845328f…`.
The shared write suite passed all 11 groups across C2, C3 and classic ESP32.
The exact same final harness fails the previous binaries (24 failing subcases,
plus two errors because the old timing property does not exist). The existing
C3 capacity/read-mode matrix passed 17 rows / 476 checks. Modular SPI/I2C/LCD/SD
checks passed. Final Small TV initial setup passed 16 factory / 21 native checks
and station isolation. The earlier 9DA candidate also passed all 15+15 regional
keys, maximum fields, cold restart and NVS migration; its exact scope is retained.

Full CYD compatibility remains **unaccepted**. The initial 25-second candidate
boot report passed, but a 60-second observation reached a pre-existing
`esp_phy_enable` assertion at `phy_init.c:332`. A same-toolchain control with
only the pre-change `m25p80.c` blob restored reproduces that assertion too.
The old installed Xtensa control also failed, at Quad Enable initialization.
These failed reports and UART logs are preserved. A short console/no-panic
observation does not establish complete application boot or Wi-Fi operation.
C3 OLED and modular OLED boot checks passed; no full-board equivalence is claimed.

See [the structured evidence record](validation/flash-write-validation-20261018.json)
and its compressed raw reports. It separates successful flash checks from the
remaining CYD PHY defect instead of presenting the combined legacy run as passed.