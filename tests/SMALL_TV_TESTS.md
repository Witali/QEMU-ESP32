# Small TV reset checks

The VM smoke test reports independent `checks_by_subsystem` decisions for
`boot`, `network`, `display`, and `source_integrity`. The overall result requires
every executed check to pass; one subsystem cannot stand in for another.

* Boot uses ROM/bootloader markers, VM status, panic checks and a new ROM banner
  after reset. `reset_reboots` tests the boot count alone.
* Network uses linked-ELF SDK association/DHCP/SNTP state and exact NTP/SDK UTC,
  including reset-relative offsets and a fresh response after reset.
* Display uses complete 240x240 RGB PPM captures, the existing >1000 nonzero
  RGB-byte smoke threshold, animation changes, and bounded post-reset drawing.
  It reads no UART or Wi-Fi/SNTP flags. This threshold proves visible content,
  not pixel-perfect scene parity, which the firmware's separate scene gates test.
* Source integrity checks the archived input images and backing flash.

`tools/small_tv_display.py` owns PPM validation and the display observer. It
allows intermediate blank/partial captures for at most 5 guest or 30 host
seconds. A permanent black panel, truncated raster, stalled guest or completion
at the deadline fails. There are no added guest sleeps or RAM/PC writes.

A network recovery exception is retained and does not skip display observation.
A display exception retains the network result. `FIXED_TEST` marks these host
observer corrections. Error details are retained in `reset_subsystems`.

Run the two independent unit suites with a Python 3 interpreter:

```text
python -X utf8 -B tests/esp_elf_test.py --output network-unit.json
python -X utf8 -B tests/small_tv_display_test.py --output display-unit.json
```

The first suite retains its 19 ELF/SDK/SNTP checks. The second has 9 display
checks and 3 subsystem isolation controls. Its real-file fixtures deliberately
contain no UART log. Both record source/helper SHA-256 values.

The change follows a failed full 0592/D64 run: both ROM boots and reset SNTP
passed, while the single immediate screen capture was black. With unchanged
old helper/application/D64, a separate live VM reproduced 624 nonzero RGB bytes
followed by 11629 after 0.034633 guest seconds. The failed report remains failed.
The old mixed `reset_reboots` condition has been replaced by independent boot
and `display_ready_after_reset` decisions, rather than weakening the LCD check.
