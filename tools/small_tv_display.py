"""Read and bound Small TV framebuffer observations independently of networking."""
import hashlib
import time


def ppm_pixels(path):
    """Require the actual 240x240 RGB panel, including the complete raster."""
    data = path.read_bytes()
    header, width_height, maximum, pixels = data.split(b'\n', 3)
    if header != b'P6' or maximum != b'255' or width_height != b'240 240' or len(pixels) != 240*240*3:
        raise AssertionError('Unexpected LCD framebuffer dimensions/format')
    return pixels


def wait_for_display_recovery(qmp, state, virtual_us, host_timeout=30, guest_timeout=5):
    """Observe only the real panel, with independent host and guest deadlines.

    FIXED_TEST: a network callback does not imply a finished display update.
    Retain the existing >1000 nonzero RGB-byte criterion, permit intermediate
    clears, and reject a permanently blank panel or a stalled guest. Neither
    Wi-Fi/SNTP flags nor ROM banners can make this display check pass.
    """
    started, initial = time.monotonic(), virtual_us()
    samples = []
    frame = state / 'after-reset.ppm'
    while True:
        qmp.execute('screendump', filename=str(frame))
        pixels = ppm_pixels(frame)
        sample = {'host_seconds': time.monotonic() - started,
                  'guest_seconds': ((virtual_us() - initial) & 0xffffffff) / 1e6,
                  'nonzero_bytes': sum(value != 0 for value in pixels),
                  'frame_sha256': hashlib.sha256(frame.read_bytes()).hexdigest()}
        samples.append(sample)
        expired = sample['host_seconds'] >= host_timeout or sample['guest_seconds'] >= guest_timeout
        ready = sample['nonzero_bytes'] > 1000
        if expired or ready:
            return {'method': 'bounded_post_reset_framebuffer', 'passed': ready and not expired,
                    **sample, 'polls': len(samples), 'host_timeout': host_timeout,
                    'guest_timeout': guest_timeout, 'samples': samples}
        time.sleep(.1)
