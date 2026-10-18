#!/usr/bin/env python3
"""Check SPI NOR writes, mapped reads and cold/snapshot persistence via QTest."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import sys
import time
import unittest

OPTIONS = None


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class Machine:
    """Own one CPU-stopped disposable VM; never access a running firmware VM."""
    def __init__(self, soc, folder, snapshot=False, existing=False, timing=False):
        self.soc, self.folder = soc, folder
        folder.mkdir(parents=True, exist_ok=True)
        self.image = folder/'flash.bin'
        if not existing:
            self.image.write_bytes(b'\xff' * (4 * 1024 * 1024))
        self.spi = 0x3ff42000 if soc == 'esp32' else 0x60002000
        self.user, self.user1, self.user2, self.mosi, self.miso, self.w0 = (
            (0x1c, 0x20, 0x24, 0x28, 0x2c, 0x80) if soc == 'esp32'
            else (0x18, 0x1c, 0x20, 0x24, 0x28, 0x58))
        test_port = port()
        self.log = (folder/'qemu.log').open('ab')
        exe = OPTIONS.qemu_xtensa if soc == 'esp32' else OPTIONS.qemu
        bios = OPTIONS.bios
        if soc == 'esp32c2':
            (folder/'esp32c2-rev200-rom.bin').write_bytes(bytes(0x90000))
            bios = str(folder)
        command = [exe, '-M', soc+',default-peripherals=off' if soc == 'esp32c2' else soc,
                   '-accel', 'qtest', '-display', 'none', '-serial', 'none',
                   '-monitor', 'none', '-nic', 'none', '-L', bios,
                   '-drive', f'file={self.image},if=mtd,format=raw',
                   '-qtest', f'tcp:127.0.0.1:{test_port},server=on,wait=off',
                   '-qtest-log', str(folder/'qtest.log')]
        if timing:
            command += ['-global','gd25q32.operation-timing=on']
        if snapshot:
            command.append('-snapshot')
        self.proc = subprocess.Popen(command, stdout=self.log, stderr=self.log,
            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0)
        self.sock = self.stream = None
        try:
            deadline = time.monotonic()+15
            while True:
                try:
                    self.sock = socket.create_connection(('127.0.0.1', test_port), .3)
                    break
                except OSError:
                    if self.proc.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError((folder/'qemu.log').read_text(errors='replace'))
                    time.sleep(.02)
            self.sock.settimeout(5)
            self.stream = self.sock.makefile('rwb', buffering=0)
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.stream:
            self.stream.close()
        if self.sock:
            self.sock.close()
        if self.proc.poll() is None:
            self.proc.terminate()
        self.proc.wait(timeout=5)
        self.log.close()

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()

    def command(self, command):
        self.stream.write((command+'\n').encode())
        while True:
            line = self.stream.readline().decode().strip()
            if line.startswith('IRQ '):
                continue
            if not line.startswith('OK'):
                raise RuntimeError((command, line))
            return line.split()[1:]

    def write(self, address, value):
        self.command(f'writel {address:#x} {value:#x}')

    def read(self, address, size):
        return bytes.fromhex(self.command(f'read {address:#x} {size}')[0][2:])

    def transfer(self, opcode, tx=b'', rx=0, address=None, mode='slow'):
        assert max(len(tx), rx) <= 64
        flags,dummy={'slow':(0,0),'fast':(1<<13,8),'dout':((1<<13)|(1<<14),8),
                     'qout':((1<<13)|(1<<20),8),'dio':((1<<13)|(1<<23),4),
                     'qio':((1<<13)|(1<<24),6)}[mode]
        self.write(self.spi+8,flags)
        user = 1 << 31 | (1 << 27 if tx else 0) | (1 << 28 if rx else 0)
        if address is not None:
            user |= 1 << 30 | (1 << 29 if dummy else 0)
            self.write(self.spi+4, address << 8 if self.soc == 'esp32' else address)
        self.write(self.spi+self.user, user)
        self.write(self.spi+self.user1, 23 << 26 | max(dummy-1,0))
        self.write(self.spi+self.user2, 7 << 28 | opcode)
        self.write(self.spi+self.mosi, max(len(tx)*8-1, 0))
        self.write(self.spi+self.miso, max(rx*8-1, 0))
        for i in range(0, len(tx), 4):
            self.write(self.spi+self.w0+i, int.from_bytes(tx[i:i+4].ljust(4,b'\0'),'little'))
        self.write(self.spi, 1 << 18)
        return self.read(self.spi+self.w0, (rx+3)//4*4)[:rx] if rx else b''

    def ready(self):
        for _ in range(20001):
            if not self.transfer(0x05,rx=1)[0]&1:
                return
            self.command('clock_step 1000000')
        raise AssertionError('Flash remained busy beyond the bounded deadline')

    def program(self, address, data):
        for i in range(0, len(data), 64):
            self.transfer(0x06)
            self.transfer(0x02, tx=data[i:i+64], address=address+i)
            self.ready()

    def erase(self, address, opcode=0x20):
        self.transfer(0x06)
        self.transfer(opcode, address=address)
        self.ready()

    def flash_read(self, address, size):
        return b''.join(self.transfer(0x03, rx=min(64,size-i), address=address+i)
                        for i in range(0,size,64))


class FlashTests(unittest.TestCase):
    def each(self):
        for soc in OPTIONS.socs:
            yield soc, OPTIONS.output.parent/self._testMethodName/soc

    def test_nor_bits_and_write_disable(self):
        """No WREN means no modification; programming can only clear bits."""
        for soc, folder in self.each():
            with self.subTest(soc=soc), Machine(soc,folder) as m:
                m.transfer(0x02,tx=b'\x00',address=0x12000)
                self.assertEqual(m.flash_read(0x12000,1),b'\xff')
                m.program(0x12000,b'\xaa')
                m.program(0x12000,b'\x55')
                self.assertEqual(m.flash_read(0x12000,1),b'\x00')
                m.transfer(0x06)
                m.transfer(0x04)
                m.transfer(0x20,address=0x12000)
                self.assertEqual(m.flash_read(0x12000,1),b'\x00')

    def test_program_clears_wel(self):
        for soc, folder in self.each():
            with self.subTest(soc=soc), Machine(soc,folder) as m:
                m.program(0x12000,b'A')
                self.assertEqual(m.transfer(0x05,rx=1)[0]&3,0)
                m.transfer(0x02,tx=b'B',address=0x12001)
                self.assertEqual(m.flash_read(0x12000,2),b'A\xff')

    def test_erase_clears_wel(self):
        for soc, folder in self.each():
            with self.subTest(soc=soc), Machine(soc,folder) as m:
                m.erase(0x12000)
                self.assertEqual(m.transfer(0x05,rx=1)[0]&3,0)

    def test_page_program_wraps_within_page(self):
        for soc, folder in self.each():
            with self.subTest(soc=soc), Machine(soc,folder) as m:
                m.program(0x120f8,bytes(range(16)))
                self.assertEqual(m.flash_read(0x120f8,8),bytes(range(8)))
                self.assertEqual(m.flash_read(0x12000,8),bytes(range(8,16)))
                self.assertEqual(m.flash_read(0x12100,8),b'\xff'*8)

    def test_sector_and_block_address_alignment(self):
        """Low address bits select a containing erase unit, never shift its bounds."""
        for soc, folder in self.each():
            for opcode, size in ((0x20,4096),(0x52,32768),(0xd8,65536)):
                with self.subTest(soc=soc,opcode=opcode), Machine(soc,folder/str(size)) as m:
                    start = 0x20000
                    for address in (start-1,start,start+size-1,start+size):
                        m.program(address,b'\x00')
                    m.erase(start+17,opcode)
                    self.assertEqual(m.flash_read(start,1),b'\xff')
                    self.assertEqual(m.flash_read(start+size-1,1),b'\xff')
                    self.assertEqual(m.flash_read(start-1,1),b'\x00')
                    self.assertEqual(m.flash_read(start+size,1),b'\x00')

    def test_mapped_cache_invalidation(self):
        """A flash-mapped page must refresh after ROM Cache_Invalidate operations."""
        for soc, folder in self.each():
            if soc == 'esp32':
                continue
            with self.subTest(soc=soc), Machine(soc,folder) as m:
                # C2 reset page size is 16 KiB; C3 pages are fixed at 64 KiB.
                size = 16384 if soc == 'esp32c2' else 65536
                physical, mapped = 0x20000, 0x3c000000
                m.program(physical,b'old!')
                m.write(0x600c5000,physical//size)
                self.assertEqual(m.read(mapped,4),b'old!')
                m.erase(physical)
                m.program(physical,b'new!')
                self.assertEqual(m.flash_read(physical,4),b'new!')
                m.write(0x600c402c,mapped)
                m.write(0x600c4030,32)
                m.write(0x600c4028,1)
                self.assertEqual(m.read(mapped,4),b'new!')
                self.assertEqual(int.from_bytes(m.read(0x600c4028,4),'little')&3,2)
                m.erase(physical)
                m.write(0x600c4028,1)
                self.assertEqual(m.read(mapped,4),b'\xff'*4)

    def test_fast_dual_quad_reads_after_programming(self):
        for soc, folder in self.each():
            if soc == 'esp32':
                continue
            with self.subTest(soc=soc), Machine(soc,folder) as m:
                data=bytes((i*37+11)&255 for i in range(64))
                m.program(0x12000,data)
                self.assertEqual(m.transfer(0x6b,rx=64,address=0x12000,mode='qout'),b'\xff'*64)
                m.transfer(0x06)
                m.transfer(0x31,tx=b'\x02')
                m.ready()
                for mode,opcode in [('fast',0x0b),('dout',0x3b),('qout',0x6b),('dio',0xbb),('qio',0xeb)]:
                    self.assertEqual(m.transfer(opcode,rx=64,address=0x12000,mode=mode),data,mode)

    def test_gd25q32_wip_and_busy_commands(self):
        """GD25Q32C PP=600 us and SE=50 ms use guest virtual clock only."""
        for soc, folder in self.each():
            if soc == 'esp32':
                continue
            with self.subTest(soc=soc), Machine(soc,folder,timing=True) as m:
                m.transfer(0x06)
                m.transfer(0x02,tx=b'A',address=0x12000)
                self.assertEqual(m.transfer(0x05,rx=1)[0]&3,1)
                self.assertEqual(m.flash_read(0x12000,1),b'\xff')
                m.transfer(0x06)
                m.transfer(0x02,tx=b'B',address=0x12001)
                m.command('clock_step 599999')
                self.assertEqual(m.transfer(0x05,rx=1)[0]&3,1)
                m.command('clock_step 1')
                self.assertEqual(m.transfer(0x05,rx=1)[0]&3,0)
                self.assertEqual(m.flash_read(0x12000,2),b'A\xff')
                m.transfer(0x06)
                m.transfer(0x20,address=0x12011)
                self.assertEqual(m.transfer(0x05,rx=1)[0]&3,1)
                m.command('clock_step 49999999')
                self.assertEqual(m.transfer(0x05,rx=1)[0]&3,1)
                m.command('clock_step 1')
                self.assertEqual(m.transfer(0x05,rx=1)[0]&3,0)
                self.assertEqual(m.flash_read(0x12000,2),b'\xff'*2)

    def test_protected_sector_cannot_be_erased(self):
        for soc, folder in self.each():
            with self.subTest(soc=soc), Machine(soc,folder) as m:
                m.program(0x3f0000,b'A')
                m.transfer(0x06)
                m.transfer(0x01,tx=b'\x04')
                m.ready()
                m.program(0x3f0000,b'\x00')
                self.assertEqual(m.flash_read(0x3f0000,1),b'A')
                m.erase(0x3f0000)
                self.assertEqual(m.flash_read(0x3f0000,1),b'A')
                m.transfer(0x06)
                m.transfer(0xc7)
                m.ready()
                self.assertEqual(m.flash_read(0x3f0000,1),b'A')
                m.program(0x12000,b'B')
                self.assertEqual(m.flash_read(0x12000,1),b'B')

    def test_raw_settings_and_cold_persistence(self):
        """Exercise the actual Small TV 444-byte sector ABI and a fresh VM boot."""
        for soc, folder in self.each():
            data = bytes((i*37+11)&255 for i in range(444))
            with self.subTest(soc=soc):
                with Machine(soc,folder) as m:
                    for generation in range(4):
                        m.erase(0x3e7000)
                        m.program(0x3e7000,data)
                        self.assertEqual(m.flash_read(0x3e7000,444),data)
                    deadline = time.monotonic()+5
                    while m.image.read_bytes()[0x3e7000:0x3e71bc] != data:
                        if time.monotonic()>deadline:
                            self.fail('Backing image did not receive completed writes')
                        time.sleep(.01)
                with Machine(soc,folder,existing=True) as m:
                    self.assertEqual(m.flash_read(0x3e7000,444),data)
                    self.assertEqual(m.flash_read(0x3e71bc,64),b'\xff'*64)

    def test_snapshot_preserves_original_image(self):
        for soc, folder in self.each():
            with self.subTest(soc=soc):
                with Machine(soc,folder,snapshot=True) as m:
                    before = hashlib.sha256(m.image.read_bytes()).hexdigest()
                    m.program(0x3e7000,b'snapshot')
                    self.assertEqual(m.flash_read(0x3e7000,8),b'snapshot')
                self.assertEqual(hashlib.sha256((folder/'flash.bin').read_bytes()).hexdigest(),before)
                with Machine(soc,folder,existing=True) as m:
                    self.assertEqual(m.flash_read(0x3e7000,8),b'\xff'*8)


def main():
    global OPTIONS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu',required=True)
    parser.add_argument('--qemu-xtensa')
    parser.add_argument('--bios',required=True)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--socs',nargs='+',default=['esp32c2','esp32c3'])
    OPTIONS = parser.parse_args()
    OPTIONS.output.parent.mkdir(parents=True,exist_ok=True)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(FlashTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    report = dict(passed=result.wasSuccessful(),tests=result.testsRun,socs=OPTIONS.socs,
                  failures=[{'case':str(case),'traceback':trace} for case,trace in result.failures],
                  errors=[{'case':str(case),'traceback':trace} for case,trace in result.errors],
                  qemu_sha256=hashlib.sha256(Path(OPTIONS.qemu).read_bytes()).hexdigest(),
                  test_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),scope=__doc__)
    OPTIONS.output.write_text(json.dumps(report,indent=2)+'\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
