#!/usr/bin/env python3
"""Validate an ESP board profile and compile native QEMU module arguments."""
import argparse
import json
from pathlib import Path
import re
from esp_services import network_profile, service_endpoints
from small_tv_web import user_network_options

CAPABILITIES = {
    'esp32c2': {'pins':set(range(21))-set(range(11,18)), 'spi':(2,), 'i2c':(0,), 'gpio':'/machine/gpio', 'root':'/machine', 'machine':'esp32c2,default-peripherals=off'},
    'esp32c3': {'pins':set(range(22))-set(range(12,18)), 'spi':(2,), 'i2c':(0,), 'gpio':'/machine/gpio', 'root':'/machine', 'machine':'esp32c3,default-peripherals=off'},
    'esp32': {'pins':set(range(40))-set(range(6,12))-{20,24,28,29,30,31}, 'spi':(2,3), 'i2c':(0,1), 'gpio':'/machine/soc/gpio', 'root':'/machine/soc', 'machine':'esp32,modular=on'},
}

def integer(value, what):
    if isinstance(value,bool) or not isinstance(value,(int,str)):
        raise ValueError(what+' must be an integer')
    try: return int(value,0) if isinstance(value,str) else value
    except ValueError: raise ValueError(what+' must be an integer')

def compile_profile(path, expected_soc=None, network='offline', service_port=0, service_urls=None):
    path=Path(path).resolve()
    profile=json.loads(path.read_text(encoding='utf-8-sig'))
    if not isinstance(profile,dict): raise ValueError('Profile must be an object')
    if profile.get('schema') != 1: raise ValueError('Unsupported hardware profile schema')
    soc=profile.get('soc')
    if soc not in CAPABILITIES or expected_soc and soc != expected_soc:
        raise ValueError('Hardware profile SoC does not match the selected machine')
    cap=CAPABILITIES[soc]
    devices=profile.get('devices')
    if not isinstance(devices,list): raise ValueError('devices must be an array')
    ids={f'spi{x}' for x in cap['spi']} | {f'i2c{x}' for x in cap['i2c']}; cs_by_bus={}; owners={}; argv=[]; disks=[]
    for node in devices:
        if not isinstance(node,dict): raise ValueError('Each device must be an object')
        name=node.get('id','')
        if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_-]{0,63}',name) or name in ids:
            raise ValueError('Module IDs must be unique identifiers')
        ids.add(name)
        bus=node.get('bus',''); model=node.get('model','')
        if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_.-]{0,63}',model): raise ValueError('Invalid device model')
        match=re.fullmatch(r'(spi|i2c)(\d+)',bus)
        if not match or int(match[2]) not in cap[match[1]]:
            raise ValueError(f'{bus} is not available for external devices on {soc}')
        kind=match[1]; host=int(match[2]); pins=node.get('pins',{})
        if not isinstance(pins,dict): raise ValueError('pins must be an object')
        roles=('sck','mosi','miso','cs','dc','reset','backlight') if kind=='spi' else ('scl','sda')
        if set(pins)-set(roles): raise ValueError(f'{name}: unsupported pin role')
        values={role:integer(pins.get(role,-1),name+'.'+role) for role in roles}
        for role,pin in values.items():
            if pin != -1 and pin not in cap['pins']: raise ValueError(f'{name}: GPIO{pin} is invalid or reserved for flash')
            if soc=='esp32' and pin>=34 and role!='miso': raise ValueError(f'GPIO{pin} is input-only')
            if pin>=0:
                owner=(bus,role) if role in ('sck','mosi','miso','dc','reset','backlight','scl','sda') else (name,role)
                if pin in owners and owners[pin]!=owner: raise ValueError(f'GPIO{pin} has conflicting connections')
                owners[pin]=owner
        required=('sck','mosi') if kind=='spi' else ('scl','sda')
        if any(values[k]<0 for k in required): raise ValueError(name+': missing bus GPIOs')
        properties=node.get('properties',{})
        if not isinstance(properties,dict): raise ValueError('properties must be a JSON object')
        if kind=='spi':
            if model=='st7789' and values['dc']<0: raise ValueError('ST7789 requires a D/C GPIO')
            if model=='sd' and (values['miso']<0 or values['cs']<0): raise ValueError('SD requires MISO and CS GPIOs')
            selections=cs_by_bus.setdefault(bus,[])
            selections.append(values['cs'])
            if len(selections)>1 and -1 in selections: raise ValueError('An always-selected device cannot share an SPI bus')
            dev={'driver':'esp-spi-module','id':name,'bus':f'/spi{host}/spi','gpio':cap['gpio'],'spi-host':host,'cs':len(selections)-1,'model':model,'sck':values['sck'],'mosi':values['mosi'],'miso':values['miso'],'cs-gpio':values['cs'],'dc-gpio':values['dc'],'reset-gpio':values['reset'],'backlight-gpio':values['backlight'],'backlight-active-low':bool(node.get('backlight_active_low',False))}
            if model=='sd':
                image=Path(node.get('image',''))
                if not image.is_absolute(): image=path.parent/image
                image=image.resolve()
                if not image.is_file(): raise ValueError('SD image does not exist: '+str(image))
                backend='disk_'+name
                argv+=['-drive',f'file={str(image).replace(",",",,")},if=none,id={backend},format=raw,snapshot=on']
                dev['drive']=backend
                disks.append(str(image))
        else:
            address=integer(node.get('address',0x3c),name+'.address')
            if not 0<address<0x80: raise ValueError('I2C address must be 1..127')
            key=(bus,address)
            if key in ids: raise ValueError('Duplicate I2C address')
            ids.add(key)
            dev={'driver':'esp-i2c-module','id':name,'bus':f'/i2c{host}/i2c','gpio':cap['gpio'],'address':address,'model':model,'scl':values['scl'],'sda':values['sda']}
            if soc=='esp32': dev.update({'scl-signal':29+host*2,'sda-signal':30+host*2})
        if properties: dev['properties']=json.dumps(properties,separators=(',',':'))
        argv+=['-device',json.dumps(dev,separators=(',',':'))]
    config = network_profile(path)
    urls = config['urls'] if service_urls is None else service_urls
    services = service_endpoints(urls)
    if set(config['ntp_hosts']) & {row['hostname'] for row in services}:
        raise ValueError('NTP and HTTP service hostnames must be distinct')
    user_network_options(network, [], service_port, urls)
    network_args = []
    if network != 'offline':
        if soc not in ('esp32c2', 'esp32c3'):
            raise ValueError('Profile service fixtures currently require ESP32-C2 or ESP32-C3')
        network_args = ['-global', 'esp32c3-wifi.clock-services=on',
                        '-global', 'esp32c3-wifi.ntp-hosts=' + ';'.join(config['ntp_hosts']),
                        '-global', 'esp32c3-wifi.station-ssid=' + config.get('station_ssid', 'QEMU'),
                        '-nic', user_network_options(network, [], service_port, urls)]
        if service_port:
            hosts = list(dict.fromkeys(row['hostname'] for row in services))
            network_args += ['-global', 'esp32c3-wifi.service-hosts=' + ';'.join(hosts)]
        if network == 'setup' and config.get('softap_client_ssid'):
            network_args += ['-global', 'esp32c3-wifi.softap-client-ssid=' + config['softap_client_ssid']]
    metadata = dict(config, urls=[row['url'] for row in service_endpoints(config['urls'])])
    return {'schema':1,'name':profile.get('name'),'soc':soc,'machine':cap['machine'],'arguments':argv,'sd_images':disks,
            'profile':str(path), 'network':metadata, 'network_arguments':network_args,
            'service_urls':[row['url'] for row in services] if service_port else []}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile');parser.add_argument('--soc');parser.add_argument('--output')
    parser.add_argument('--network', choices=['offline', 'clock', 'setup'], default='offline')
    parser.add_argument('--service-port', type=int, default=0)
    parser.add_argument('--service-url', dest='service_urls', action='append')
    args=parser.parse_args()
    result=compile_profile(args.profile,args.soc,args.network,args.service_port,args.service_urls)
    output=json.dumps(result,indent=2)
    if args.output: Path(args.output).write_text(output+'\n',encoding='utf-8')
    else: print(output)

if __name__=='__main__':
    try: main()
    except (ValueError,OSError) as exc: raise SystemExit(str(exc))
