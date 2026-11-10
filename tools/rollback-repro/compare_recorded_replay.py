#!/usr/bin/env python3
"""Compare a recomp's final frame checksums against an original .brep recording."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from harness_paths import WORKSPACE as w
sys.path.insert(0,str(w/'brawlonline.net/harness'))
from ppharness import replay

def compare(trace, source, frames=None):
 recorded=replay.read(source)
 if recorded.truncated or not recorded.endg or not recorded.endg['complete']:
  raise ValueError('Original recording is incomplete')
 final=int(recorded.endg['finalFrame'])
 count=final-recorded.first_frame+1
 if frames is not None:
  if not 0<frames<=count:raise ValueError('Invalid requested prefix length')
  count=frames
 # Later corrected simulation supersedes the earlier prediction for that frame.
 actual={}
 with Path(trace).open() as f:
  for row in csv.DictReader(f):
   index=int(row['session_frame'])
   if index<0:raise ValueError('Negative session frame')
   actual[index]=row
 missing=[];bad=[]
 for index in range(count):
  frame=recorded.first_frame+index
  if index not in actual:missing.append(frame);continue
  row=actual[index]
  expected=recorded.checksums[index]
  if int(row['gameplay_hash'],16)!=expected or row['input_hash']!=row['expected_input_hash']:
   bad.append(dict(frame=frame,recorded=f'{expected:08x}',recomp=row['gameplay_hash'],input_matches=row['input_hash']==row['expected_input_hash']))
 return dict(source=str(source),source_sha256=hashlib.sha256(Path(source).read_bytes()).hexdigest(),
  required_frames=count,compared_frames=count-len(missing),full_match=frames is None,
  missing_frames=missing,mismatched_frames=len(bad),first_mismatches=bad[:10],
  passed=not missing and not bad)

if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('trace',type=Path);p.add_argument('replay',type=Path)
 p.add_argument('--frames',type=int);p.add_argument('--output',type=Path,required=True)
 a=p.parse_args();result=compare(a.trace,a.replay,a.frames)
 a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
 raise SystemExit(0 if result['passed'] else 1)
