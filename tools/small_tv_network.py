"""Synthetic station/setup settings for the isolated Small TV QEMU networks."""
import json
from pathlib import Path
import struct

from esp_services import network_profile

DEFAULT_PROFILE = Path(__file__).resolve().parents[1] / 'configs/boards/small-tv-c2.json'


def seed_network(data, variant, yoradio, setup, ssid=None):
    """Clear private NVS/settings and preserve every application/media byte."""
    data = bytearray(data)
    if ssid is None:
        ssid = network_profile(DEFAULT_PROFILE).get('station_ssid', 'QEMU')
    start = 0x3e7000
    if variant == 'original':
        sizes = struct.unpack_from('<5I', data, start + 0x1a8)
    elif variant == 'reconstructed':
        manifest = json.loads((Path(yoradio) / 'firmware/development/small-tv-esp32c2-reconstructed/assets-manifest.json').read_text())
        sizes = [row['size'] for row in manifest['records'][:5]]
    else:
        raise ValueError('Unknown firmware variant')
    data[0x9000:0xf000] = b'\xff' * 0x6000
    raw = bytearray(4096)
    for offset, width, value in [(4, 65, '' if setup else ssid), (0x45, 65, ''),
                                (0x86, 65, '192.168.43.2' if setup else '10.0.2.2'),
                                (0xc7, 20, 'CN'), (0xdb, 65, 'Shenzhen'),
                                (0x11c, 65, 'disabled')]:
        raw[offset:offset + width] = value.encode().ljust(width, b'\0')
    # Recovered raw-sector ABI; native settings import it before older NVS.
    values = [2 if setup else 1, 0, 0, 0, 0, 0, 0, 2, 0,
              0xf800, 0xffe0, 0xffff, 0, 22, 9, 500, 5, 31]
    struct.pack_into('<18I', raw, 0x160, *values)
    struct.pack_into('<I', raw, 0, 31)
    struct.pack_into('<5I', raw, 0x1a8, *sizes)
    data[start:start + 4096] = raw
    return data


def seed_clock_network(data, variant, state, yoradio, ssid=None):
    """Boot directly into STA on the usual 10.0.2.0/24 virtual network."""
    return seed_network(data, variant, yoradio, False, ssid)


def seed_setup_network(data, variant, state, yoradio):
    """Boot without saved credentials; the real firmware starts its setup AP."""
    return seed_network(data, variant, yoradio, True)
