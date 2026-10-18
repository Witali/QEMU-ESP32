# CYD boot evidence boundaries

The legacy CYD smoke check must observe the application at a comparable boot
stage and inspect the LCD panel itself. A live QEMU process, a ROM entry message,
or the absence of panic during a stall cannot establish application readiness.

`tools/test_esp_legacy.py` now requires the YoRadio AP/server startup marker
and more than 1000 nonzero RGB bytes within the top 320x240 LCD area. The console
occupies the rows below the LCD in the 320x284 QEMU surface and cannot substitute
for panel content. The complete raster size must also be valid.

`tools/test_esp_legacy_checks.py` supplies five concrete controls: ROM plus console
only; `network.begin` before PHY initialization; application plus a real panel;
application plus console only; and a truncated raster. Existing C3 OLED checks
are unchanged. The full tool unit set has 16 passing tests.

`tools/esp32_cyd_boot_test.py` records actual application milestones and stops
on the first panic/assertion. A successful bounded checkpoint additionally
requires AP/server startup, a 10-second observation window and LCD content.
NIC is disabled, so the AP/server marker is a boot milestone only: neither tool
establishes Wi-Fi association, DHCP, HTTP functionality, RF accuracy or SNTP.
Those need separate network tests against a corresponding peripheral model.

Historical CYD results must retain their limitations. On the old 20B-family
Xtensa binary the archived image did not reach its application startup marker
within 180 host seconds and its LCD remained blank. The newer binary reached
`network.begin` and failed the PHY clock assertion. After the separate DPORT
correction it reached the first `register_chipv7_phy` register read, where the
unimplemented `0x60033c00` access faulted. These are different boot stages;
none is a passing CYD application acceptance. See [ESP32_PHY_CLOCKS.md](ESP32_PHY_CLOCKS.md).
