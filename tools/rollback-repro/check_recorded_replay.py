#!/usr/bin/env python3
"""Validate an original corpus replay using the reference engine's replay player."""
import argparse
import json
import os
import shutil
from pathlib import Path
import sys
import time

from harness_paths import WORKSPACE as w
sys.path[:0] = [str(w/'brawlonline.net/harness'), str(w/'brawlonline.net/harness/tools')]
from host_platform import require_display
import gameplay_rollback as G
from ppharness import brawl as B
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--match', required=True)
p.add_argument('--name', required=True)
p.add_argument('--rollback-benchmark',action='store_true')
p.add_argument('--frames',type=int,default=600)
a = p.parse_args()
for key in ('PPHARNESS_DOLPHIN','PPHARNESS_TEMPLATE','PPHARNESS_ISO','PPHARNESS_INSTANCES','BRAWL_SHARED_SD'):
 if not os.environ.get(key):raise ValueError(f'{key} must be configured')
require_display()
replay = w/'replay-corpus'/f'match-{a.match}'/'game.brep'
if not replay.is_file(): raise FileNotFoundError(replay)
out = w/'artifacts'/a.name
out.mkdir()
d = G.make_instance('recorded-reference', video=os.environ['PPHARNESS_VIDEO'], keep=False,
 dolphin_ini={'General':{'WiiSDCardPath':os.environ.get('BRAWL_SHARED_SD',str(w/'artifacts/online-loop-sd.raw'))},
 'Core':{'WiiSDCardAllowWrites':False,'WiiSDCardEnableFolderSync':False}})
# The selected display/backend is supplied by the experiment environment.
if a.rollback_benchmark:d.env['PPR_REPLAY_ROLLBACK_BENCHMARK']='1'
try:
 d.launch(); d.connect(); c=d.client
 c.wait_state('running', timeout=120)
 boot_deadline=time.monotonic()+120
 while B.read_scene(c.read_mem).name != 'muMenuMain':
  if time.monotonic()>boot_deadline: raise TimeoutError('Boot did not reach main menu')
  B.tap(c,0,['A'],hold=8,release=60)
 c.call('replay_play',path=str(replay),play_anyway=False)
 deadline=time.monotonic()+max(600,a.frames/10)
 last_print=0
 while time.monotonic()<deadline:
  if d.process.poll() is not None: raise RuntimeError(f'Dolphin exited {d.process.returncode}')
  status=c.call('replay_status')
  if a.rollback_benchmark:
   snapshots=c.call('gprb_status')
   if snapshots.get('error') or snapshots.get('desyncs_detected'):
    raise RuntimeError(str(snapshots))
   if snapshots.get('current_frame',-1)>=a.frames:
    (out/'snapshot-status.json').write_text(json.dumps(snapshots,indent=2))
    (out/'replay-status.json').write_text(json.dumps(status,indent=2))
    if snapshots['save_count']==0 or snapshots['load_count']==0:
     raise RuntimeError('Benchmark performed no saves or loads')
    print('Recorded rollback benchmark completed',flush=True);break

  if time.monotonic()-last_print>15:
   print(json.dumps(status),flush=True); last_print=time.monotonic()
   (out/'replay-status.json').write_text(json.dumps(status,indent=2))
  if status.get('busy'):
   time.sleep(.25); continue
  if status['error'] or status['desync'] or status['desync_frame'] >= 0: raise RuntimeError(str(status))
  if status.get('finished'):
   (out/'replay-status.json').write_text(json.dumps(status,indent=2))
   (out/'snapshot-status.json').write_text(json.dumps(c.call('gprb_status'),indent=2))
   if not status['started'] or status['frames_checked'] <= 0:
    raise RuntimeError('Replay finished without checked frames: '+str(status))
   print('Recorded replay finished',flush=True); break
  time.sleep(.25)
 else: raise TimeoutError('Recorded replay did not finish')
finally:
 d.stop()
 if d.process is not None:
  shutil.copy2(d.stdout_path,out/'dolphin-stdout.log')
  shutil.copy2(d.stderr_path,out/'dolphin-stderr.log')
  if d.log_path.exists():shutil.copy2(d.log_path,out/'dolphin-runtime.log')
 d.cleanup()
