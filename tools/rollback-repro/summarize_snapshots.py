#!/usr/bin/env python3
"""Summarize completed snapshot metrics; compare measured native and Dolphin snapshot operations."""
import argparse
import csv
import json
import statistics
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('metrics', type=Path)
p.add_argument('--dolphin-status', type=Path)
p.add_argument('--output', type=Path)
p.add_argument('--previous-metrics', type=Path)
p.add_argument('--markdown-output', type=Path)
a = p.parse_args()
with a.metrics.open() as f:
    rows = list(csv.DictReader(f))
if not rows or any(None in r.values() for r in rows):
    raise SystemExit('Empty or incomplete metrics: wait for the run to finish')
saves = [r for r in rows if r['event'] == 'save']
loads = [r for r in rows if r['event'] == 'load']
# A state with a full base is not an incremental snapshot, including replacement
# of that base during early resimulation. Payload also includes CpuContext.
deltas = [r for r in saves if int(r['state_payload_bytes']) < int(r['selected_ram_bytes'])]
def summary(key, group):
    if group and key not in group[0]:
        return None  # Metrics written before write tracking was added.
    values = sorted(int(r[key]) for r in group)
    return dict(mean=statistics.mean(values), median=statistics.median(values),
                p95=values[min(len(values)-1, int(len(values)*.95))], max=max(values)) if values else None
result = dict(metrics=str(a.metrics.resolve()), saves=len(saves),
              loads=sum(r['event'] == 'load' for r in rows),
              incremental_saves=len(deltas),
              verified=all(r['verified'] == '1' for r in rows),
              selected_ram_bytes=int(saves[0]['selected_ram_bytes']),
              restore_guest_write_bytes=summary('state_payload_bytes', loads) if any(int(r['state_payload_bytes']) for r in loads) else None,
              restore_guest_read_bytes=summary('ram_bytes_examined', loads),
              load_duration_ns=summary('duration_ns', loads),
              peak_delta_history_bytes=max(int(r['delta_history_bytes']) for r in rows),
              initial_payload_bytes=int(saves[0]['state_payload_bytes']),
              peak_store_resident_bytes=max(int(r['resident_bytes']) for r in rows),
              note='Store resident bytes exclude full reference copies in verification mode; save timing includes verification when enabled.')
for key in ('state_payload_bytes', 'ram_bytes_examined', 'dirty_metadata_bytes', 'duration_ns', 'tracking_capture_ns', 'tracking_reset_ns'):
    result['incremental_'+key] = summary(key, deltas)
if a.dolphin_status:
    d = json.loads(a.dolphin_status.read_text())
    if not d['save_count'] or not d['load_count']:
        raise SystemExit('Dolphin recorded no save or restore operations; this is not a rollback benchmark')
    mean = d['save_granules_total'] * 64 / d['save_count']
    result['dolphin_mean_ram_delta_bytes'] = mean
    result['dolphin_reported_save_mean_us'] = d['save_us_total'] / d['save_count']
    result['dolphin_reported_load_mean_us'] = d['load_us_total'] / d['load_count'] if d['load_count'] else None
    result['dolphin_mean_restore_candidate_bytes'] = d['load_granules_total'] * 64 / d['load_count'] if d['load_count'] else None
    for source,denominator,target in [
        ('save_candidate_bytes_total','save_count','dolphin_mean_save_candidate_bytes'),
        ('save_bitmap_bytes_total','save_count','dolphin_mean_save_bitmap_bytes'),
        ('load_bitmap_bytes_total','load_count','dolphin_mean_load_bitmap_bytes'),
        ('load_written_bytes_total','load_count','dolphin_mean_restore_written_bytes')]:
        result[target] = d[source] / d[denominator] if source in d and d[denominator] else None
    result['dolphin_region_mode'] = d.get('snapshot_region_mode')
    result['timing_note'] = 'Native timers cover Save/Load callbacks; Dolphin reports its own SaveFrame/LoadFrame timers. These are measured implementation scopes, not total per-frame overhead or a controlled speed ratio.'
    complete_payload = ('save_payload_bytes_total' in d and
                        d.get('save_payload_incomplete_count', 1) == 0)
    initial = d.get('save_initial_count', 0)
    incremental_count = d['save_count'] - initial
    result['dolphin_incremental_saves'] = incremental_count
    for total, initial_key, target in [
        ('save_candidate_bytes_total', 'save_initial_candidate_bytes_total', 'dolphin_mean_save_candidate_bytes'),
        ('save_bitmap_bytes_total', 'save_initial_bitmap_bytes_total', 'dolphin_mean_save_bitmap_bytes')]:
        result[target] = ((d[total] - d[initial_key]) / incremental_count
                          if initial_key in d and incremental_count else None)

    result['dolphin_mean_saved_payload_bytes'] = (
        (d['save_payload_bytes_total'] - d['save_initial_payload_bytes_total']) / incremental_count
        if complete_payload and incremental_count else None)
    result['dolphin_incremental_save_mean_us'] = (
        (d['save_us_total'] - d['save_initial_us_total']) / incremental_count
        if 'save_initial_us_total' in d and incremental_count else None)
    result['dolphin_save_region_count'] = d.get('save_region_count')
    result['dolphin_payload_components_total_bytes'] = {
        k: d.get(k) for k in ('save_payload_bytes_total', 'save_index_bytes_total',
                             'save_cpu_bytes_total', 'save_device_bytes_total')}
    result['comparison_note'] = ('Payload includes RAM, indices and CPU/device state. '
        'Unavailable or incomplete Dolphin payload accounting is null, never replaced with RAM-only bytes. '
        'Incremental save time excludes initial base capture in both implementations; load time includes every load. '
        'Historical profiled runs are retained as historical, not controlled performance ratios.')
text = json.dumps(result, indent=2)
if a.output:
    a.output.write_text(text+'\n')
print(text)

if a.markdown_output:
    import subprocess
    previous = None
    if a.previous_metrics:
        previous = json.loads(subprocess.check_output(
            [__import__('sys').executable, __file__, str(a.previous_metrics)], text=True))
    def native(r, key, scale):
        if r is None or r.get(key) is None: return None
        v = r[key]
        return (v['mean'] if isinstance(v, dict) else v) / scale
    def dolphin(key, scale):
        v = result.get(key)
        return None if v is None else v / scale
    # These labels and column order are a standing reporting contract.
    specs = [
        ('Mean incremental saved payload¹', 'incremental_state_payload_bytes', 'dolphin_mean_saved_payload_bytes', 1e6, 1e6, 'MB', 3),
        ('Mean guest RAM copied per restore', 'restore_guest_write_bytes', 'dolphin_mean_restore_written_bytes', 1e6, 1e6, 'MB', 3),
        ('Mean measured save time', 'incremental_duration_ns', 'dolphin_incremental_save_mean_us', 1e6, 1e3, 'ms', 3),
        ('Mean measured restore time', 'load_duration_ns', 'dolphin_reported_load_mean_us', 1e6, 1e3, 'ms', 3),
        ('Mean guest RAM examined to find changes', 'incremental_ram_bytes_examined', 'dolphin_mean_save_candidate_bytes', 1e6, 1e6, 'MB/save', 3),
        ('Tracking metadata read', 'incremental_dirty_metadata_bytes', 'dolphin_mean_save_bitmap_bytes', 1e6, 1e6, 'MB/save', 3),
        ('Peak snapshot storage + working buffers', 'peak_store_resident_bytes', 'dolphin_peak_store_resident_bytes', 1e6, 1e6, 'MB', 2),
    ]
    lines = ['| Metric | Recomp, previous | Recomp, current | Brawl Online / Dolphin |',
             '|---|---:|---:|---:|']
    for label, nk, dk, ns, ds, unit, decimals in specs:
        values = [native(previous, nk, ns), native(result, nk, ns), dolphin(dk, ds)]
        cells = ['Unmeasured' if v is None else f'**{v:.{decimals}f} {unit}**' for v in values]
        lines.append('| ' + label + ' | ' + ' | '.join(cells) + ' |')
    lines += ['', 'Units are decimal MB and milliseconds per operation. Save time excludes initial base capture; restore bytes count actual guest writes.',
              '¹ Full logical payload: RAM, indices and CPU/device state. Missing complete accounting is shown as Unmeasured.',
              f'Current source: {a.metrics}. Dolphin source: {a.dolphin_status}.',
              f'Previous source: {a.previous_metrics}. Profiling and verification conditions must be reported alongside this table.']
    a.markdown_output.write_text('\n'.join(lines) + '\n')
