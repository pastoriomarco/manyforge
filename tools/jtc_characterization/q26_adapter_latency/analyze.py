#!/usr/bin/env python3
"""Offline descriptive Q26 observations, never a safe allowance selection."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import statistics


def distribution(values: list[float]) -> dict:
    if not values:
        return dict(count=0)
    ordered = sorted(values)
    def percentile(p: float) -> float:
        x = (len(ordered) - 1) * p
        lo = int(x)
        hi = min(lo + 1, len(ordered) - 1)
        return ordered[lo] + (ordered[hi] - ordered[lo]) * (x - lo)
    return dict(count=len(values), min=min(values), mean=statistics.mean(values),
                median=statistics.median(values), p90=percentile(.90),
                p95=percentile(.95), p99=percentile(.99), max=max(values))


def analyze(root: Path) -> dict:
    results = {}
    for case in sorted(root.glob('rate*')):
        path = case / 'replacements.jsonl'
        rows = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
        rawpath = case / 'raw-observations.jsonl'
        raw = [json.loads(line) for line in rawpath.read_text().splitlines()] if rawpath.exists() else []
        loadpath = case / 'compiler-load.json'
        load = json.loads(loadpath.read_text()) if loadpath.exists() else []
        feedback_index = {}
        for f in raw:
            if f['kind'] == 'feedback':
                feedback_index.setdefault((f['uuid'], f['source_ns'], f['cursor_ns'], f['q'], f['v'], f['a']), []).append(f)
        states = [s for s in raw if s['kind'] == 'state']
        association = []
        for row in rows:
            if not row['correlated']:
                association.append(dict(index=row['index'], valid=False))
                continue
            s = row['first_correlated_state']
            prior = row['predecessor']
            matching = [f for f in feedback_index.get((row['uuid'], s['source_ns'], s['cursor_ns'], s['q'], s['v'], s['a']), [])
                        if f['local_ns'] >= row['replace_entry_ns']]
            candidates = [a for a in states if a['local_ns'] >= row['replace_entry_ns']
                          and a['source_ns'] > prior['source_ns'] and a['cursor_ns'] < prior['cursor_ns']
                          and any(f['local_ns'] >= row['replace_entry_ns'] for f in feedback_index.get(
                              (row['uuid'], a['source_ns'], a['cursor_ns'], a['q'], a['v'], a['a']), []))]
            first = min(candidates, key=lambda a: a['local_ns']) if candidates else None
            valid = bool(matching and first == s and s['source_ns'] > prior['source_ns']
                         and s['cursor_ns'] < prior['cursor_ns'] and s['local_ns'] >= row['replace_entry_ns'])
            active_workers = sorted({c['worker'] for c in load if c.get('begin_ns', 0) <= row['replace_entry_ns'] < c.get('end_ns', 0)
                                     and 'command' in c})
            association.append(dict(index=row['index'], valid=valid,
                                    active_compiler_workers_at_entry=active_workers,
                                    expected_same_curve_q_error=s['q'] - .02 * (row['offset_s'] + s['cursor_ns'] / 1e9),
                                    path_v_error=s['v']-.02, path_a=s['a']))
        measured = [r for r in rows if not r['warmup']]
        observed = [r for r in measured if r['correlated']]
        stamps = sorted({r['source_ns'] for r in raw if r['kind'] == 'state'})
        results[case.name] = dict(
            outcome=json.loads((case/'outcome.json').read_text()),
            total_attempted=len(rows), warmups_attempted=sum(r['warmup'] for r in rows),
            measured_attempted=len(measured), measured_correlated=len(observed),
            measured_uncorrelated=sum(not r['correlated'] for r in measured),
            measured_not_attempted=50-len(measured),
            entry_to_correlated_state_receipt_ms=distribution([(r['first_correlated_state']['local_ns']-r['replace_entry_ns'])/1e6 for r in observed]),
            return_to_correlated_state_receipt_ms=distribution([(r['first_correlated_state']['local_ns']-r['replace_return_ns'])/1e6 for r in observed]),
            replace_call_duration_ms=distribution([(r['replace_return_ns']-r['replace_entry_ns'])/1e6 for r in measured]),
            controller_state_source_gap_ms=distribution([(b-a)/1e6 for a,b in zip(stamps,stamps[1:])]),
            association_checks=association, compiler_attempts=len(load),
            compiler_errors=[r for r in load if 'error' in r],
            limitation='observer receipt and call brackets, not exact SDK send/internal install/production safe allowance')
    return results


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    (args.root/'analysis.json').write_text(json.dumps(analyze(args.root), indent=2) + '\n')
