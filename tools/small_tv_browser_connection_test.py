"""Observe owned guest sockets/real Chrome loads, varying only an extra probe connection."""
from pathlib import Path
import argparse,hashlib,json,re,struct,subprocess,sys,time
from types import SimpleNamespace
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--yoradio',type=Path,required=True)
parser.add_argument('--dependency-root',type=Path,required=True)
parser.add_argument('--qemu-root',type=Path,default=Path(__file__).resolve().parents[1])
parser.add_argument('--qemu',type=Path)
parser.add_argument('--node',type=Path,required=True)
parser.add_argument('--playwright-modules',type=Path,required=True)
parser.add_argument('--browser-executable',type=Path,required=True)
parser.add_argument('--rounds',type=int,default=10)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
ROOT=args.yoradio.resolve();QEMU=args.qemu_root.resolve();BASE=Path(__file__).resolve().parent;OUT=args.output.resolve()
if not 1<=args.rounds<=100:raise ValueError('Use 1..100 rounds')
sys.path[:0]=[str(QEMU/'tools'),str(args.dependency_root/'.build/small-tv-parity/oracle-deps')]
import small_tv_vm as vm
from small_tv_http_test import http,wait_http,clean_uart
from elftools.elf.elffile import ELFFile
OUT.mkdir(exist_ok=False)
elf=ROOT/'.build/small-tv-esp32c2/small_tv_reconstructed.elf'
with elf.open('rb') as stream:
 e=ELFFile(stream);symbols=[s for s in e.get_section_by_name('.symtab').iter_symbols() if s.name=='sockets']
 assert len(symbols)==1
 address,length=symbols[0]['st_value'],symbols[0]['st_size'];types={}
 for cu in e.get_dwarf_info().iter_CUs():
  for die in cu.iter_DIEs():
   a=die.attributes.get('DW_AT_name');name=a.value.decode() if a and isinstance(a.value,bytes) else ''
   if die.tag=='DW_TAG_structure_type' and name in ('lwip_sock','netconn','tcp_pcb','udp_pcb') and name not in types and 'DW_AT_byte_size' in die.attributes:
    types[name]={'size':die.attributes['DW_AT_byte_size'].value,'fields':{x.attributes['DW_AT_name'].value.decode():x.attributes['DW_AT_data_member_location'].value for x in die.iter_children() if x.tag=='DW_TAG_member' and 'DW_AT_data_member_location' in x.attributes}}
assert len(types)==4 and length%types['lwip_sock']['size']==0
socket_count=length//types['lwip_sock']['size'];assert 1<=socket_count<=64
profile=OUT/'profile.json'
fixture=json.loads((ROOT/'Small_TV/qemu/profiles/small-tv-c2.json').read_text())
fixture['network']['station_ssid']='manual'
vm.save_json(profile,fixture)
options=SimpleNamespace(yoradio=str(ROOT),qemu=str(args.qemu or QEMU/'bin/qemu-system-riscv32.exe'),
 rom_elf=str(args.dependency_root/'.idf/tools-v6.1/tools/esp-rom-elfs/20260528/esp32c2_rev200_rom.elf'),
 hardware_profile=str(profile),network='setup',display='none',persistent=True,paused=True,gdb_port=0,http_base_port=0,firmware='reconstructed')
folder=OUT/'vm';vm.prepare(options,folder)
image=bytearray((folder/'flash.bin').read_bytes())
image[0x3e7004:0x3e7045]=b'manual'.ljust(65,b'\0')
for offset,value in [(0x160,1),(0x164,3),(0x168,1),(0x16c,1),(0x170,1),(0x174,1),(0x17c,2),(0x180,470),(0x184,0x07e0),(0x188,0x001f),(0x18c,0xffe0)]:
 struct.pack_into('<I',image,0x3e7000+offset,value)
(folder/'flash.bin').write_bytes(image)
inputs=[ROOT/'firmware/development/small-tv-esp32c2-reconstructed/app.bin',elf,BASE/'small_tv_browser_connection_test.mjs',Path(__file__),profile,Path(options.qemu),Path(options.rom_elf),args.node,args.browser_executable] + [QEMU/'tools'/name for name in ('small_tv_vm.py','small_tv_http_test.py','small_tv_network.py','small_tv_web.py','esp_board.py','esp_services.py','esp_elf.py')]
protected=[(0,0x9000),(0xf000,0x3e7000),(0x3e8000,0x400000)]
protected_before=[hashlib.sha256(image[a:b]).hexdigest() for a,b in protected]
report={'finished':False,'scope':__doc__,'inputs_sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},'layout':{'address':address,'bytes':length,'types':types},'samples':[]}

def read(qmp,addr,size):
 """Read a bounded physical RAM range; never modify the guest."""
 assert 0x3fca0000<=addr<addr+size<=0x3fce0000
 text=qmp.execute('human-monitor-command',**{'command-line':f'xp /{size}bx {addr:#x}'})
 data=b''.join(bytes.fromhex(v) for line in text.splitlines() if ':' in line for v in re.findall(r'0x([0-9a-fA-F]{2})(?=\s|$)',line.split(':',1)[1]))
 assert len(data)==size
 return data

def snapshot(qmp):
 """Decode allocated descriptors through the exact ELF's DWARF layout."""
 qmp.execute('stop')
 try:
  data=read(qmp,address,length);rows=[];sock=types['lwip_sock'];net=types['netconn']
  for index in range(socket_count):
   raw=data[index*sock['size']:(index+1)*sock['size']];conn=struct.unpack_from('<I',raw,sock['fields']['conn'])[0]
   if not conn:continue
   c=read(qmp,conn,net['size']);typ=struct.unpack_from('<I',c,net['fields']['type'])[0];pcb=struct.unpack_from('<I',c,net['fields']['pcb'])[0]
   row={'slot':index,'type':typ,'fd_used':raw[sock['fields']['fd_used']],'closing':raw[sock['fields']['fd_free_pending']]}
   if pcb and typ&0xf0 in (0x10,0x20):
    layout=types['tcp_pcb' if typ&0xf0==0x10 else 'udp_pcb'];b=read(qmp,pcb,72 if typ&0xf0==0x10 else 64)
    row['local_port']=struct.unpack_from('<H',b,layout['fields']['local_port'])[0]
    if typ&0xf0==0x10:row['tcp_state']=struct.unpack_from('<I',b,layout['fields']['state'])[0]
    if row.get('tcp_state')!=1:row['remote_port']=struct.unpack_from('<H',b,layout['fields']['remote_port'])[0]
   rows.append(row)
  return rows
 finally:qmp.execute('cont')

session=None;process=None
try:
 session,_=vm.launch(options,folder);report['session']={k:session[k] for k in ('name','pid','web_urls')}
 with vm.QMP(session['qmp_port'],session['name']) as qmp:
  network=qmp.execute('human-monitor-command',**{'command-line':'info network'});report['network_info']=network
  backends=re.findall(r'(#\S+): index=\d+,type=user',network);assert len(backends)==1
  qmp.execute('object-add',**{'qom-type':'filter-dump','id':'browsertrace','netdev':backends[0],'file':str(OUT/'network.pcap')})
  qmp.execute('qom-set',path='/machine/wifi',property='ntp-unix-seconds',value=1792324800);qmp.execute('cont')
 wait_http(session['web_urls']['station'],expected=b'/setclocktype')
 expected={}
 for path in ['/getgifvalue','/getclocktype','/GetTimeModeValue','/get_led_brightness','/get_clock_colors','/getsettings']:
  status,body=http(session['web_urls']['station'],path)
  assert status==200,path
  expected[path]=json.loads(body)
 vm.save_json(OUT/'expected.json',expected)
 command=[str(args.node),str(BASE/'small_tv_browser_connection_test.mjs'),
  '--url',session['web_urls']['station']+'/', '--rounds',str(args.rounds),'--output',str(OUT),
  '--modules',str(args.playwright_modules),'--browser',str(args.browser_executable)]
 with (OUT/'node.stderr.log').open('w') as err:
  process=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,text=True,encoding='utf-8')
  while True:
   line=process.stdout.readline()
   if not line:break
   event=json.loads(line)
   with vm.QMP(session['qmp_port'],session['name']) as qmp:event['sockets']=snapshot(qmp)
   report['samples'].append(event)
   if event['phase']=='after-page':print(event['round'],event['mode'],'errors',event['errors'],'sockets',len(event['sockets']),flush=True)
   process.stdin.write('continue\n');process.stdin.flush()
  code=process.wait(timeout=20);assert code==0,(OUT/'node.stderr.log').read_text()
 with vm.QMP(session['qmp_port'],session['name']) as qmp:qmp.execute('object-del',id='browsertrace')
 report['finished']=True
 report['browser']=json.loads((OUT/'browser.json').read_text())
 report['passed']=all(x['passed'] for x in report['browser']['rows'])
except Exception as error:
 report['error']=str(error);raise
finally:
 if process and process.poll() is None:process.terminate();process.wait(timeout=10)
 if session:vm.control(folder,'stop');vm.ensure_stopped(folder)
 report['uart_clean']=clean_uart(folder)
 report['rom_boots']=(folder/'uart.log').read_bytes().count(b'ESP-ROM:esp32c2')
 after=(folder/'flash.bin').read_bytes()
 report['protected_flash_unchanged']=[hashlib.sha256(after[a:b]).hexdigest() for a,b in protected]==protected_before
 report['inputs_unchanged']=all(hashlib.sha256(Path(p).read_bytes()).hexdigest()==h for p,h in report['inputs_sha256'].items())
 report['passed']=report.get('passed',False) and report['uart_clean'] and report['rom_boots']==1 and report['protected_flash_unchanged'] and report['inputs_unchanged']
 vm.save_json(OUT/'report.json',report)
raise SystemExit(0 if report['passed'] else 1)
