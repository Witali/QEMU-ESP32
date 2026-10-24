#!/usr/bin/env python3
"""CPU-stopped QTest regressions for classic ESP32 flash reads and clock gates.

Source preparation only until explicitly run against a separately built emulator.
Uses a fresh synthetic flash fixture, stdio QTest, and no firmware or host sockets.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import sys
import threading
import time


LIMIT = 32 * 1024 * 1024
SPI = 0x3ff42000
CLOCK = 0x3ff000cc
MODES = {
    "slow": (0x03, 0, 0),
    "fast": (0x0b, 1 << 13, 8),
    "dout": (0x3b, (1 << 13) | (1 << 14), 8),
    "qout": (0x6b, (1 << 13) | (1 << 20), 8),
    "dio": (0xbb, (1 << 13) | (1 << 23), 4),
    "qio": (0xeb, (1 << 13) | (1 << 24), 6),
}
MODEL_FILES = (
    "hw/ssi/esp32_spi.c", "include/hw/ssi/esp32_spi.h",
    "hw/xtensa/esp32.c", "hw/misc/esp32_dport.c",
    "include/hw/misc/esp32_dport.h", "hw/block/m25p80.c",
)


def need(condition, message):
    if not condition:
        raise RuntimeError(message)


def fact(path):
    raw = path.read_bytes()
    return {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


class StoppedQTest:
    def __init__(self, command, folder):
        self.folder = folder
        self.command_line = command
        self.errors = []
        self.responses = queue.Queue(maxsize=32768)
        self.eof = threading.Event()
        self.stop = threading.Event()
        self.deadline = time.monotonic() + 60
        self.sent_bytes = 0
        self.received_bytes = 0
        self.closed = False
        self.cleanup_method = None
        self.stdin_log = (folder / "qtest-sent.txt").open("wb")
        self.stderr_log = (folder / "qemu-stderr.log").open("wb")
        self.proc = None
        self.reader = None
        self.watcher = None
        try:
            self.proc = subprocess.Popen(
                command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=self.stderr_log, bufsize=0,
                creationflags=(subprocess.CREATE_NO_WINDOW
                               if sys.platform == "win32" else 0),
            )
            self.reader = threading.Thread(target=self._read, daemon=True)
            self.watcher = threading.Thread(target=self._watch, daemon=True)
            self.reader.start()
            self.watcher.start()
        except BaseException:
            self.close()
            raise

    def _read(self):
        pending = bytearray()
        try:
            with (self.folder / "qtest-received.bin").open("wb") as log:
                while True:
                    raw = os.read(self.proc.stdout.fileno(), 4096)
                    if raw == b"":
                        self.eof.set()
                        need(not pending, "Partial QTest response at actual EOF")
                        return
                    self.received_bytes += len(raw)
                    need(self.received_bytes <= LIMIT, "QTest stdout size bound")
                    log.write(raw)
                    log.flush()
                    pending.extend(raw)
                    need(len(pending) <= 65536, "QTest response line bound")
                    while b"\n" in pending:
                        line, _, rest = pending.partition(b"\n")
                        pending = bytearray(rest)
                        self.responses.put_nowait(line.rstrip(b"\r").decode("ascii"))
        except BaseException as exc:
            self.errors.append("stdout reader: " + repr(exc))

    def _watch(self):
        while not self.stop.wait(0.02):
            try:
                need(time.monotonic() < self.deadline, "60 second MMIO fixture deadline")
                for name in ("qtest-sent.txt", "qtest-received.bin", "qemu-stderr.log"):
                    p = self.folder / name
                    need(not p.exists() or p.stat().st_size <= LIMIT, name + " size bound")
                need(not self.errors, "QTest reader failed")
            except BaseException as exc:
                self.errors.append("watchdog: " + repr(exc))
                if self.proc.poll() is None:
                    self.proc.terminate()
                return

    def command(self, text):
        need(not self.errors, "QTest controller error: " + repr(self.errors))
        need(self.proc.poll() is None, "QEMU exited before MMIO request")
        need(time.monotonic() < self.deadline, "MMIO fixture deadline")
        raw = (text + "\n").encode("ascii")
        self.sent_bytes += len(raw)
        need(self.sent_bytes <= LIMIT, "QTest stdin log size bound")
        self.stdin_log.write(raw)
        self.stdin_log.flush()
        need(self.proc.stdin.write(raw) == len(raw), "Short QTest stdin write")
        self.proc.stdin.flush()
        until = min(self.deadline, time.monotonic() + 3)
        while True:
            need(not self.errors, "QTest controller error: " + repr(self.errors))
            remaining = until - time.monotonic()
            need(remaining > 0, "QTest command response timeout: " + text)
            try:
                line = self.responses.get(timeout=min(0.1, remaining))
            except queue.Empty:
                need(not self.eof.is_set(), "Actual QTest stdout EOF before response")
                continue
            if line.startswith("IRQ "):
                continue
            need(line == "OK" or line.startswith("OK "), "QTest rejected request: " + line)
            return line[2:].strip()

    def write(self, address, value):
        need(self.command(f"writel {address:#x} {value:#x}") == "", "Unexpected write response")

    def read(self, address):
        return int(self.command(f"readl {address:#x}"), 0)

    def close(self):
        if self.closed:
            return
        self.closed = True
        if self.proc:
            if self.proc.poll() is None:
                self.cleanup_method = "terminate-owned-paused-qtest-process"
                self.proc.terminate()
            else:
                self.errors.append("QEMU exited before owner cleanup")
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.cleanup_method = "kill-owned-paused-qtest-process"
                self.proc.kill()
                self.proc.wait(timeout=3)
                self.errors.append("Graceful owner termination timed out")
        self.stop.set()
        if self.watcher:
            self.watcher.join(timeout=2)
            if self.watcher.is_alive():
                self.errors.append("Watchdog did not join")
        if self.reader:
            self.reader.join(timeout=3)
            if self.reader.is_alive():
                self.errors.append("Actual stdout reader did not join")
            if not self.eof.is_set():
                self.errors.append("No actual b'' stdout EOF")
        if self.proc:
            self.proc.stdin.close()
            self.proc.stdout.close()
        self.stdin_log.close()
        self.stderr_log.close()
        for name in ("qtest-sent.txt", "qtest-received.bin", "qemu-stderr.log"):
            p = self.folder / name
            if p.exists() and p.stat().st_size > LIMIT:
                self.errors.append("Final size bound exceeded: " + name)


def transfer(vm, opcode, *, address=None, rx=0, tx=b"", mode="slow",
             builtin=False, addr_bits=24, dummy_clocks=None, dummy_enabled=None,
             full_duplex=False):
    need(0 <= rx <= 64 and len(tx) <= 64, "PIO fixture transfer bound")
    _, flags, default_dummy = MODES[mode]
    clocks = default_dummy if dummy_clocks is None else dummy_clocks
    enabled = clocks > 0 if dummy_enabled is None else dummy_enabled
    vm.write(SPI + 8, flags)
    user = (1 << 31) | ((1 << 28) if rx else 0) | ((1 << 27) if tx else 0)
    if full_duplex:
        user |= 1
    if address is not None:
        user |= 1 << 30
        # Match spi_flash_hal_read: align the real 24-bit address at bit 8;
        # lower bits are ones even when ADDR_BITLEN extends it to 28 bits.
        value = address if builtin else (address << 8) | 0xff
        vm.write(SPI + 4, value)
    if enabled:
        user |= 1 << 29
    vm.write(SPI + 0x1c, user)
    vm.write(SPI + 0x20, ((addr_bits - 1) << 26) | max(clocks - 1, 0))
    vm.write(SPI + 0x24, (7 << 28) | opcode)
    vm.write(SPI + 0x28, max(len(tx) * 8 - 1, 0))
    vm.write(SPI + 0x2c, max(rx * 8 - 1, 0))
    for offset in range(0, 64, 4):
        word = tx[offset:offset + 4].ljust(4, b"\xa5")
        vm.write(SPI + 0x80 + offset, int.from_bytes(word, "little"))
    vm.write(SPI + 0x34, 6)  # CS0 only.
    vm.write(SPI, (1 << 31) if builtin else (1 << 18))
    return b"".join(vm.read(SPI + 0x80 + i).to_bytes(4, "little")
                    for i in range(0, rx, 4))[:rx]


def cases(vm, image, checks):
    def equal(name, actual, expected):
        checks.append({"case": name,
                       "actual": actual.hex() if isinstance(actual, bytes) else actual,
                       "expected": expected.hex() if isinstance(expected, bytes) else expected,
                       "passed": actual == expected})

    # Full R/W behavior, reset value, zero, unrelated high bits, SDK RMW masks,
    # and clearing clocks must work. A constant 0x8f8f cannot pass these checks.
    equal("DPORT documented reset", vm.read(CLOCK), 0xfffce030)
    for value in (0, 0x80000010, 0xffffffff, 0):
        vm.write(CLOCK, value)
        equal(f"DPORT write/read {value:#x}", vm.read(CLOCK), value)
    expected = 0x80000010
    vm.write(CLOCK, expected)
    for name, mask in (("common", 0x3c9), ("wifi", 0x406),
                       ("rng", 1 << 15), ("bt", 0x61 << 11)):
        vm.write(CLOCK, vm.read(CLOCK) | mask)
        expected |= mask
        equal("DPORT SDK enable " + name, vm.read(CLOCK), expected)
    equal("DPORT required PHY clocks enabled", vm.read(CLOCK) & 0x8f8f, 0x8f8f)
    for name, mask in (("rng", 1 << 15), ("common", 0x3c9)):
        vm.write(CLOCK, vm.read(CLOCK) & ~mask)
        expected &= ~mask
        equal("DPORT SDK disable " + name, vm.read(CLOCK), expected)
    equal("DPORT PHY mask detects cleared gates", vm.read(CLOCK) & 0x8f8f == 0x8f8f, False)

    for mode in ("qio", "qout"):
        equal(mode + " QE clear produces pulled-up output",
              transfer(vm, MODES[mode][0], address=0x3700fc, rx=16, mode=mode),
              b"\xff" * 16)
    for count in (2, 16, 64):
        equal(f"GD SR2 initial continuous {count}", transfer(vm, 0x35, rx=count),
              b"\x00" * count)
    transfer(vm, 0x06)
    equal("GD SR1 WEL continuous", transfer(vm, 0x05, rx=16), b"\x02" * 16)
    transfer(vm, 0x31, tx=b"\x02")
    equal("GD SR2 QE write/read", transfer(vm, 0x35, rx=1), b"\x02")
    equal("GD SR1 WEL cleared", transfer(vm, 0x05, rx=16), b"\x00" * 16)
    for count in (2, 16, 64):
        equal(f"GD SR2 QE continuous {count}", transfer(vm, 0x35, rx=count),
              b"\x02" * count)
    for clocks in (8, 16, 256):
        equal(f"GD SR2 after {clocks} dummy clocks",
              transfer(vm, 0x35, rx=3, dummy_clocks=clocks), b"\x02" * 3)
    transfer(vm, 0x06)
    transfer(vm, 0x31, tx=b"\x00")
    equal("GD SR2 clear continuous", transfer(vm, 0x35, rx=16), b"\x00" * 16)
    transfer(vm, 0x06)
    transfer(vm, 0x31, tx=b"\x02")
    equal("GD SR2 restore continuous", transfer(vm, 0x35, rx=3), b"\x02" * 3)
    addresses = (0x123450, 0x3700fc, 0x3710fc)
    for mode, (opcode, _, _) in MODES.items():
        for builtin in (False, True):
            for address in addresses:
                for count in (2, 3, 16, 64):
                    equal(f"{mode} {'READ' if builtin else 'USR'} {address:#x}+{count}",
                          transfer(vm, opcode, address=address, rx=count,
                                   mode=mode, builtin=builtin),
                          bytes(image[address:address + count]))
    # SDK6 classic ESP32 splits a mode byte across 28 address bits and dummy.
    # Separately rounding 28 address bits and 5*4 dummy bits adds a bad byte.
    for mode, clocks in (("qio", 5), ("dio", 2)):
        for address in addresses:
            for count in (2, 3, 16, 64):
                equal(f"SDK {mode} ADDR28 dummy{clocks} {address:#x}+{count}",
                      transfer(vm, MODES[mode][0], address=address, rx=count,
                               mode=mode, addr_bits=28, dummy_clocks=clocks),
                      bytes(image[address:address + count]))
    # SDK adds one sampling-delay clock for the ESP32 GPIO/input path.
    # SSI is byte-oriented: subbyte compensation must not discard a whole byte.
    for mode, (opcode, _, base_clocks) in MODES.items():
        address_bits = 28 if mode in ("qio", "dio") else 24
        clocks = {"qio": 6, "dio": 3}.get(mode, base_clocks + 1)
        for address in addresses:
            for count in (2, 3, 16, 64):
                equal(f"SDK {mode} extra sampling clock {address:#x}+{count}",
                      transfer(vm, opcode, address=address, rx=count, mode=mode,
                               addr_bits=address_bits, dummy_clocks=clocks),
                      bytes(image[address:address + count]))
    equal("SDK JEDEC ID with one sampling clock",
          transfer(vm, 0x9f, rx=3, dummy_clocks=1), b"\xc8\x40\x16")
    equal("SDK SR2 with one sampling clock",
          transfer(vm, 0x35, rx=3, dummy_clocks=1), b"\x02" * 3)
    # Stale DUMMY_CYCLELEN is ignored when USR_DUMMY is clear.
    equal("slow read ignores disabled dummy length",
          transfer(vm, 0x03, address=0x3700fc, rx=16, dummy_clocks=256,
                   dummy_enabled=False), bytes(image[0x3700fc:0x37010c]))
    for mode in ("fast", "qio"):
        observed = transfer(vm, MODES[mode][0], address=0x3700fc, rx=16,
                            mode=mode, dummy_enabled=False)
        unequal = observed != bytes(image[0x3700fc:0x37010c])
        equal(mode + " omitted dummy negative control", unequal, True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--qemu-sha256", required=True)
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--rom-dir", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    qemu = Path(args.qemu).resolve(strict=True)
    rom = Path(args.rom_dir).resolve(strict=True)
    source = Path(args.source_root).resolve(strict=True)
    folder = Path(args.out).resolve()
    need(re.fullmatch(r"[0-9a-f]{64}", args.qemu_sha256), "Explicit executable digest")
    need(fact(qemu)["sha256"] == args.qemu_sha256, "Selected executable digest")
    need(folder.parent.name == ".build" and folder.parent.is_dir(), "Fresh direct .build child")
    need(not folder.exists(), "Output already exists")
    for path in (qemu, rom, source, folder):
        need(not any(char in str(path) for char in ",\r\n"), "Unsafe QEMU option path")
    folder.mkdir()
    image = bytearray(b"\xff" * (4 * 1024 * 1024))
    for base in (0x123400, 0x3700c0, 0x3710c0):
        image[base:base + 512] = bytes(((i * 73 + base // 256 + 29) & 255)
                                           for i in range(512))
    image[0x3700fc:0x3700fe] = b"\xb9\x04"
    image[0x3710fc:0x3710fe] = b"\xa6\x04"
    flash = folder / "synthetic-flash.bin"
    flash.write_bytes(image)
    inputs = [qemu, Path(__file__).resolve(), flash,
              rom / "esp32-v3-rom.bin", rom / "esp32-v3-rom-app.bin"]
    inputs += [source / relative for relative in MODEL_FILES]
    before = {str(p): fact(p) for p in inputs}
    command = [str(qemu), "-M", "esp32,audiodev=quiet", "-smp", "2",
               "-accel", "qtest", "-S", "-L", str(rom),
               "-drive", f"file={flash},if=mtd,format=raw", "-snapshot",
               "-nic", "none", "-audiodev", "none,id=quiet", "-display", "none",
               "-serial", "none", "-monitor", "none",
               "-qtest", "stdio", "-qtest-log", "none"]
    report = {"schema": "esp32-idf6-paused-qtest-v1", "result": "INCOMPLETE",
              "command": command, "inputs_before": before, "checks": [],
              "guest_cpus_resumed": False, "firmware_used": False,
              "production_boot_qualified": False, "physical_or_radio_qualified": False}
    vm = None
    error = None
    try:
        vm = StoppedQTest(command, folder)
        cases(vm, image, report["checks"])
        need(len(report["checks"]) == 274, "Complete 274-case MMIO matrix")
        report["case_execution_completed"] = True
    except BaseException as exc:
        error = repr(exc)
    finally:
        if vm:
            try:
                vm.close()
            except BaseException as exc:
                error = repr(exc) if error is None else error + "; cleanup: " + repr(exc)
            report.update(qemu_exit_code=vm.proc.returncode,
                          cleanup_method=vm.cleanup_method,
                          stdout_actual_eof=vm.eof.is_set(),
                          stdout_reader_joined=vm.reader is not None and not vm.reader.is_alive(),
                          controller_errors=vm.errors)
        try:
            report["inputs_after"] = {str(p): fact(p) for p in inputs}
            report["inputs_unchanged"] = report["inputs_after"] == before
        except BaseException as exc:
            error = repr(exc) if error is None else error + "; input check: " + repr(exc)
        if (error is None and vm is not None and not vm.errors
                and report.get("inputs_unchanged") and report.get("stdout_actual_eof")
                and report.get("stdout_reader_joined") and report["checks"]
                and report.get("case_execution_completed")):
            report["result"] = ("MMIO_REGRESSIONS_COMPLETED"
                                if all(row["passed"] for row in report["checks"])
                                else "MMIO_REGRESSIONS_FAILED")
        report["error"] = error
        (folder / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(report["result"])
    return 0 if report["result"] == "MMIO_REGRESSIONS_COMPLETED" else 1


if __name__ == "__main__":
    sys.exit(main())
