"""Apply recorded match setup through the recomp control socket."""
import collections,ctypes,json,socket,struct,threading,time
from pathlib import Path

class Memory:
 def __init__(self,path,token=None):
  if token is None:
   self.sock=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);self.sock.settimeout(15);self.sock.connect(str(path))
  else:
   port=int(Path(path).read_text().strip())
   if not 0<port<=65535:raise ValueError('Invalid control endpoint port')
   self.sock=socket.create_connection(('127.0.0.1',port),timeout=15)
   self.sock.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
  self.file=self.sock.makefile('rb')
  if token is not None:self.call('auth '+token)
 def call(self,line):
  self.sock.sendall((line+'\n').encode());s=self.file.readline().decode().strip()
  if not s or s.startswith('error'):raise RuntimeError(s or 'recomp IPC closed')
  return s
 def read(self,a,n):return b''.join(bytes.fromhex(self.call(f'read {a+i:x} {min(n-i,16384):x}')) for i in range(0,n,16384))
 def write(self,a,b):self.call(f'write {a:x} {b.hex()}')
 def u32(self,a):return struct.unpack('>I',self.read(a,4))[0]
 def w32(self,a,v):self.write(a,struct.pack('>I',v))
 def status(self):return self.call('status').split()
 def close(self):self.file.close();self.sock.close()
 # Adapter for the upstream PPOM inspection helpers.
 read_mem=read
 write_mem=write
 read_u32=u32

def setup_key(m):
 gg=m.u32(0x805A00E0);mm=m.u32(gg+8);p=m.read(mm,0x320);key=bytearray(p[8:40])
 for i in range(4):
  pl=p[0x98+i*0x5c:];key.extend(pl[k] for k in (0,1,4,5))
  if p[0x13]:key.append(pl[0xb])
 cfg=m.read(0x805B7480,0xb9)
 if any(x>=4 and x<8 for x in cfg[0xb5:0xb9]):raise RuntimeError('test client currently requires GameCube layouts')
 key.extend(cfg[:48]);key.extend(cfg[0xb5:0xb9]);rule=m.read(m.u32(gg+0x1c),16);key.extend(rule)
 if rule[5]==2:
  for i in range(4):key.extend(p[0x98+i*0x5c+0x26:0x98+i*0x5c+0x28])
 key.append(p[0x29]&0x20);key.extend(p[0x30:0x40]);menu=m.read(0x804e0000,0x2520)
 for o in range(0,len(menu)-0x20+1,4):
  size=int.from_bytes(menu[o:o+2],'big');typ=menu[o+2];text=menu[o+6]
  if size%4 or not 0x20<=size<=0x400 or o+size>len(menu) or typ>2 or not 0x1c<=text<size:continue
  options=int.from_bytes(menu[o+0x18:o+0x1c],'big')
  if (typ==0 and not 0x804e0000<=options<0x804e2520) or (typ!=0 and text!=0x20):continue
  t=menu[o+text:o+size];percent=t.find(b'%')
  if not t[:1].isalpha() or percent<0 or any(x<32 or x>=127 for x in t[:percent]) or b'Character Select' in t[:percent]:continue
  key.extend(menu[o+8:o+12])
 # This prototype selects ordinary versus; reject special modes rather than hash incompletely.
 special=m.read(m.u32(gg+0x1c)+0x18,8)
 if any(special):raise RuntimeError('special modes are outside this first private-client test')
 return bytes(key)

def apply_sync(m,b):
 raw=bytes.fromhex(b['game_frame'])
 if len(raw)!=24 or len(b['rng'])!=3 or sorted(b['start_pos'])!=list(range(4)):raise RuntimeError('invalid host sync block')
 for off in (4,12,20):m.write(0x901812a0+off,raw[off:off+4])
 m.w32(m.u32(0x8059ffac)+0x100,b['app'])
 for a,v in zip((0x805a00bc,0x805a0424,0x8059ff58),b['rng']):m.w32(a,v)
 m.w32(0x8059c668,b['serial']);stage=m.u32(0x80b8a428)
 for i,v in enumerate(b['start_pos']):m.w32(stage+0x1b4+4*i,v)

def apply_tasks(m,want):
 if len(want)!=71:raise RuntimeError('invalid task list count')
 sched=m.u32(0x805a0068);li=0;changed=0
 for base,prev,nxt,count in ((0x14,4,8,17),(0x58,12,16,40),(0xf8,20,24,14)):
  for i in range(count):
   seq=[];t=m.u32(sched+base+4*i);seen=set()
   while t:
    if t in seen or len(seq)>=4096:raise RuntimeError('invalid native task chain')
    seen.add(t);name=m.read(m.u32(t),48).split(b'\0')[0].decode();seq.append((name,t));t=m.u32(t+nxt)
   names=[n for n,_ in seq]
   if collections.Counter(names)!=collections.Counter(want[li]):raise RuntimeError(f'task set differs in list {li}: {names} vs {want[li]}')
   if names!=want[li]:
    pool=collections.defaultdict(collections.deque)
    for n,a in seq:pool[n].append(a)
    order=[pool[n].popleft() for n in want[li]];m.w32(sched+base+4*i,order[0] if order else 0)
    for j,a in enumerate(order):m.w32(a+prev,order[j-1] if j else 0);m.w32(a+nxt,order[j+1] if j+1<len(order) else 0)
    changed+=1
   li+=1
 return changed
