# Binary migration file regression

`migration_file_test.py` starts two owned, headless `-M none -m 4M -S` VMs.
It loads every byte value, including LF, CRLF, Ctrl-Z, NUL and 0xff, into source
RAM and confirms that target RAM initially differs. It then uses QEMU's real
file migration transport, requires completed outgoing and incoming migration,
and compares the restored RAM bytes. It pins executable, test, helper and
payload hashes. It never connects to another VM or accesses a firmware image.

```text
python tests/migration_file_test.py --qemu <runtime-qemu-system-riscv32.exe> --output <new-result-directory>
```

Windows text-mode file descriptors expanded LF on write and treated Ctrl-Z as
EOF on read. A paused 4 MiB zero-RAM control already reproduced the problem:
the file contained 17 inserted CR bytes, and the target failed to load RAM.
Opening every migration file channel with `O_BINARY` preserves the byte stream,
including the primary and secondary outgoing and incoming channels. This flag
is zero on platforms without a distinct text mode. The correction is marked
`FIXED_EMULATION` in the source.

The all-byte test fails on the old Windows executable and passes after the
correction. A first harness revision waited only for `paused`; QEMU correctly
restored the source's `prelaunch` state. The final harness requires completed
incoming migration and accepts either stopped state before comparing RAM.
The initial failed harness report is retained in local investigation evidence.

This is a file-transport and RAM test. It does not establish migration support
for ESP peripherals, live host TCP streams, or the complete Small TV machine.
