#!/usr/bin/env python3
"""Check rollback directly against original IID replay checksums."""
import argparse,csv,json,os,shutil,subprocess,sys,time
from pathlib import Path
from recorded_setup import RecordedSetup, countdown_slots
from compare_recorded_replay import compare
from harness_paths import WORKSPACE as w;root=w/'BrawlRecompiled';build=Path(os.environ.get('BRAWL_RECOMP_BUILD',str(root/'wiicompiled/build-linux')))
p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--matches',nargs='+',default=['001','064','128']);p.add_argument('--frames',type=int);p.add_argument('--diagnostic-straight-through',action='store_true');p.add_argument('--verify-snapshots',action='store_true');p.add_argument('--client',type=Path);a=p.parse_args()
from check_wayland import require_wayland
require_wayland(dict(os.environ,XDG_RUNTIME_DIR=os.environ.get('XDG_RUNTIME_DIR','/tmp/brawl-wayland-1000'),WAYLAND_DISPLAY=os.environ.get('WAYLAND_DISPLAY','brawl-wayland')))
base=w/'artifacts'/a.name;base.mkdir()
if a.client:
    client=a.client.resolve()
    if not (client/'WiiCompiled').is_file():raise FileNotFoundError(client/'WiiCompiled')
else:
    client=base/'client';client.mkdir()
    for name in ('WiiCompiled','dsp_coef.bin','initial_pipeline_cache.db','cacert.pem','portable.txt'):os.link(build/name,client/name)
    shutil.copytree(build/'wii_bootstrap',client/'wii_bootstrap')
    ud=client/'UserData';ud.mkdir()
    for name in ('NAND','Codes','Cache'):shutil.copytree(build/'UserData'/name,ud/name,copy_function=os.link if name=='Cache' else shutil.copy2)
    shutil.copy2(build/'UserData/Config.toml',ud/'Config.toml');(ud/'sd.raw').symlink_to(Path(os.environ.get('BRAWL_SHARED_SD',str(w/'artifacts/online-loop-sd.raw'))))
results=[]
for match in a.matches:
    directories=[]
    setup=json.loads((w/f'artifacts/replay-inputs/match-{match}/setup.json').read_text())
    metadata=json.loads((w/f'artifacts/replay-inputs/match-{match}/metadata.json').read_text())
    frames=a.frames if a.frames is not None else metadata['ENDG']['finalFrame']+1
    countdown=base/f'match-{match}-countdown.bin';countdown.write_bytes(countdown_slots(setup))
    for mode,distance in ([('full',0)] if a.diagnostic_straight_through else [])+[('delta',3)]:
        name=f'{a.name}/match-{match}-{mode}';out=w/'artifacts'/name;out.mkdir()
        env=dict(os.environ,BRAWL_BUILD_DIR=str(client),BRAWL_ROLLBACK_INPUT=str(w/f'artifacts/replay-inputs/match-{match}/inputs.brpad'),BRAWL_SNAPSHOT_MODE=mode,BRAWL_SNAPSHOT_VERIFY='1' if mode=='delta' and a.verify_snapshots else '0')
        for key in list(env):
            if key.startswith('BRAWL_ONLINE_') or key in ('BRAWL_ROLLBACK_PORT','BRAWL_ROLLBACK_PEER_PORT','BRAWL_INTEROP_PORT','BRAWL_CROSSPLAY_SEED'):env.pop(key,None)
        ipc=Path(f'/tmp/brawl-recorded-{os.getpid()}-{match}-{mode}.sock')
        env.update(XDG_RUNTIME_DIR=os.environ.get('XDG_RUNTIME_DIR','/tmp/brawl-wayland-1000'),WAYLAND_DISPLAY=os.environ.get('WAYLAND_DISPLAY','brawl-wayland'),
          SDL_VIDEODRIVER='wayland',SDL_AUDIODRIVER='dummy',NSMBW_DETERMINISTIC='1',
          NSMBW_INPUT_SCRIPT='1000:A:200',BRAWL_ONLINE_IPC=str(ipc),
          BRAWL_REPLAY_COUNTDOWN=str(countdown),BRAWL_REPLAY_MATCH_INDEX=str(setup['match_index']),BRAWL_ROLLBACK_START=str(setup['start_frame']-1),
          BRAWL_ROLLBACK_TRACE=str(out/'trace.csv'),BRAWL_ROLLBACK_DISTANCE=str(distance),
          BRAWL_ROLLBACK_FRAMES=str(frames))
        print('Starting recorded match',name,flush=True)
        controller=None
        proc=subprocess.Popen([str(client/'WiiCompiled')],cwd=client,env=env,
            stdout=(out/'runtime.log').open('w'),stderr=subprocess.STDOUT)
        try:
            deadline=time.monotonic()+max(600,frames/10)
            while time.monotonic()<deadline:
                if controller is None and ipc.exists():controller=RecordedSetup(ipc,setup,out)
                if controller:controller.tick()
                log=out/'runtime.log'
                if log.exists() and '[rollback] COMPLETE' in log.read_text(errors='replace'):break
                if proc.poll() is not None:raise RuntimeError(f'{name} exited {proc.returncode}')
                time.sleep(.01)
            else:raise TimeoutError(name)
        finally:
            if proc.poll() is None:proc.terminate()
            try:proc.wait(timeout=5)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
            if controller:controller.close()
            ipc.unlink(missing_ok=True)
        original=compare(out/'trace.csv',w/f'replay-corpus/match-{match}/game.brep',a.frames)
        (out/'original-recording-comparison.json').write_text(json.dumps(original,indent=2))
        if not original['passed']:raise RuntimeError(f'{name} differs from original recording')
        directories.append(out)
    if a.diagnostic_straight_through:
        output=base/f'match-{match}-comparison.json'
        subprocess.run([sys.executable,str(Path(__file__).resolve().parent/'compare_traces.py'),str(directories[0]/'trace.csv'),str(directories[1]/'trace.csv'),'--output',str(output)],check=True)
    results.append(dict(match=match,comparison=original))
    (base/'report.json').write_text(json.dumps(results,indent=2))
print('All selected recorded matches matched original checksums through rollback',flush=True)
