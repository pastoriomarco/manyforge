#!/usr/bin/env python3
"""Read recorded probe traces; report observations rather than qualification passes."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from old_main_stop_join import sample_export


def stamp_ns(value: dict) -> int:
    return value['sec'] * 1_000_000_000 + value['nanosec']


def read_case(root: Path, name: str) -> tuple[list[dict], dict]:
    path = root / name
    return ([json.loads(line) for line in (path / 'trace.jsonl').read_text().splitlines()],
            json.loads((path / 'outcome.json').read_text()))


def analyze(root: Path) -> dict:
    manifest = json.loads((root / 'manifest.json').read_text())
    result = {'attempt': root.name, 'source_sha256': manifest['source_sha256'],
              'packages': manifest['packages'], 'cases': {}}
    for entry in manifest['cases']:
        name = entry['case']
        events, outcome = read_case(root, name)
        samples = [event for event in events if event['kind'] == 'sample']
        facts = {'harness_outcome': outcome['outcome'],
                 'sample_count': len(samples),
                 'trace_sha256': hashlib.sha256((root / name / 'trace.jsonl').read_bytes()).hexdigest(),
                 'goal_responses': [event for event in events if event['kind'] == 'goal_response'],
                 'goal_results': [event for event in events if event['kind'] == 'goal_result']}
        if name.startswith(('main_join_', 'negative_join_', 'manymove_join_')):
            trip_event = next(event for event in events if event['kind'] == 'main_join_trip')
            export = next(event for event in events if event['kind'] == 'main_join_export')
            response = next(event for event in events if event['kind'] == 'goal_response'
                            and event['label'] == 'replacement')
            sent = next(event for event in events if event['kind'] == 'goal_send'
                        and event['label'] == 'replacement')
            end = next(event for event in events if event['kind'] == 'main_join_observation_end')
            trip = trip_event['sample']
            points = export['exported_points']
            terminal = next((event for event in events if event['kind'] == 'goal_result'
                             and event['label'] == 'replacement'), None)
            feedback = [event for event in events if event['kind'] == 'goal_feedback'
                        and event['label'] == 'replacement' and event['uuid'] == response['uuid']]
            correlated = []
            for event in feedback:
                source = stamp_ns(event['feedback']['header']['stamp'])
                desired = event['feedback']['desired']
                for sample in samples:
                    if sample['source_ns'] == source and all(
                            sample['reference'][field] == desired[field]
                            for field in ('positions', 'velocities', 'accelerations')):
                        correlated.append((event, sample))
            # Include the consumed sample even if its local receipt preceded the
            # asynchronous goal-response callback. Same source stamp+PVA+UUID
            # corroborates the restart for this fixture, not a generic install ACK.
            first = correlated[0][1] if correlated else None
            tracked = [sample for sample in samples if first is not None
                       and sample['source_ns'] >= first['source_ns']
                       and (terminal is None or sample['local_ns'] < terminal['local_ns'])]
            prior = next((sample for sample in reversed(samples)
                          if first is not None and sample['source_ns'] < first['source_ns']), None)
            pva_errors = []
            lead_errors = []
            factor = 1.0 if export['old_main_unit_factor_assertion'] else trip['factor']
            lead_end = (trip_event['lead_in_wall_s'] - export['measured_compute_latency_s']) * factor
            for sample in tracked:
                cursor = sample['cursor_ns'] / 1e9
                if cursor < points[0]['t']:
                    continue
                expected = sample_export(points, cursor)
                actual = [sample['reference'][field][0]
                          for field in ('positions', 'velocities', 'accelerations')]
                pva_errors.append([abs(actual[index] - expected[index]) for index in range(3)])
                if 0 <= cursor < lead_end:
                    old_q = 0.2 * (trip['cursor_ns'] / 1e9 + trip['factor'] * (
                        export['measured_compute_latency_s'] + cursor / factor))
                    lead_errors.append([abs(actual[0] - old_q),
                                        abs(actual[1] * factor - 0.2 * trip['factor']),
                                        abs(actual[2] * factor**2)])
            facts.update(trip=trip_event, export=export, observation_end=end,
                         first_restart_sample=tracked[0] if tracked else None,
                         first_uuid_feedback=correlated[0][0] if correlated else None,
                         preceding_sample=prior,
                         first_output_position_change=(first['output']['positions'][0]
                                                       - prior['output']['positions'][0])
                         if first is not None and prior is not None else None,
                         exact_source_stamp_pva_uuid_correlations=len(correlated),
                         observed_restart_sample_count=len(tracked),
                         export_pva_max_absolute_errors=[max(row[index] for row in pva_errors)
                                                        for index in range(3)] if pva_errors else None,
                         lead_in_physical_pva_max_absolute_errors_from_original_linear_path=
                         [max(row[index] for row in lead_errors) for index in range(3)]
                         if lead_errors else None,
                         lead_in_sample_count=len(lead_errors),
                         final_result_before_original_cutoff=terminal is not None
                         and terminal['local_ns'] <= trip_event['unchanged_deadline_local_ns'],
                         acceptance_elapsed_s=end['acceptance_elapsed_s'],
                         goal_send_to_response_s=(response['local_ns'] - sent['local_ns']) / 1e9,
                         export_to_response_s=(response['local_ns'] - export['local_ns']) / 1e9,
                         deadline_unchanged=trip_event['unchanged_deadline_local_ns']
                         == export['unchanged_deadline_local_ns'] == end['unchanged_deadline_local_ns'],
                         association='Exact source-stamp/PVA match with replacement-UUID feedback plus cursor '
                                     'restart; includes pre-response samples; no generic internal install ACK',
                         limitation='Analytical linear-path candidate; not full Core planner, curved anchor '
                                    'or Kernel hard-floor qualification')
        elif name.startswith('replace_'):
            trip = next(event for event in events if event['kind'] == 'replacement_trip')['sample']
            goal = next(event for event in events if event['kind'] == 'goal_send'
                        and event['label'] == 'replacement')
            response = next(event for event in events if event['kind'] == 'goal_response'
                            and event['label'] == 'replacement')
            post = [sample for sample in samples if sample['local_ns'] > response['local_ns']]
            feedback = [event for event in events if event['kind'] == 'goal_feedback'
                        and event['label'] == 'replacement']
            header = stamp_ns(goal['goal']['trajectory']['header']['stamp'])
            window = [sample for sample in post if sample['source_ns'] < header] if header else []
            # This fixture's old trajectory is exactly q(trip)+0.2*factor*elapsed.
            def old_reference(sample: dict) -> float:
                return (trip['reference']['positions'][0]
                        + 0.2 * trip['factor'] * (sample['source_ns'] - trip['source_ns']) / 1e9)
            facts.update(trip=trip, submitted=goal, first_post_response_sample=post[0],
                         first_uuid_feedback=feedback[0] if feedback else None,
                         association='UUID feedback plus cursor/PVA transition; acceptance stamp unavailable; '
                                     'neither UUID nor local receipt alone proves consumption',
                         pre_header_sample_count=len(window),
                         pre_header_cursor_range_ns=[window[0]['cursor_ns'], window[-1]['cursor_ns']]
                         if window else None,
                         pre_header_reference_velocity_range=[
                             min(sample['reference']['velocities'][0] for sample in window),
                             max(sample['reference']['velocities'][0] for sample in window)]
                         if window else None,
                         pre_header_max_reference_deviation_from_old_linear_path=
                         max(abs(sample['reference']['positions'][0] - old_reference(sample))
                             for sample in window) if window else None,
                         pre_header_max_output_deviation_from_old_linear_next_sample=
                         max(abs(sample['output']['positions'][0] - (old_reference(sample) + 0.002))
                             for sample in window) if window else None)
        elif name.startswith('q35_cancel_'):
            trigger = next(event for event in events if event['kind'] == 'q35_stop_trigger')
            pin = next(event for event in events if event['kind'] == 'q35_freeze_pin')
            sent = next(event for event in events if event['kind'] == 'cancel_send')
            ack = next(event for event in events if event['kind'] == 'cancel_response')
            terminal = next((event for event in events if event['kind'] == 'goal_result'
                             and event['label'] == 'moving'), None)
            factor = trigger['frozen_applied_factor']
            post = [sample for sample in samples
                    if sample['source_ns'] > trigger['sample']['source_ns']]
            # This fresh one-goal fixture has a linear positive-velocity path,
            # no replacement and no other trajectory producer. Cursor restart
            # plus its -1 path acceleration distinguishes the cancel waveform
            # here; controller_state still has no internal-trajectory UUID.
            first = next((sample for sample in post if sample['reference']['accelerations'] and
                          sample['reference']['accelerations'][0] < -0.9), None)
            braking = [sample for sample in post if first is not None and
                       sample['source_ns'] >= first['source_ns']]
            zero = next((sample for sample in braking
                         if sample['reference']['velocities'] and
                         abs(sample['reference']['velocities'][0]) < 1e-8), None)
            nonzero = [sample for sample in braking if zero is not None and
                       sample['source_ns'] < zero['source_ns'] and
                       sample['reference']['velocities'][0] > 1e-8]
            previous = next((sample for sample in reversed(samples)
                             if first is not None and sample['source_ns'] < first['source_ns']), None)
            first_next = next((sample for sample in braking if first is not None and
                               sample['source_ns'] > first['source_ns']), None)
            physical = []
            for previous_sample, sample in zip(samples, samples[1:]):
                if first is None or sample['source_ns'] < first['source_ns']:
                    continue
                dt = (sample['source_ns'] - previous_sample['source_ns']) / 1e9
                if dt <= 0:
                    continue
                physical.append(dict(source_ns=sample['source_ns'],
                    reference_velocity_path=sample['reference']['velocities'][0],
                    reference_velocity_physical_equivalent=
                    sample['reference']['velocities'][0] * sample['factor'],
                    reference_acceleration_path=sample['reference']['accelerations'][0],
                    reference_acceleration_physical_equivalent=
                    sample['reference']['accelerations'][0] * sample['factor']**2,
                    mock_measured_position=sample['measured']['positions'][0],
                    mock_measured_velocity=sample['measured']['velocities'][0],
                    output_position=sample['output']['positions'][0],
                    output_position_finite_difference_velocity=
                    (sample['output']['positions'][0] - previous_sample['output']['positions'][0]) / dt))
            before = trigger['sample']
            positive_ack = ack['response']['return_code'] == 0 and any(
                entry['goal_id']['uuid'] == sent['uuid']
                for entry in ack['response']['goals_canceling'])
            facts.update(trigger=trigger, pin=pin, cancel_send=sent, cancel_response=ack,
                positive_exact_uuid_cancel_ack=positive_ack, first_cancel_shape_sample=first,
                preceding_sample=previous, next_sample=first_next, first_zero_reference_sample=zero,
                cursor_restart_observed=bool(first and previous and
                                             first['cursor_ns'] < previous['cursor_ns']),
                initial_reference_physical_equivalent=first['reference']['velocities'][0] * factor
                if first else None, pre_stop_mock_measured_velocity=before['measured']['velocities'][0],
                reference_physical_equivalent_to_pre_stop_mock_velocity_ratio=
                first['reference']['velocities'][0] * factor / before['measured']['velocities'][0]
                if first and before['measured']['velocities'][0] else None,
                first_output_position_change=first['output']['positions'][0] - previous['output']['positions'][0]
                if first and previous else None,
                first_zero_source_ms_after_cancel_start=(zero['source_ns'] - first['source_ns']) / 1e6
                if zero and first else None,
                nonzero_to_zero_source_bracket_ms=[
                    (nonzero[-1]['source_ns'] - first['source_ns']) / 1e6,
                    (zero['source_ns'] - first['source_ns']) / 1e6] if nonzero and zero else None,
                cancel_ack_receipt_ms_after_request=(ack['local_ns'] - sent['local_ns']) / 1e6,
                result_receipt_ms_after_request=(terminal['local_ns'] - sent['local_ns']) / 1e6
                if terminal else None,
                first_zero_receipt_ms_after_request=(zero['local_ns'] - sent['local_ns']) / 1e6
                if zero else None,
                physical_time_series=physical, actual_factor_values=sorted({sample['factor'] for sample in post}),
                association='Single accepted moving goal + exact UUID cancel ACK/result + cursor restart/-1 '
                            'acceleration fingerprint; fixture correlation, not generic internal cancel ACK',
                limitation='Position-only ideal mock; physical-time derivative equivalents assume observed '
                           'constant factor; not device delivery, physical rest or Stop-deadline qualification',
                remedy_chosen=False)
        elif name.startswith('cancel_'):
            sent = next(event for event in events if event['kind'] == 'cancel_send')
            terminal = next(event for event in events if event['kind'] == 'goal_result')
            post = [sample for sample in samples if sample['local_ns'] > sent['local_ns']]
            zero = next((sample for sample in post if abs(sample['reference']['velocities'][0]) < 1e-8), None)
            facts.update(before_cancel=outcome['before_cancel'], last_sample=post[-1],
                         cancel_send=sent,
                         cancel_response=next(event for event in events if event['kind'] == 'cancel_response'),
                         first_post_request_sample=post[0], first_zero_reference_sample=zero,
                         result_receipt_ms_after_request=(terminal['local_ns'] - sent['local_ns']) / 1e6,
                         first_zero_receipt_ms_after_request=(zero['local_ns'] - sent['local_ns']) / 1e6
                         if zero else None,
                         result_to_zero_receipt_ms=(zero['local_ns'] - terminal['local_ns']) / 1e6
                         if zero else None,
                         observation_duration_s=(post[-1]['local_ns'] - sent['local_ns']) / 1e9,
                         finish_proof=False,
                         limitation='Post-request reference observations, not exact internal cancel '
                                    'consumption acknowledgement or physical rest proof')
        elif name == 'ramp_freeze':
            sends = [event for event in events if event['kind'] == 'scale_send'][1:]
            changed = []
            for sample in samples:
                if sample['local_ns'] >= sends[0]['local_ns'] and (
                        not changed or sample['factor'] != changed[-1]['factor']):
                    changed.append(sample)
            facts.update(published_values=[event['value'] for event in sends],
                         observed_factor_changes=[sample['factor'] for sample in changed],
                         observed_monotonic=all(a['factor'] >= b['factor']
                                                for a, b in zip(changed, changed[1:])),
                         freeze=next(event for event in events if event['kind'] == 'freeze'),
                         last_sample=samples[-1],
                         guarantee='Single-run ordered observations; no generation ACK or no-coalescing guarantee')
        else:
            facts['outcome'] = outcome
        result['cases'][name] = facts
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('attempts', nargs='+', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.write_text(json.dumps({'scope': 'functional characterization, not timing/physical qualification',
                                      'attempts': [analyze(root) for root in args.attempts]},
                                     indent=2, allow_nan=False) + '\n')


if __name__ == '__main__':
    main()
