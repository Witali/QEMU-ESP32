# ESP32 modem clock/reset register correction

The ESP32 DPORT model now retains the modem clock and reset words:

| Register | Address | Reset value | Behavior |
| --- | --- | --- | --- |
| `DPORT_WIFI_CLK_EN_REG` | `0x3ff000cc` | `0xfffce030` | Read/write clock-enable latch |
| `DPORT_WIFI_RST_EN_REG` | `0x3ff000d0` | `0` | Read/write reset latch; software clears asserted bits |

The [ESP32 TRM v5.8](https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf),
sections 12.3.7 and 12.5, registers 12.21/12.22, defines the addresses, reset
values and software-cleared reset behavior. The matching local IDF 5.5.4
`soc/dport_reg.h` calls the reset word `DPORT_CORE_RST_EN_REG`; the two names
refer to the same address. SDK clock-gate code also uses modem bits beyond
the named Ethernet/SDIO fields, so the model retains the complete word.

Previously both words read as zero and writes were ignored. The SDK's clock
read/modify/write sequence therefore failed the `PHY_INIT_MODEM_CLOCK_REQUIRED_BITS`
assertion (`0x8f8f`) before it could initialize PHY. `FIXED_EMULATION` marks
the corrected handlers. Reset restores both latch defaults. This change models
the register state; it does not claim RF calibration, clock-domain timing,
Wi-Fi packet support, or full ESP32 migration support.

`tests/esp32_phy_clock_test.py` exercises actual QTest MMIO with the CPU stopped:
power-on defaults, the SDK mask sequence, independent/software-cleared reset,
and system-reset restoration. The same final test rejects all four cases on
the old binary and passes all four on the corrected binary. It records SDK,
test, VM-helper and display-helper hashes.

```text
python -X utf8 -B tests/esp32_phy_clock_test.py --qemu PATH_TO_XTENSA_QEMU --bios pc-bios --sdk-root PATH_TO_IDF_5_5_4 --output NEW_RESULT_DIRECTORY
```

The corrected Xtensa binary is `2099ed93...`; the RISC-V binary remains exactly
`d64f3013...`. Existing common SPI NOR tests passed 11 groups across ESP32,
ESP32-C2 and ESP32-C3; modular SPI/LCD/SD/I2C checks also passed. Exact hashes
and raw records are stored with the validation evidence, not inferred from a
binary's package label.

CYD application acceptance remains open. With this correction, the unchanged
archived CYD app passes the former PHY clock assertion and then faults at
`0x401b9920`, reading `0x60033c00`. The matching Arduino ESP32 3.3.8 libraries
identify this as the first register read in `register_chipv7_phy`; both use
lib-builder `14a0af9` / IDF `735507283d`. The entry instructions match apart from
the literal relocation. The existing APB alias formula maps the address to
`0x3ff73c00`, where the classic ESP32 machine has no Wi-Fi/PHY device.

The machine currently offers OpenETH. Accurate continuation of this unmodified
Wi-Fi path requires a corresponding peripheral model. A successful clock-latch
test or absence of panic during a ROM-only stall cannot establish CYD startup.
