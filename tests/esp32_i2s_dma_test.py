#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise classic ESP32 I2S output DMA start and cancellation through QTest."""

import argparse
import subprocess
from pathlib import Path
from threading import Timer

I2S0 = 0x3ff4f000
CONF = I2S0 + 0x08
INT_RAW = I2S0 + 0x0c
INT_CLR = I2S0 + 0x18
OUT_LINK = I2S0 + 0x30
OUT_EOF_DES_ADDR = I2S0 + 0x38
LC_CONF = I2S0 + 0x60

TX_START = 1 << 4
TX_RESET = 1 << 0
OUT_LINK_START = 1 << 29
OUT_LINK_STOP = 1 << 28
LC_OUT_RESET = 1 << 1
OUT_EOF = 1 << 12

DESCRIPTOR = 0x3ffb1000
BUFFER = 0x3ffb2000


def write(address, value):
    return f'writel {address:#x} {value:#x}'


def check(qemu, bios):
    commands = [
        # A single 16-byte, DMA-owned descriptor with the EOF flag.
        write(DESCRIPTOR, (1 << 31) | (1 << 30) | (16 << 12) | 16),
        write(DESCRIPTOR + 4, BUFFER),
        write(DESCRIPTOR + 8, 0),
        write(BUFFER, 0x81238123),
    ]
    checks = {}
    arm_link = write(OUT_LINK, OUT_LINK_START | (DESCRIPTOR & 0xfffff))
    start_tx = write(CONF, TX_START)
    advance = 'clock_step 100000'

    def reset():
        commands.extend((write(CONF, TX_RESET), write(CONF, 0),
                         write(INT_CLR, 0xffffffff)))

    def expect(name, completion):
        checks[name] = (len(commands), completion)
        commands.extend((f'readl {INT_RAW:#x}', f'readl {OUT_EOF_DES_ADDR:#x}'))

    reset()
    commands.extend((arm_link, advance, write(CONF, 0), advance))
    expect('armed before TX start', False)
    commands.extend((start_tx, advance))
    expect('DMA link before TX start', True)

    reset()
    commands.extend((start_tx, arm_link, advance))
    expect('TX start before DMA link', True)

    for name, cancel in (
        ('link stop', (write(OUT_LINK, OUT_LINK_STOP),)),
        ('DMA reset', (write(LC_CONF, LC_OUT_RESET), write(LC_CONF, 0))),
        ('TX reset', (write(CONF, TX_RESET), write(CONF, 0))),
    ):
        reset()
        commands.extend((arm_link, advance, *cancel, start_tx, advance))
        expect(name, False)

    # The qtest accelerator executes no guest instructions. The VM runs so
    # clock_step can deliver virtual timer callbacks between MMIO writes.
    process = subprocess.Popen([
        str(qemu), '-M', 'esp32,audiodev=quiet', '-smp', '2',
        '-accel', 'qtest', '-L', str(bios), '-nic', 'none',
        '-audiodev', 'none,id=quiet', '-display', 'none', '-serial', 'none',
        '-monitor', 'none', '-qtest', 'stdio', '-qtest-log', 'none',
    ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    watchdog = Timer(15, process.kill)
    watchdog.start()
    replies = []
    try:
        for command in commands:
            process.stdin.write(command + '\n')
            process.stdin.flush()
            line = process.stdout.readline()
            while line.startswith('IRQ '):
                line = process.stdout.readline()
            reply = line.split()
            if reply[:1] != ['OK']:
                raise RuntimeError(f'{command!r}: invalid QTest response {line!r}')
            replies.append(reply)
    finally:
        watchdog.cancel()
        process.terminate()
        process.wait()
        process.stdin.close()
        process.stdout.close()

    failures = []
    for name, (index, completion) in checks.items():
        raw = int(replies[index][1], 0)
        descriptor = int(replies[index + 1][1], 0)
        if bool(raw & OUT_EOF) != completion or (completion and descriptor != DESCRIPTOR):
            failures.append(f'{name}: raw={raw:#x}, EOF descriptor={descriptor:#x}')
    if failures:
        raise SystemExit('\n'.join(failures))
    print(f'Passed {len(checks)} ESP32 I2S DMA start and cancellation checks')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--bios', type=Path, required=True)
    args = parser.parse_args()
    check(args.qemu.resolve(strict=True), args.bios.resolve(strict=True))
