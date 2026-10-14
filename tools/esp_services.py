"""Compile device-profile URL lists into isolated DNS and loopback TCP routes."""
import ipaddress
import json
from pathlib import Path
import re
from urllib.parse import urlsplit


def hostname(value):
    try:
        host = value.removesuffix('.').encode('idna').decode('ascii').lower()
    except (UnicodeError, AttributeError):
        raise ValueError('Service hostname must be a valid DNS name') from None
    if not host or len(host) > 253 or any(
            not re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?', label)
            for label in host.split('.')):
        raise ValueError('Service hostname must be a valid DNS name')
    try:
        ipaddress.ip_address(host)
    except ValueError:
        return host
    raise ValueError('Service URLs must use DNS names for isolated routing')


def service_endpoints(urls):
    """Assign one subnet address per hostname and preserve multiple paths/ports."""
    if not isinstance(urls, list):
        raise ValueError('Service URLs must be a list')
    hosts, endpoints = [], []
    for url in urls:
        if not isinstance(url, str):
            raise ValueError('Each service URL must be a string')
        try:
            parsed = urlsplit(url)
            host = hostname(parsed.hostname or '')
            port = parsed.port
        except ValueError:
            raise ValueError('Service URL must contain a valid HTTP(S) hostname and port') from None
        if port is None:
            port = 443 if parsed.scheme == 'https' else 80
        if (parsed.scheme not in ('http', 'https') or parsed.username is not None or
                parsed.password is not None or parsed.fragment or not 1 <= port <= 65535):
            raise ValueError('Service URL must contain a valid HTTP(S) hostname and port')
        if host not in hosts:
            hosts.append(host)
        if len(hosts) > 155:
            raise ValueError('At most 155 service hostnames fit in the virtual subnet')
        authority = host + (f':{port}' if port != (443 if parsed.scheme == 'https' else 80) else '')
        # Query strings may contain private keys; never put them in VM metadata.
        endpoints.append({'url': f'{parsed.scheme}://{authority}{parsed.path}',
                          'hostname': host, 'port': port, 'suffix': 100 + hosts.index(host)})
    return endpoints


def network_profile(path):
    """All provider names and device SSIDs come from the selected device profile."""
    profile = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    if not isinstance(profile, dict) or profile.get('schema') != 1:
        raise ValueError('Device profile must be an object with schema 1')
    network = profile.get('network', {})
    if not isinstance(network, dict):
        raise ValueError('Profile network must be an object')
    result = dict(network)
    result['urls'] = network.get('urls', [])
    service_endpoints(result['urls'])
    names = network.get('ntp_hosts', [])
    if not isinstance(names, list) or len(names) > 3:
        raise ValueError('Profile ntp_hosts must list at most three DNS names')
    result['ntp_hosts'] = [hostname(name) for name in names]
    if len(set(result['ntp_hosts'])) != len(result['ntp_hosts']):
        raise ValueError('Profile NTP hostnames must be distinct')
    if set(result['ntp_hosts']) & {row['hostname'] for row in service_endpoints(result['urls'])}:
        raise ValueError('NTP and HTTP service hostnames must be distinct')
    for key in ('station_ssid', 'softap_client_ssid'):
        value = network.get(key)
        if value is None:
            result.pop(key, None)
        if value is not None and (not isinstance(value, str) or not 1 <= len(value.encode('utf-8')) <= 32):
            raise ValueError('Profile ' + key + ' must contain 1..32 UTF-8 bytes')
    return result
