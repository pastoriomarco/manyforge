#!/usr/bin/env python3
"""Offline metrics from Q42 raw evidence, with no copied braking planner."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from old_main_stop_join import sample_export


def stamp(value: dict) -> int:
    return value['sec'] * 1_000_000_000 + value['nanosec']


def analyze(case: Path) -> dict:
    events = [json.loads(line) for line in (case / 'trace.jsonl').read_text().splitlines()]
    outcome = json.loads((case / 'outcome.json').read_text())
    result = dict(outcome=outcome, trace_sha256=hashlib.sha256((case / 'trace.jsonl').read_bytes()).hexdigest())
    if outcome['outcome'] != 'observed':
        return result
    samples = [e for e in events if e['kind'] == 'sample']
    trigger = next(e for e in events if e['kind'] == 'q42_stop_trigger')
    exported = next(e for e in events if e['kind'] == 'q42_core_export')
    sent = next(e for e in events if e['kind'] == 'goal_send' and e['label'] == 'replacement')
    response = next(e for e in events if e['kind'] == 'goal_response' and e['label'] == 'replacement')
    factor = trigger['frozen_applied_factor']
    brake = exported['output']['brake']
    result.update(factor=factor, trigger=trigger, exported=exported,
                  replacement_response=response,
                  brake_first_path_velocity=brake['trajectory']['points'][0]['v'][0],
                  brake_first_physical_velocity=brake['trajectory']['points'][0]['v'][0] * factor,
                  sample_physical_velocity=trigger['sample']['reference']['velocities'][0] * factor,
                  final_exported_path_acceleration=brake['trajectory']['points'][-1]['a'][0],
                  exporter_process_s=(exported['export_receipt']['export_finish_local_ns'] -
                                      exported['export_receipt']['export_start_local_ns']) / 1e9,
                  decision_to_goal_send_s=(sent['local_ns'] -
                                          exported['export_receipt']['request_written_local_ns']) / 1e9,
                  send_to_response_s=(response['local_ns'] - sent['local_ns']) / 1e9)
    if outcome['rejected']:
        cancel = next(e for e in events if e['kind'] == 'cancel_send')
        ack = next(e for e in events if e['kind'] == 'cancel_response')
        active = next(e for e in events if e['kind'] == 'goal_response' and e['label'] == 'calibration')
        boundary_prior = next((e for e in reversed(samples) if e['local_ns'] < cancel['local_ns']), None)
        first = next((e for e in samples if boundary_prior and e['local_ns'] >= cancel['local_ns'] and
                      e['source_ns'] > boundary_prior['source_ns'] and
                      e['cursor_ns'] < boundary_prior['cursor_ns'] and
                      e['reference']['accelerations'] and e['reference']['accelerations'][0] < -.9), None)
        braking = [e for e in samples if first and e['source_ns'] >= first['source_ns']]
        exact = next((e for e in braking if e['reference']['velocities'] == [0.0]), None)
        epsilon = next((e for e in braking if abs(e['reference']['velocities'][0]) <= 1e-8), None)
        prior = next((e for e in reversed(samples) if first and e['source_ns'] < first['source_ns']), None)
        result.update(cancel_send=cancel, cancel_ack=ack, active_uuid=active['uuid'],
                      cancel_uuid_matches_active=cancel['uuid'] == active['uuid'],
                      positive_exact_uuid_ack=ack['response']['return_code'] == 0 and
                      len(ack['response']['goals_canceling']) == 1 and
                      ack['response']['goals_canceling'][0]['goal_id']['uuid'] == active['uuid'],
                      first_cancel_shape_sample=first, preceding_sample=prior,
                      cancel_cursor_restart=bool(first and prior and first['cursor_ns'] < prior['cursor_ns']),
                      first_exact_zero=exact, first_epsilon_zero=epsilon,
                      association='one active accepted fixture goal, exact UUID ACK, restarted cursor and cancel waveform; controller_state has no UUID')
        return result
    first = next(e['sample'] for e in events if e['kind'] == 'q42_first_correlated_consumption')
    prior = next((e for e in reversed(samples) if e['source_ns'] < first['source_ns']), None)
    boundary_prior = next(e for e in reversed(samples) if e['local_ns'] < sent['local_ns'])
    raw_restart = next((e for e in samples if e['local_ns'] >= sent['local_ns'] and
                        e['source_ns'] > boundary_prior['source_ns'] and
                        e['cursor_ns'] < boundary_prior['cursor_ns']), None)
    raw_restart_prior = next((e for e in reversed(samples) if raw_restart and
                              e['source_ns'] < raw_restart['source_ns']), None)
    feedback = [e for e in events if e['kind'] == 'goal_feedback' and e['label'] == 'replacement'
                and e['uuid'] == response['uuid']]
    correlated = [e for e in samples if e['source_ns'] >= first['source_ns'] and
                  any(stamp(f['feedback']['header']['stamp']) == e['source_ns']
                    and all(e['reference'][field] == f['feedback']['desired'][field]
                            for field in ('positions', 'velocities', 'accelerations')) for f in feedback)]
    # Reproduce interpolation of SENT (nanosecond-quantized) points. The oracle
    # is the existing spline sampler; neither Core brake nor join is rederived.
    wire = [dict(t=stamp(p['time_from_start']) / 1e9, q=p['positions'][0],
                 v=p['velocities'][0], a=p['accelerations'][0])
            for p in sent['goal']['trajectory']['points']]
    errors = []
    for e in correlated:
        expected = sample_export(wire, e['cursor_ns'] / 1e9)
        actual = tuple(e['reference'][field][0] for field in ('positions', 'velocities', 'accelerations'))
        errors.append([abs(a - b) for a, b in zip(actual, expected)])
    after_end = [e for e in samples if e['source_ns'] >= first['source_ns']
                 and e['cursor_ns'] / 1e9 >= wire[-1]['t']]
    terminal = next((e for e in events if e['kind'] == 'goal_result' and
                     e['label'] == 'replacement'), None)
    post_terminal = [e for e in samples if terminal and e['local_ns'] >= terminal['local_ns']]
    exact = next((e for e in samples if e['source_ns'] >= first['source_ns']
                  and e['reference']['velocities'] == [0.0]), None)
    observed_install = (first['local_ns'] - sent['local_ns']) / 1e9
    allowance = exported['input']['start_allowance_s']
    result.update(first_correlated_sample=first, preceding_sample=prior,
                  pre_send_predecessor_sample=boundary_prior,
                  consumption_cursor_restart=first['cursor_ns'] < boundary_prior['cursor_ns'],
                  first_raw_restart_sample=raw_restart, raw_restart_preceding_sample=raw_restart_prior,
                  first_raw_restart_physical_velocity=raw_restart['reference']['velocities'][0] * factor if raw_restart else None,
                  pre_restart_physical_velocity=raw_restart_prior['reference']['velocities'][0] * factor if raw_restart_prior else None,
                  exact_stamp_pva_uuid_correlated_samples=len(correlated),
                  first_physical_reference_velocity=first['reference']['velocities'][0] * factor,
                  preceding_physical_reference_velocity=prior['reference']['velocities'][0] * factor if prior else None,
                  first_output_position_change=first['output']['positions'][0] - prior['output']['positions'][0] if prior else None,
                  wire_pva_max_abs_error=[max(row[i] for row in errors) for i in range(3)] if errors else None,
                  observed_send_to_consumption_receipt_s=observed_install,
                  calibrated_allowance_s=allowance, within_calibrated_allowance=observed_install <= allowance,
                  first_exact_zero=exact, brake_endpoint_cursor_samples=after_end,
                  terminal_receipt=terminal, post_terminal_hold_samples=post_terminal,
                  post_terminal_exact_reference_zero=bool(post_terminal) and all(
                      e['reference']['velocities'] == [0.0] for e in post_terminal),
                  post_terminal_path_accelerations=sorted({e['reference']['accelerations'][0] for e in post_terminal}),
                  post_terminal_output_position_range=[min(e['output']['positions'][0] for e in post_terminal),
                                                       max(e['output']['positions'][0] for e in post_terminal)] if post_terminal else None,
                  association='exact replacement UUID feedback/source stamp/PVA + cursor restart; not an internal install ACK',
                  limitations=['single observed calibration is not a timing guarantee',
                               'export/file/process overhead is not production Core cost',
                               'linear one-joint mock cannot qualify soft curved joins or physical stopping'])
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads((args.root / 'manifest.json').read_text())
    value = {entry['case']: analyze(args.root / entry['case']) for entry in manifest['cases']}
    args.output.write_text(json.dumps(value, indent=2) + '\n')
