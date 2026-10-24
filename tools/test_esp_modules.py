#!/usr/bin/env python3
"""Integration checks against actual QEMU MMIO, GPIO matrix, SSI and SD models."""
import sys, json, socket, subprocess, time, struct, hashlib, argparse
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from small_tv_vm import QMP, save_json
from esp_board import compile_profile
r = Path(__file__).resolve().parent.parent
base = r / '.cache/esp-boards/module-tests'
options = None

def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

class VM:

    def __init__(self, soc, devices, tag, legacy=False):
        self.folder = base / tag
        self.folder.mkdir(exist_ok=True)
        self.sd = self.folder / 'sd.img'
        self.sd.write_bytes(b'\x00' * (8 * 1024 * 1024))
        self.sd_hash = sha(self.sd)
        for d in devices:
            if d['model'] == 'sd':
                d['image'] = str(self.sd)
        p = self.folder / 'board.json'
        save_json(p, {'schema': 1, 'soc': soc, 'devices': devices})
        config = compile_profile(p)
        if legacy:
            config = {'machine': 'esp32,st7789=on,sdspi=on', 'arguments': ['-drive', f'file={self.sd},if=sd,format=raw,snapshot=on']}
        self.soc = soc
        self.gpio = 0x3ff44000 if soc == 'esp32' else 0x60004000
        self.mux = 0x3ff49000 if soc == 'esp32' else 0x60009000
        self.qp = port()
        self.tp = port()
        exe = Path(options.qemu_xtensa if soc == 'esp32' else options.qemu_riscv)
        rom = r / 'pc-bios' if soc != 'esp32c2' else Path(options.c2_rom_dir)
        flash = self.folder / 'flash.bin'
        flash.write_bytes(b'\xff' * (4 * 1024 * 1024))
        cmd = [str(exe), '-M', config['machine'], '-L', str(rom), '-drive', f'file={flash},if=mtd,format=raw', '-snapshot', '-nic', 'none', '-display', 'none', '-serial', 'none', '-monitor', 'none', '-S', '-qmp', f'tcp:127.0.0.1:{self.qp},server=on,wait=off', '-qtest', f'tcp:127.0.0.1:{self.tp},server=on,wait=off', '-qtest-log', str(self.folder / 'qtest.log')] + config['arguments']
        self.log = (self.folder / 'qemu.log').open('wb')
        self.p = subprocess.Popen(cmd, stdout=self.log, stderr=self.log, creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0)
        deadline = time.monotonic() + 15
        while True:
            if self.p.poll() is not None:
                raise RuntimeError((self.folder / 'qemu.log').read_text(errors='replace'))
            try:
                self.sock = socket.create_connection(('127.0.0.1', self.tp), 0.3)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise RuntimeError('QTest startup timed out')
                time.sleep(0.1)
        self.sock.settimeout(5)
        self.file = self.sock.makefile('rwb')
        self.q = QMP(self.qp)

    def command(self, text):
        self.file.write((text + '\n').encode())
        self.file.flush()
        while True:
            line = self.file.readline().decode().strip()
            if not line:
                raise RuntimeError('QTest connection closed')
            if line.startswith('IRQ'):
                continue
            if not line.startswith('OK'):
                raise RuntimeError(line)
            return line[2:].strip()

    def w(self, a, v):
        self.command(f'writel {a:#x} {v:#x}')

    def rd(self, a):
        return int(self.command(f'readl {a:#x}'), 0)

    def gpio_out(self, pin, signal=None):
        c3 = self.soc != 'esp32'
        off = (0x554 if c3 else 0x530) + pin * 4
        self.w(self.gpio + off, signal if signal is not None else 128 if c3 else 256)
        self.w(self.gpio + 36, 1 << pin)
        if c3:
            self.w(self.mux + (pin + 1) * 4, 1 << 12)

    def gpio_in(self, pin, signal):
        c3 = self.soc != 'esp32'
        self.w(self.gpio + (0x154 if c3 else 0x130) + signal * 4, pin | 1 << (6 if c3 else 7))
        if c3:
            self.w(self.mux + (pin + 1) * 4, 1 << 12)

    def level(self, pin, high):
        self.w(self.gpio + (8 if high else 12), 1 << pin)

    def spi(self, host, tx, select=None, command=None, address=None):
        c3 = self.soc != 'esp32'
        reg = 0x60024000 if c3 else 0x3ff64000 if host == 2 else 0x3ff65000
        assert 0 < len(tx) <= 64
        fifo = reg + (152 if c3 else 128)
        for offset in range(0, len(tx), 4):
            self.w(fifo + offset, int.from_bytes(tx[offset:offset + 4].ljust(4, b'\x00'), 'little'))
        user = 1 << 27 | 1 << 28 | 1
        if command is not None:
            user |= 1 << 31
            self.w(reg + (24 if c3 else 36), 7 << 28 | command)
        if address is not None:
            user |= 1 << 30
            self.w(reg + 4, address)
            self.w(reg + (20 if c3 else 32), 31 << (27 if c3 else 26))
        self.w(reg + (16 if c3 else 28), user)
        self.w(reg + (28 if c3 else 40), len(tx) * 8 - 1)
        if not c3:
            self.w(reg + 44, len(tx) * 8 - 1)
        self.w(reg + (32 if c3 else 52), 63 if select is None else 63 & ~(1 << select))
        self.w(reg, 1 << (24 if c3 else 18))
        return b''.join((self.rd(fifo + i).to_bytes(4, 'little') for i in range(0, len(tx), 4)))[:len(tx)]

    def stat(self, dev):
        return self.q.execute('qom-get', path='/machine/peripheral/' + dev, property='wired-bytes')

    def close(self):
        self.q.execute('quit')
        self.q.close()
        self.file.close()
        self.sock.close()
        self.p.wait(timeout=5)
        self.log.close()
        assert sha(self.sd) == self.sd_hash, 'Snapshot changed backing SD image'

def lcd_node(host=2, pins=None):
    return {'id': 'lcd', 'bus': 'spi' + str(host), 'model': 'st7789', 'pins': pins or {'sck': 4, 'mosi': 6, 'cs': 3, 'dc': 5}, 'properties': {'visible-width': 240, 'controls': False, 'panel-inverted': True}}

def sd_node(host=2, pins=None):
    return {'id': 'sd', 'bus': 'spi' + str(host), 'model': 'sd', 'pins': pins or {'sck': 4, 'mosi': 6, 'miso': 2, 'cs': 10}}

def sd_cmd(v, host, cmd, arg=0, crc=1, hw=None):
    data = bytes([64 | cmd]) + arg.to_bytes(4, 'big') + bytes([crc]) + b'\xff' * 10
    out = v.spi(host, data, hw)
    for i in range(6, len(out)):
        if out[i] != 255:
            return (out[i], out[i + 1:])
    raise AssertionError(f'SD CMD{cmd} did not respond: {out.hex()}')

def check_sd(v, host, hw=None):
    assert sd_cmd(v, host, 0, 0, 149, hw)[0] == 1
    status, r7 = sd_cmd(v, host, 8, 426, 135, hw)
    assert status == 1 and r7[:4] == b'\x00\x00\x01\xaa'
    for attempt in range(20):
        sd_cmd(v, host, 55, hw=hw)
        status, _ = sd_cmd(v, host, 41, 0x40000000, hw=hw)
        if status == 0:
            break
    assert status == 0
    assert sd_cmd(v, host, 16, 512, hw=hw)[0] == 0
    phased = v.spi(host, b'\x01' + b'\xff' * 10, hw, command=80, address=512)
    assert next((x for x in phased[1:] if x != 255), None) == 0, ('SPI command/address phases did not reach SD', phased.hex())
    assert sd_cmd(v, host, 24, 4 * 512, hw=hw)[0] == 0
    payload = bytes((i * 37 + 11 & 255 for i in range(512)))
    stream = b'\xfe' + payload + b'\x00\x00' + b'\xff' * 5
    responses = b''.join((v.spi(host, stream[i:i + 64], hw) for i in range(0, len(stream), 64)))
    assert any((b & 31 == 5 for b in responses[-8:])), 'SD data-write response missing'
    response, rest = sd_cmd(v, host, 17, 4 * 512, hw=hw)
    assert response == 0
    data = rest
    while len(data) < 530:
        data += v.spi(host, b'\xff' * 64, hw)
    start = data.index(b'\xfe') + 1
    assert data[start:start + 512] == payload, 'SD sector readback differs'

def test_spi(soc='esp32c2', dual=False):
    lp = {'sck': 14, 'mosi': 13, 'cs': 15, 'dc': 2} if dual else None
    sp = {'sck': 18, 'mosi': 23, 'miso': 19, 'cs': 5} if dual else None
    v = VM(soc, [lcd_node(pins=lp), sd_node(3 if dual else 2, sp)], soc + ('-dual' if dual else '-shared'))
    try:
        if dual:
            v.gpio_out(14, 8)
            v.gpio_out(13, 10)
            v.gpio_out(15, 11)
            v.gpio_out(2)
            v.gpio_out(18, 63)
            v.gpio_out(23, 65)
            v.gpio_in(19, 64)
            v.gpio_out(5, 68)
        else:
            v.gpio_out(4, 63)
            v.gpio_out(6, 65)
            v.gpio_in(2, 64)
            v.gpio_out(3, 68)
            v.gpio_out(10, 69)
            v.gpio_out(5)

        def lcd(cmd, args=b''):
            v.level(2 if dual else 5, False)
            v.spi(2, bytes([cmd]), 0)
            if args:
                v.level(2 if dual else 5, True)
                v.spi(2, args, 0)
        lcd(17)
        lcd(41)
        lcd(33)
        lcd(42, b'\x00\x00\x00\x01')
        lcd(43, b'\x00\x00\x00\x00')
        lcd(44, b'\xf8\x00\x00\x1f')
        screen = v.folder / 'lcd.ppm'
        v.q.execute('screendump', filename=str(screen))
        px = screen.read_bytes().split(b'\n', 3)[3]
        assert px[:6] == b'\xff\x00\x00\x00\x00\xff', (px[:6].hex(), v.stat('lcd'), v.stat('sd'))
        sh = 3 if dual else 2
        select = 0 if dual else 1
        n = v.stat('lcd')
        check_sd(v, sh, select)
        assert v.stat('lcd') == n, 'SD traffic reached LCD'
        assert v.stat('sd') > 1000
        cp = 18 if dual else 4
        v.gpio_out(cp, 256 if dual else 128)
        before = v.stat('sd')
        v.spi(sh, b'\xff' * 8, select)
        assert v.stat('sd') == before, 'Wrong clock route still transfers'
        v.gpio_out(cp, 63)
        v.gpio_in(9 if not dual else 22, 64)
        assert v.spi(sh, b'\xff' * 8, select) == b'\x00' * 8, 'Wrong MISO route still receives'
        return {'lcd_sd_transfer': True, 'topology': 'two_hosts' if dual else 'shared_bus', 'lcd_pixels': True, 'sd_sector_write_read': True, 'cs_isolation': True, 'wrong_sck_blocked': True, 'wrong_miso_blocked': True, 'sd_snapshot_unchanged': True}
    finally:
        v.close()

def test_legacy_cyd():
    v = VM('esp32', [], 'legacy-cyd-mmio', legacy=True)
    try:
        v.level(21, True)
        v.level(15, False)

        def lcd(cmd, args=b''):
            v.level(2, False)
            v.spi(2, bytes([cmd]), 0)
            if args:
                v.level(2, True)
                v.spi(2, args, 0)
        lcd(17)
        lcd(41)
        lcd(42, b'\x00\x00\x00\x01')
        lcd(43, b'\x00\x00\x00\x00')
        lcd(44, b'\xf8\x00\x00\x1f')
        screen = v.folder / 'lcd.ppm'
        v.q.execute('screendump', filename=str(screen))
        px = screen.read_bytes().split(b'\n', 3)[3]
        assert px[:6] == b'\xff\x00\x00\x00\x00\xff', px[:6].hex()
        v.level(15, True)
        v.level(5, False)
        check_sd(v, 3, 0)
        v.level(5, True)
        assert v.spi(3, b'\xff' * 8, 0) == b'\xff' * 8, 'Legacy SD idle changed'
        return {'legacy_flags': True, 'lcd_pixels': True, 'sd_sector_write_read': True, 'sd_idle_high': True, 'snapshot_unchanged': True}
    finally:
        v.close()

def test_i2c(soc):
    node = {'id': 'oled', 'bus': 'i2c0', 'model': 'ssd1306', 'address': 60, 'pins': {'scl': 9, 'sda': 8}, 'properties': {'visible-width': 72, 'visible-height': 40, 'column-offset': 28, 'magnify': 4}}
    v = VM(soc, [node], soc + '-i2c')
    try:
        v.gpio_out(9, 53)
        v.gpio_out(8, 54)
        reg = 0x60013000

        def send(data):
            v.w(reg + 24, 1 << 12 | 1 << 13)
            v.w(reg + 36, 0xffffffff)
            for b in bytes([120]) + data:
                v.w(reg + 28, b)
            v.w(reg + 88, 1 << 11 | 1 << 8 | len(data) + 1)
            v.w(reg + 92, 2 << 11)
            v.w(reg + 4, 1 << 4 | 1 << 5)
            return not bool(v.rd(reg + 32) & 1 << 10)
        assert send(b'\x00\xae\xa5\xaf'), 'Remapped I2C did not ACK'
        screen = v.folder / 'oled.ppm'
        v.q.execute('screendump', filename=str(screen))
        pixels = screen.read_bytes().split(b'\n', 3)[3]
        assert sum((b != 0 for b in pixels)) > 1000, 'OLED remained black'
        v.gpio_out(9, 128)
        assert not send(b'\x00\xa5'), 'Incorrect I2C route still ACKs'
        return {'alternate_gpio_oled': True, 'wrong_scl_nack': True}
    finally:
        v.close()

def check_profiles():
    """Reject configurations that cannot be wired to the real SoC."""
    import copy
    good = {'schema': 1, 'soc': 'esp32c2', 'devices': [lcd_node()]}
    bad = []
    node = copy.deepcopy(good)
    node['devices'][0]['bus'] = 'spi3'
    bad.append(node)
    node = copy.deepcopy(good)
    node['devices'][0]['pins']['sck'] = 12
    bad.append(node)
    node = copy.deepcopy(good)
    node['devices'][0]['pins']['dc'] = 4
    bad.append(node)
    node = copy.deepcopy(good)
    node['devices'][0]['pins']['cs'] = -1
    node['devices'].append(lcd_node())
    bad.append(node)
    node = copy.deepcopy(good)
    node['devices'].append(copy.deepcopy(node['devices'][0]))
    bad.append(node)
    for index, node in enumerate(bad):
        path = base / f'invalid-{index}.json'
        save_json(path, node)
        try:
            compile_profile(path)
        except ValueError:
            continue
        raise AssertionError(f'Invalid profile {index} was accepted')
    return {'unsupported_bus': True, 'flash_gpio': True, 'conflicting_pins': True, 'always_selected_shared_bus': True, 'duplicate_id': True}

def main():
    global options, base
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--soc', choices=('all', 'esp32'), default='all', help='Select all checks or only ESP32 dual-host SPI and legacy CYD checks')
    parser.add_argument('--qemu-riscv', default=str(r / 'bin/qemu-system-riscv32.exe'))
    parser.add_argument('--qemu-xtensa', default=str(r / 'bin/qemu-system-xtensa.exe'))
    parser.add_argument('--c2-rom-dir', help='Directory containing genuine esp32c2-rev200-rom.bin (required for --soc=all)')
    parser.add_argument('--state-directory', default=str(base))
    options = parser.parse_args()
    if options.soc == 'all' and not options.c2_rom_dir:
        parser.error('--c2-rom-dir is required when --soc=all')
    base = Path(options.state_directory).resolve()
    base.mkdir(parents=True, exist_ok=True)
    if options.soc == 'all':
        checks = {'profile_validation': check_profiles(), 'c2_shared_spi': test_spi(), 'c3_shared_spi': test_spi('esp32c3'), 'esp32_dual_spi': test_spi('esp32', True), 'c2_i2c': test_i2c('esp32c2'), 'c3_i2c': test_i2c('esp32c3'), 'legacy_cyd_mmio': test_legacy_cyd()}
        qemu_sha256 = {'riscv32': sha(Path(options.qemu_riscv)), 'xtensa': sha(Path(options.qemu_xtensa))}
    else:
        checks = {'esp32_dual_spi': test_spi('esp32', True), 'legacy_cyd_mmio': test_legacy_cyd()}
        qemu_sha256 = {'xtensa': sha(Path(options.qemu_xtensa))}
    report = {'passed': True, 'soc': options.soc, 'checks': checks, 'qemu_sha256': qemu_sha256}
    save_json(base / 'modular-report.json', report)
    print(json.dumps(report, indent=2))
if __name__ == '__main__':
    main()
