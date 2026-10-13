ESP32-C3 selectable SPI NOR Flash
================================

Use the machine properties to select a Flash family and capacity::

  qemu-system-riscv32 -M esp32c3,flash-model=w25q,flash-size=8M \
      -L /path/to/qemu/share/qemu -nographic \
      -drive file=flash-8m.bin,if=mtd,format=raw

``flash-model`` accepts ``auto`` (default, original model mapping),
``gd25q`` (GigaDevice) and ``w25q`` (Winbond). ``flash-size`` accepts
``auto`` (default, infer from image), ``2M``, ``4M``, ``8M`` and ``16M``.
The units are MiB: 1 MiB = 1,048,576 bytes. The model name suffix denotes
megabits, so W25Q64 is 8 MiB, not 64 MiB.

================= ============= ============ ============ ============
Capacity          Auto model    GigaDevice   Winbond      JEDEC suffix
================= ============= ============ ============ ============
2 MiB / 16 Mbit   w25x16        gd25q16      w25q16       40 15
4 MiB / 32 Mbit   gd25q32       gd25q32      w25q32       40 16
8 MiB / 64 Mbit   gd25q64       gd25q64      w25q64       40 17
16 MiB / 128 Mbit is25lp128     gd25q128     w25q128      40 18
================= ============= ============ ============ ============

The suffix column applies to the explicit GD/Winbond families: prefix C8
for GigaDevice and EF for Winbond. Legacy auto 2 MiB returns EF 30 15;
auto 16 MiB returns 9D 60 18. No existing default model is changed.

The MTD image must have exactly the selected capacity. Invalid names,
capacities, missing MTD drives with explicit properties, and mismatches
are startup errors. The emulator does not resize or truncate user images.
To create a larger image safely, copy and pad with erased bytes (FF), e.g.::

  python3 -c 'from pathlib import Path; d=Path("flash-4m.bin").read_bytes(); n=8*1024*1024; assert len(d)<=n; f=open("flash-8m.bin","xb"); f.write(d); f.write(b"\xff"*(n-len(d))); f.close()'

Adjust the firmware flash-size configuration and partition table when
testing actual use of the extra capacity. Changing the chip alone does not
enlarge firmware partitions. These machine properties are fixed at startup.

Read modes and scope
-------------------

SPI1 supports the six ESP-IDF ESP32-C3 SDR read modes: slow read (03),
fast read (0B), dual output (3B), dual I/O (BB), quad output (6B),
and quad I/O (EB). The firmware's SPI registers select the read mode;
``flash-model`` does not override the firmware mode.

GigaDevice/Winbond use status-register-2 bit 1 (S9) as Quad Enable.
The model supports 05/35 status reads, 06 write-enable, 01 status writes
of one/two bytes and 31 status-register-2 writes. Quad reads/programming
require QE. A status write clears WEL. Legacy status bits and protection
remain governed by the existing m25p80 model.

For these families, SPI1 transports dummy phases as wire bytes:
fast/DOUT/QOUT use eight single-wire dummy clocks; DIO uses one mode byte;
QIO uses one mode byte and four quad-wire dummy clocks (three bytes total).
The controller's encoded dummy count is N-1. The optional m25p80 property
``byte-dummy-cycles`` enables this convention for the C3 attachment;
other boards retain their previous dummy-token convention.

This is functional emulation of these command paths, not a complete
electrical/timing model of every ordering suffix or silicon revision.
Programming/erase latency, nonvolatile status persistence across emulator
runs, OTP/security registers, full SFDP tables, QPI/continuous-read modes,
DDR/Octal operation and vendor-specific optional features are not certified
by these tests. Do not use these tests to infer hardware throughput.

Regression test
---------------

Run on Linux or WSL::

  python3 tests/esp32c3_flash_test.py \
      --qemu /path/to/qemu-system-riscv32 \
      --bios /path/to/qemu/share/qemu --output flash-results.json

Tests cover both explicit families at all four capacities, legacy defaults,
JEDEC IDs, QE/WEL, six read modes through user/special commands, distinct
patterns at low/high addresses, capacity wrap, program/erase and neighboring
data preservation. Negative cases check configuration errors. CPU execution
is halted; commands are issued through QEMU's qtest register interface.

References
----------

* `ESP-IDF v6.1 C3 supported Flash families <https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/peripherals/spi_flash/index.html#support-for-features-of-flash-chips>`_
* `GD25Q16C Rev3.2 <https://download.gigadevice.com/Datasheet/DS-00086-GD25Q16C-Rev3.2.pdf>`_: JEDEC table, status registers and read commands.
* `GD25Q32C Rev4.1 <https://download.gigadevice.com/Datasheet/DS-00088-GD25Q32C-Rev4.1.pdf>`_: sections 6, 7.4-7.11 and 7.26.
* `GD25Q128E Rev1.4 <https://download.gigadevice.com/Datasheet/DS-00480-GD25Q128E-Rev1.4.pdf>`_: JEDEC table and default dummy configuration.
* `W25Q128JV RevF <https://www.winbond.com/resource-files/w25q128jv%20revf%2003272018%20plus.pdf>`_: sections 7.1.4, 8.1.1 and 8.2.7-8.2.11.

Espressif lists ISSI, GD, MXIC, FM, Winbond, XMC and BOYA for quad modes.
It lists families, not a guarantee of every feature in every part number.
The present selector intentionally exposes GD and Winbond plus the original
automatic mapping; it does not claim to implement all Espressif-listed vendors.
