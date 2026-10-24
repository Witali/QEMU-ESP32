#!/usr/bin/env python3
"""Paused MMIO checks for an explicit, functional ESP32 input-delay property."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

CONTROLLER_SHA256 = "09146e1c15c90fc2d0b8c81eb0c3c2cbd057ac86254c0069440427a7d0d3f01b"
CONTROLLER = Path(__file__).resolve().with_name("test_esp32_idf6_compat.py")
if hashlib.sha256(CONTROLLER.read_bytes()).hexdigest() != CONTROLLER_SHA256:
    raise RuntimeError("Exact canonical 274-case controller required")
sys.dont_write_bytecode = True
import test_esp32_idf6_compat as common
if Path(common.__file__).resolve() != CONTROLLER:
    raise RuntimeError("Canonical controller must come from the same directory")

# Independent SDK register words and fixed-80MHz delay expectations, not model calls.
# Each tuple is name, SPI_CLOCK value, compensation clocks at input-delay-ns=25.
CLOCK_CASES = (
    ("EQU80", 0x80000000, 2),
    ("DIV40", 0x1001, 1),
    ("DIV20", 0x3043, 0),
    ("PRE2_N0", 1 << 18, 1),
    ("EQU_ignores_PRE_N", 0x800C5085, 2),
)


def cases(vm, image, delay_ns, checks):
    def equal(name, actual, expected):
        checks.append(dict(case=name, passed=actual == expected,
                           actual=actual.hex() if isinstance(actual, bytes) else actual,
                           expected=expected.hex() if isinstance(expected, bytes) else expected))
    clock_reg = common.SPI + 0x18
    equal("SPI_CLOCK documented initial reset", vm.read(clock_reg), 0x80003043)
    common.transfer(vm, 0x06)
    common.transfer(vm, 0x31, tx=b"\x02")  # QE is established by a real write.
    for name, clock, clocks_at_25ns in CLOCK_CASES:
        extra = clocks_at_25ns if delay_ns == 25 else 0
        vm.write(clock_reg, clock)
        equal(name + " full clock write/read", vm.read(clock_reg), clock)
        for mode, (opcode, _, default_dummy) in common.MODES.items():
            bits = 28 if mode in ("qio", "dio") else 24
            protocol_dummy = {"qio": 5, "dio": 2}.get(mode, default_dummy)
            for address in (0x123450, 0x3700fc, 0x3710fc):
                for count in (2, 3, 16, 64):
                    equal(f"{name} {mode} {address:#x}+{count} extra{extra}",
                          common.transfer(vm, opcode, address=address, rx=count, mode=mode,
                                          addr_bits=bits, dummy_clocks=protocol_dummy + extra),
                          bytes(image[address:address + count]))
        equal(name + " JEDEC after compensation",
              common.transfer(vm, 0x9f, rx=3, dummy_clocks=extra), b"\xc8\x40\x16")
        equal(name + " written QE after compensation",
              common.transfer(vm, 0x35, rx=3, dummy_clocks=extra), b"\x02" * 3)
        incomplete = common.transfer(vm, 0xeb, address=0x3700fc, rx=16, mode="qio",
                                     addr_bits=28, dummy_clocks=extra)
        equal(name + " omitted QIO protocol dummy is detected",
              incomplete != bytes(image[0x3700fc:0x37010c]), True)
        equal(name + " full duplex does not compensate",
              common.transfer(vm, 0x9f, tx=b"\xff", rx=3, dummy_clocks=8), b"\x40\x16\x00")
        equal(name + " DOUTDIN receive path does not compensate",
              common.transfer(vm, 0x9f, rx=3, dummy_clocks=8, full_duplex=True),
              b"\x40\x16\x00")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("qemu", "qemu-sha256", "source-root", "rom-dir", "out"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--input-delay-ns", type=int, choices=(0, 25), default=25)
    args = parser.parse_args()
    qemu, rom, source = (Path(v).resolve(strict=True)
                         for v in (args.qemu, args.rom_dir, args.source_root))
    folder = Path(args.out).resolve()
    common.need(re.fullmatch(r"[0-9a-f]{64}", args.qemu_sha256), "Explicit executable digest")
    common.need(common.fact(qemu)["sha256"] == args.qemu_sha256, "Selected executable digest")
    common.need(folder.parent.name == ".build" and folder.parent.is_dir(), "Fresh direct .build child")
    common.need(not folder.exists(), "Output already exists")
    for path in (qemu, rom, source, folder):
        common.need(not any(c in str(path) for c in ",\r\n"), "Unsafe QEMU option path")
    folder.mkdir()
    image = bytearray(b"\xff" * (4 * 1024 * 1024))
    for start in (0x123400, 0x3700c0, 0x3710c0):
        image[start:start + 512] = bytes((i * 73 + start // 256 + 29) & 255 for i in range(512))
    image[0x3700fc:0x3700fe], image[0x3710fc:0x3710fe] = b"\xb9\x04", b"\xa6\x04"
    flash = folder / "synthetic-flash.bin"
    flash.write_bytes(image)
    inputs = [qemu, Path(__file__).resolve(), CONTROLLER, flash,
              rom / "esp32-v3-rom.bin", rom / "esp32-v3-rom-app.bin"]
    inputs += [source / relative for relative in common.MODEL_FILES]
    before = {str(p): common.fact(p) for p in inputs}
    command = [str(qemu), "-M", "esp32,audiodev=quiet", "-smp", "2", "-accel", "qtest", "-S",
               "-L", str(rom), "-drive", f"file={flash},if=mtd,format=raw", "-snapshot",
               "-nic", "none", "-audiodev", "none,id=quiet", "-display", "none", "-serial", "none",
               "-monitor", "none", "-qtest", "stdio", "-qtest-log", "none", "-global",
               f"driver=ssi.esp32.spi,property=input-delay-ns,value={args.input_delay_ns}"]
    report = dict(schema="esp32-spi-input-delay-paused-qtest-v1", result="INCOMPLETE", command=command,
                  inputs_before=before, checks=[], expected_checks=391, input_delay_ns=args.input_delay_ns,
                  clock_cases=CLOCK_CASES, fixed_source_clock_hz=80000000, guest_cpus_resumed=False,
                  firmware_used=False, production_boot_qualified=False, physical_or_radio_qualified=False,
                  partial_bit_waveform_or_analog_latency_qualified=False)
    vm = None
    error = None
    try:
        vm = common.StoppedQTest(command, folder)
        cases(vm, image, args.input_delay_ns, report["checks"])
        common.need(len(report["checks"]) == 391, "Complete 391-case input-delay matrix")
        report["case_execution_completed"] = True
    except BaseException as exc:
        error = repr(exc)
    finally:
        if vm:
            try:
                vm.close()
            except BaseException as exc:
                error = repr(exc) if error is None else error + "; cleanup: " + repr(exc)
            report.update(qemu_exit_code=vm.proc.returncode, cleanup_method=vm.cleanup_method,
                          stdout_actual_eof=vm.eof.is_set(),
                          stdout_reader_joined=vm.reader is not None and not vm.reader.is_alive(),
                          controller_errors=vm.errors)
        try:
            report["inputs_after"] = {str(p): common.fact(p) for p in inputs}
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
