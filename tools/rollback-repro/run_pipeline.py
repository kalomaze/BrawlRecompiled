#!/usr/bin/env python3
"""Execute an ordered TOML rollback experiment; client internals stay in their adapters."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import threading
import time
import tomllib

ROOT = None  # Explicit workspace from the TOML, independent of harness location.
TOOLS = Path(__file__).resolve().parent


def load_spec(path):
    spec = tomllib.loads(path.read_text())
    if set(spec) != {'workspace', 'run', 'environment', 'stages'}:
        raise ValueError('Required TOML fields: workspace, run, environment, stages')
    if not Path(spec['workspace']).is_absolute() or not Path(spec['workspace']).is_dir():
        raise ValueError('workspace must be an existing absolute directory')
    run = spec['run']
    if set(run) != {'name', 'matches', 'frames', 'python', 'previous_metrics'}:
        raise ValueError('run requires name, matches, frames, python, previous_metrics')
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', run['name']):
        raise ValueError('run.name must be a simple unique directory/session name')
    if not run['matches'] or len(set(run['matches'])) != len(run['matches']):
        raise ValueError('Specify distinct match IDs')
    if any(not re.fullmatch(r'\d{3}', m) for m in run['matches']):
        raise ValueError('Match IDs must be three-digit strings')
    if type(run['frames']) is not int or run['frames'] < 0:
        raise ValueError('frames must be positive, or zero for complete matches')
    required_env = {'BRAWL_RECOMP_BUILD', 'PPHARNESS_DOLPHIN', 'PPHARNESS_TEMPLATE',
                    'PPHARNESS_ISO', 'PPHARNESS_INSTANCES', 'BRAWL_SHARED_SD',
                    'XDG_RUNTIME_DIR', 'WAYLAND_DISPLAY', 'PPHARNESS_DOLPHIN_PRIORITY'}
    if set(spec['environment']) != required_env:
        raise ValueError(f'environment must specify exactly {sorted(required_env)}')
    if spec['environment']['PPHARNESS_DOLPHIN_PRIORITY'] != 'normal':
        raise ValueError('Paired measurements require normal priority for both clients')
    names = set()
    for stage in spec['stages']:
        name = stage['name']
        if not re.fullmatch(r'[a-zA-Z0-9_-]+', name) or name in names:
            raise ValueError('Stage names must be distinct simple names')
        names.add(name)
        if stage['kind'] == 'command':
            if set(stage) != {'name', 'kind', 'argv'} or not stage['argv'] or not all(isinstance(v,str) for v in stage['argv']):
                raise ValueError('Command stage requires an argv array (no shell)')
        elif stage['kind'] == 'clients':
            if set(stage) != {'name','kind','engines','schedule','verify_snapshots','purpose'}:
                raise ValueError('Client stage has unknown or missing fields')
            if sorted(stage['engines']) != ['dolphin', 'recomp']:
                raise ValueError('Paired stages require exactly recomp and dolphin')
            if stage['schedule'] not in ('sequential', 'parallel'):
                raise ValueError('schedule must be sequential or parallel')
            if stage['purpose'] not in ('correctness', 'timing') or type(stage['verify_snapshots']) is not bool:
                raise ValueError('Invalid purpose or verification flag')
            if stage['purpose'] == 'timing' and (stage['schedule'] != 'sequential' or stage['verify_snapshots']):
                raise ValueError('Timing comparisons require sequential clients and verification off')
        else:
            raise ValueError(f'Unknown stage kind: {stage["kind"]}')
    if not spec['stages']:
        raise ValueError('No stages specified')
    return spec


def environment(spec):
    # Do not inherit experiment switches from the interactive shell.
    env = {k:v for k,v in os.environ.items() if not k.startswith(('BRAWL_', 'PPHARNESS_', 'PPR_', 'NSMBW_'))}
    env.update(spec['environment'])
    env['BRAWL_WORKSPACE_ROOT'] = spec['workspace']
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    return env


def command_plan(spec, stage, match):
    run = spec['run']
    prefix = f"{run['name']}/{stage['name']}/match-{match}"
    frames = run['frames']
    if frames == 0:
        metadata = json.loads((ROOT/f'artifacts/replay-inputs/match-{match}/metadata.json').read_text())
        frames = metadata['ENDG']['finalFrame'] + 1
    common = [run['python']]
    recomp = common + [str(TOOLS/'run_snapshot_replays.py'), '--name', prefix+'/recomp', '--matches', match, '--frames', str(frames)]
    if run['frames'] == 0:
        recomp = recomp[:-2]  # Omission requests full-match validation in the native adapter.
    if match != run['matches'][0]:
        first = ROOT/'artifacts'/run['name']/stage['name']/f"match-{run['matches'][0]}"/'recomp/client'
        recomp += ['--client', str(first)]
    if stage['verify_snapshots']:
        recomp += ['--verify-snapshots']
    dolphin = common + [str(TOOLS/'check_recorded_replay.py'), '--name', prefix+'/dolphin', '--match', match, '--rollback-benchmark', '--frames', str(frames)]
    output = ROOT/'artifacts'/prefix
    report = common + [str(TOOLS/'summarize_snapshots.py'),
        str(output/f'recomp/match-{match}-delta/trace.csv.snapshots.csv'),
        '--dolphin-status', str(output/'dolphin/snapshot-status.json'),
        '--previous-metrics', run['previous_metrics'],
        '--output', str(output/'accounting.json'), '--markdown-output', str(output/'accounting.md')]
    return {'recomp': recomp, 'dolphin': dolphin, 'report': report}


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def execute(config):
    spec = load_spec(config)
    out = ROOT/'artifacts'/spec['run']['name']
    out.mkdir()  # Existing run names are errors; never overwrite measurements.
    shutil.copy2(config, out/'experiment.toml')
    env = environment(spec)
    lock = threading.Lock()
    state = {'status':'running', 'started':time.time(), 'steps':[]}
    def persist():
        tmp = out/'status.json.tmp'
        tmp.write_text(json.dumps(state, indent=2))
        tmp.replace(out/'status.json')
    def note(message):
        line = time.strftime('%Y-%m-%dT%H:%M:%S%z')+' '+message
        print(line, flush=True)
        with (out/'pipeline.log').open('a') as f:
            f.write(line+'\n')
    def run(label, argv):
        log = out/(label+'.log')
        with lock:
            entry = {'name':label,'argv':argv,'log':str(log),'status':'running','started':time.time()}
            state['steps'].append(entry);persist();note('START '+label)
        with log.open('x') as f:
            proc = subprocess.Popen(argv, cwd=ROOT, env=env, stdout=f, stderr=subprocess.STDOUT)
            with lock:
                entry['pid']=proc.pid;persist()
            code = proc.wait()
        with lock:
            entry.update(status='passed' if code==0 else 'failed', exit_code=code, finished=time.time())
            persist();note(f'END {label}: exit {code}')
        if code:
            raise RuntimeError(f'{label} failed ({code}); see {log}')
    try:
        # Display and immutable recording prerequisites precede builds or client creation.
        run('00-wayland', [spec['run']['python'], str(TOOLS/'check_wayland.py')])
        for key in ('PPHARNESS_ISO','BRAWL_SHARED_SD','PPHARNESS_TEMPLATE'):
            if not Path(env[key]).exists():raise FileNotFoundError(env[key])
        fingerprints = {}
        for match in spec['run']['matches']:
            path = ROOT/f'replay-corpus/match-{match}/game.brep'
            fingerprints[match] = hashlib.sha256(path.read_bytes()).hexdigest()
            for filename in ('inputs.brpad','setup.json','metadata.json'):
                required = ROOT/f'artifacts/replay-inputs/match-{match}'/filename
                if not required.is_file():raise FileNotFoundError(required)
        state['recording_sha256'] = fingerprints
        state['adapter_sha256'] = {name:digest(TOOLS/name) for name in ('run_snapshot_replays.py','check_recorded_replay.py','summarize_snapshots.py','recorded_setup.py','compare_recorded_replay.py')}
        for number,stage in enumerate(spec['stages'], 1):
            label = f'{number:02d}-{stage["name"]}'
            if stage['kind'] == 'command':
                run(label, stage['argv']);continue
            binaries = [Path(env['BRAWL_RECOMP_BUILD'])/'WiiCompiled', Path(env['PPHARNESS_DOLPHIN'])]
            state[label] = {'binary_sha256':{str(p):digest(p) for p in binaries},
                            'schedule':stage['schedule'], 'purpose':stage['purpose'],
                            'verify_snapshots':stage['verify_snapshots']}
            for match in spec['run']['matches']:
                plans = command_plan(spec, stage, match)
                (out/stage['name']/f'match-{match}').mkdir(parents=True)
                if stage['schedule'] == 'parallel':
                    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                        futures = [pool.submit(run,f'{label}-{match}-{engine}',plans[engine]) for engine in stage['engines']]
                        # Join both clients and inspect every failure before stopping the pipeline.
                        errors = [exc for f in futures if (exc := f.exception()) is not None]
                        if errors:raise ExceptionGroup('Paired client stage failed', errors)
                else:
                    for engine in stage['engines']:run(f'{label}-{match}-{engine}',plans[engine])
                if stage['purpose'] == 'timing':
                    run(f'{label}-{match}-report',plans['report'])
        state['status']='passed'
    except BaseException as exc:
        state.update(status='failed',error=str(exc));raise
    finally:
        state['finished']=time.time();persist();note('PIPELINE '+state['status'])


def main():
    global ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=['plan','launch','run'])
    parser.add_argument('config',type=Path)
    args = parser.parse_args()
    config = args.config.resolve()
    spec = load_spec(config)
    ROOT = Path(spec['workspace'])
    if args.action == 'plan':
        print(json.dumps(spec,indent=2));return
    if args.action == 'run':
        execute(config);return
    name = spec['run']['name']
    if (ROOT/'artifacts'/name).exists():raise FileExistsError(name)
    # The shell survives pipeline completion/failure, independent of the tool's parent.
    subprocess.run(['tmux','new-session','-d','-s',name,'-c',str(ROOT),'bash --noprofile --norc'],check=True)
    argv = [sys.executable,str(Path(__file__).resolve()),'run',str(config)]
    subprocess.run(['tmux','send-keys','-t',name,shlex.join(argv),'Enter'],check=True)
    print(f'tmux attach -t {name}\nLogs: {ROOT/"artifacts"/name}')

if __name__ == '__main__':
    main()
