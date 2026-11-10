"""Recorded PPOM replay startup, following GameBridge::SyncPlayback and CountdownSlots."""
import json
from pathlib import Path
import sys
import time
from online_protocol import Memory, setup_key, apply_sync, apply_tasks
from harness_paths import WORKSPACE as w
sys.path.insert(0,str(w/'brawlonline.net/tools/gamecode'))
import ppom

def countdown_slots(setup):
    holds=setup['holds']; ports=setup['ports']
    if len(holds)!=4 or not 0<ports<16:raise ValueError('Invalid recorded countdown')
    slots=bytearray(256)
    for port,hold in enumerate(holds):
        if not isinstance(hold,int) or hold & ~0x3f7f:raise ValueError('Invalid recorded held buttons')
        at=64*port
        if not ports & (1<<port):slots[at+0x38]=255;continue
        for off in (0,4):slots[at+off+2:at+off+4]=(hold & 0xf7f).to_bytes(2,'big')
        slots[at+0x3f]=(hold & 0x3000)>>12
    return bytes(slots)

class RecordedSetup:
    def __init__(self, socket, setup, output):
        self.m=Memory(socket); self.setup=setup; self.out=Path(output)
        self.last_status=None;self.last_boot_press=0;self.block=None;self.published={};self.requested=False;self.in_match=False;self.gates=set()
        self.m.call('seed '+str(setup['seed']))
    def publish(self, address, raw):
        raw=bytes(raw)
        if self.published.get(address)==raw[4:]:return
        seq=self.m.u32(address)+1
        self.m.write(address+4,raw[4:]);self.m.w32(address,seq)
        self.published[address]=raw[4:]
    def tick(self):
        phase,scene,*_=self.m.status()
        if (phase,scene)!=self.last_status:
            print('Recorded setup:',phase,scene,flush=True);self.last_status=(phase,scene)
        if scene=='scPrizeInfo' and time.monotonic()-self.last_boot_press>1:
            self.m.call('pad 0 256 0 0 8');self.last_boot_press=time.monotonic()
        if self.block is None:
            # Plugin discovery is only attempted once the game has reached its menu.
            if scene not in ('muMenuMain','scSelctCharacter'):return
            self.block=ppom.find_block(self.m)
            if self.block.version<9:raise RuntimeError('Recorded corpus needs PPOM v9 or newer')
        b=self.block; s=self.setup
        (wr,rd,rc,rs),slots=ppom.read_requests(self.m,b)
        if wr-rd>ppom.REQ_SLOTS:raise RuntimeError('PPOM request ring overflow')
        while rd!=wr:
            seq=rd+1; actual,cmd,payload=slots[(seq-1)%ppom.REQ_SLOTS]
            if actual!=seq or (cmd!=0xba and rc!=rs):break
            if cmd==0xba:
                # CANCEL has no response; offline playback has no matchmaking session.
                rd=seq;ppom.consume(self.m,b,rd);continue
            if cmd!=0xb9:raise RuntimeError(f'Unexpected command in offline replay: {cmd:#x}')
            ppom.write_response(self.m,b,seq,cmd,bytes(ppom.RESP_PAYLOAD))
            rc+=1;rd=seq;ppom.consume(self.m,b,rd)
        local=bytearray(self.m.read(b.local,b.local_size))
        if not self.requested and not self.in_match and local[0x36] in (1,2):self.requested=True
        local[4:8]=bytes((3 if self.in_match else 2,s['local_slot'],1,0))
        local[0x39]=int(self.requested);local[0x3a:0x40]=b'\0\0\0\0\0\3'
        local[0x37:0x39]=b'\0\0';local[0x7d:0x80]=b'\0\0\0'
        self.publish(b.local,local)
        if not self.in_match:
            rec=bytearray.fromhex(s['session'])
            if len(rec)>b.session_size:raise RuntimeError('Recorded SESSION larger than plugin SESSION')
            rec.extend(bytes(b.session_size-len(rec)));rec[4]=2
            rec[0x20c:0x210]=bytes(4);rec[0x21b]=0
            for port in range(4):rec[0x0c+port*0x80+0x37]=0
            self.publish(b.session,rec)
        if phase in self.gates:return
        if phase=='css':self.gates.add(phase);self.m.call('release')
        elif phase=='stage':
            addr=self.m.u32(self.m.u32(0x805a00e0)+8)+8
            mine=bytearray(self.m.read(addr,32));want=bytes.fromhex(s['setup_key'])[:32]
            # init was captured before finalized mode flags; setup_key records
            # the committed match setup that both engines must reproduce.
            if any(mine[i]!=want[i] for i in range(32) if i!=0x14):
                raise RuntimeError(f'Recorded init block differs: {mine.hex()} != {want.hex()}')
            mine[0x14]=want[0x14];self.m.write(addr,mine)
            self.gates.add(phase);self.m.call('release')
        elif phase=='barrier':
            key=setup_key(self.m);want=bytes.fromhex(s['setup_key'])
            (self.out/'recorded-setup-check.json').write_text(json.dumps(dict(actual=key.hex(),recorded=want.hex(),equal=key==want),indent=2))
            if key!=want:raise RuntimeError('Recorded setup key differs; see recorded-setup-check.json')
            apply_tasks(self.m,s['tasks']);apply_sync(self.m,s['sync'])
            self.in_match=True;self.gates.add(phase);self.m.call('release')
        elif phase=='start':
            self.gates.add(phase);self.m.call('go');self.m.call('release')
    def close(self):self.m.close()
