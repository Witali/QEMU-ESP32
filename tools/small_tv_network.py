"""Synthetic Small TV settings for the isolated QEMU clock network."""
import ctypes
from pathlib import Path
import struct
import subprocess

SSID = 'yoRadio-QEMU'

def seed_clock_network(data, variant, state, yoradio):
    data = bytearray(data)
    # Never carry hardware calibration, owner MAC or stored credentials into
    # a networked VM. Guest application and asset bytes are preserved.
    data[0x9000:0xf000] = b'\xff' * 0x6000
    if variant == 'original':
        start = 0x3e7000
        data[start:start + 0x1000] = b'\xff' * 0x1000
        def string(offset, size, value):
            data[start + offset:start + offset + size] = value.encode() + b'\0' * (size - len(value))
        string(4, 65, SSID)
        string(0x45, 65, '')
        string(0x86, 65, '10.0.2.2')
        string(0xc7, 20, 'CN')
        string(0xdb, 65, 'Shenzhen')
        # Nonempty sentinel prevents fallback to the vendor's compiled key.
        string(0x11c, 65, 'disabled')
        struct.pack_into('<I', data, start + 0x160, 1) # station mode
    else:
        class Settings(ctypes.LittleEndianStructure):
            _fields_ = [('version', ctypes.c_uint32)] + [
                (name, ctypes.c_char * size) for name, size in (
                    ('ssid',33),('password',65),('city',65),('country',3),('ntp',65),('key',65))
            ] + [('levels',ctypes.c_uint16 * 5),('flags',ctypes.c_uint8 * 13),('photos',ctypes.c_uint32 * 5)]
        settings = Settings()
        settings.version = 1
        settings.ssid = SSID.encode()
        settings.city, settings.country, settings.ntp = b'Shenzhen', b'CN', b'10.0.2.2'
        settings.levels[:] = [0,500,0xf800,0xffe0,0xffff]
        settings.flags[:] = [0,0,2,22,9,5,0,0,0,0,0,0,31]
        settings.photos[:] = [69565,100509,87740,66439,85691]
        (state / 'settings.bin').write_bytes(bytes(settings))
        csv = state / 'nvs.csv'
        # CSV quoting accommodates commas in a user-selected state path.
        import csv as csv_module
        with csv.open('w', newline='', encoding='utf-8') as stream:
            writer = csv_module.writer(stream)
            writer.writerows([['key','type','encoding','value'],['small_tv','namespace','',''],
                             ['configuration','file','binary',str(state / 'settings.bin')]])
        python = Path(yoradio) / '.idf/tools-v6.1/python_env/idf6.1_py3.12_env/Scripts/python.exe'
        if not python.is_file():
            raise ValueError('Clock network requires the pinned ESP-IDF Python environment for NVS generation')
        result = subprocess.run([str(python),'-m','esp_idf_nvs_partition_gen','generate',
                                 str(csv),str(state / 'nvs.bin'),'0x6000'], capture_output=True, text=True)
        if result.returncode:
            raise ValueError('Could not generate synthetic NVS: ' + result.stderr[-1000:])
        data[0x9000:0xf000] = (state / 'nvs.bin').read_bytes()
    return data
