# Classic ESP32: ESP-IDF 6 flash and clock compatibility

The classic ESP32 machine forwards SPI dummy bytes, selects the builtin READ
opcode from the configured read mode, and accounts for the four continuous-mode
bits that ESP-IDF includes in a 28-bit QIO/DIO address. Its NOR device uses the
existing byte-dummy-cycles contract. GD/Winbond Status Register 2 repeats until
CS rises. DPORT_WIFI_CLK_EN is ordinary 32-bit read/write storage with documented
reset 0xfffce030; software can enable and disable clock bits.

SPI_CLOCK now retains its complete register value and resets to 0x80003043.
Its divider is 1 with CLK_EQU_SYSCLK, otherwise (CLKDIV_PRE+1)*(CLKCNT_N+1).

## Optional input path

The SPI device property `input-delay-ns` defaults to zero. An explicit functional
input path can be selected with:

```
-global driver=ssi.esp32.spi,property=input-delay-ns,value=25
```

This collapses a receive delay and the compensating dummy clocks programmed by
the guest. It uses the ESP-IDF flash-host calculation at a fixed 80 MHz source:
`floor((ns+1)*80/1000)/divider`. At 25 ns, SPI frequencies 80/40/20 MHz require
2/1/0 compensation clocks. Compensation applies only to receive-only transfers;
MOSI and USER.DOUTDIN exclude it. The property remains configured across reset.
Only complete combined wire bytes are forwarded through SSI. Fractional-bit
waveforms, analog timing, arbitrary input paths and dynamic APB clock changes
are outside this functional model. This setting is an explicit emulation
assumption, not a measurement of a connected board.

## Checks

Run `tools/test_esp32_idf6_compat.py` for the 274-case flash/DPORT matrix, then
`tools/test_esp32_spi_input_delay.py` twice with `--input-delay-ns 0` and `25`.
The latter checks 391 comparisons per run: reset, five clock settings, actual
backing bytes in six read modes, JEDEC/status, omitted protocol dummy clocks,
and full-duplex/DOUTDIN exclusions. Each accepts explicit `--qemu`,
`--qemu-sha256`, `--source-root`, `--rom-dir`, and `--out`. The output must be a
fresh child of an existing `.build` directory. Both CPUs remain stopped;
QTest uses owned stdio and a synthetic snapshot medium. Owner termination of
this paused fixture is intentional; it is not a natural guest boot result.

`tools/test_esp_modules.py --soc esp32 --qemu-xtensa <exe> --state-directory
<fresh-directory>` runs the existing dual-host SPI2/SPI3 and legacy CYD checks
without unrelated C2/C3 firmware. These check LCD pixels, SD sector write/read,
CS isolation and incorrect GPIO routes. Python assertions must remain enabled.

The unchanged CYD SDK6.0.2 QIO80/CPU240/two-core production image now reaches
app_main and display setup without the earlier SPIFFS mount error when the
25 ns functional path is explicitly selected. It then faults during RF
calibration at unsupported MMIO 0x60033c00. Pipeline, radio and Web readiness
are not established. RF, network traffic, audio output, performance and physical
acceptance require their own evidence; the firmware and its task bindings are
unchanged by these emulator changes.

Primary definitions: ESP-IDF v6.0.2 `spi_flash_hal.c`, `spi_flash_hal_common.inc`,
ESP32 `gpio_caps.h`, `spi_flash_ll.h`, `spi_reg.h`, and `dport_reg.h`.
[GigaDevice GD25Q32C Rev4.1 section7.4](https://download.gigadevice.com/Datasheet/DS-00088-GD25Q32C-Rev4.1.pdf)
and the [Winbond W25Q32JV RevI manufacturer document, printed page24](https://devzone.nordicsemi.com/cfs-file/__key/support-attachments/beef5d1b77644c448dabff31668f3a47-0c4c655c5804440494198f0e003d1611/5810.W25Q32JV-RevI-05042021-Plus.pdf)
specify continuous status output. The Winbond document is hosted at Nordic;
the direct manufacturer download was unavailable during this check.
