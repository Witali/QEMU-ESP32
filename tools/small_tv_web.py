"""Reserve consecutive loopback ports for a VM's real guest HTTP endpoints."""
import socket


def web_targets(network):
    """Keep setup and station services separate, regardless of their guest IPs."""
    if network == 'setup':
        return [('setup', '192.168.43.1'), ('station', '192.168.43.15')]
    if network == 'clock':
        return [('station', '10.0.2.15')]
    if network == 'offline':
        return []
    raise ValueError('Unknown Small TV network profile')


def reserve_web_ports(network, base=0):
    """Preflight a contiguous port block; QEMU owns the listeners after launch.

    Zero picks the first available block at/above 18080; -1 disables forwarding.
    An explicit positive base is never silently replaced by another range.
    """
    targets = web_targets(network)
    if not -1 <= base <= 65535:
        raise ValueError('HTTP base port must be -1..65535')
    if base == -1 or not targets:
        return [], []
    count = len(targets)
    candidates = range(18080, 65536 - count + 1, count) if base == 0 else [base]
    for first in candidates:
        if first + count - 1 > 65535:
            raise ValueError('HTTP port block exceeds port 65535')
        held = []
        try:
            for index in range(count):
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                held.append(sock)
                if hasattr(socket, 'SO_EXCLUSIVEADDRUSE'):
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                sock.bind(('127.0.0.1', first + index))
            routes = [{'name': name, 'guest_address': address, 'guest_port': 80,
                       'host_address': '127.0.0.1', 'host_port': first + index,
                       'url': f'http://127.0.0.1:{first + index}'}
                      for index, (name, address) in enumerate(targets)]
            return routes, held
        except OSError:
            for sock in held:
                sock.close()
            if base:
                raise ValueError(f'HTTP loopback port block {base}..{base + count - 1} is busy')
    raise ValueError('No free consecutive HTTP loopback ports')


def user_network_options(network, routes, weather_port=0):
    """Build actual SLIRP forwards; no host-side replacement HTTP server is used."""
    if not isinstance(weather_port, int) or not 0 <= weather_port <= 65535:
        raise ValueError('Weather fixture port must be 0..65535')
    if weather_port and network not in ('clock', 'setup'):
        raise ValueError('Weather fixture needs a station-capable network')
    options = 'user,model=esp32c3-wifi,restrict=on'
    if network == 'setup':
        options += ',net=192.168.43.0/24,host=192.168.43.2,dns=192.168.43.3,dhcpstart=192.168.43.15'
    for route in routes:
        options += f",hostfwd=tcp:127.0.0.1:{route['host_port']}-{route['guest_address']}:80"
    if weather_port:
        address = '192.168.43.100' if network == 'setup' else '10.0.2.100'
        options += f',guestfwd=tcp:{address}:80-tcp:127.0.0.1:{weather_port}'
    return options
