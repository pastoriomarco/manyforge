#!/usr/bin/env python3
"""Nonphysical Q42 probe; consumes the frozen real Core exporter, never a brake implementation."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import time
import traceback
import xml.etree.ElementTree as ET

import rfc8785
import rclpy

from jtc_characterization import Probe, ns, point, safe


class Q42Probe(Probe):
    def __init__(self, output: Path, case: str) -> None:
        self.events: list[dict] = []
        super().__init__(output, case)

    def event(self, kind: str, **fields: object) -> None:
        record = safe({'kind': kind, 'case': self.case, 'local_ns': time.monotonic_ns(),
                       'ros_ns': self.get_clock().now().nanoseconds, **fields})
        self.events.append(record)
        self.trace.write(json.dumps(record, allow_nan=False) + '\n')
        self.trace.flush()

    def consumed(self, label: str) -> dict | None:
        handle = self.handles.get(label)
        if handle is None or not handle.accepted:
            return None
        uuid = list(handle.goal_id.uuid)
        sent = next(e for e in self.events if e['kind'] == 'goal_send' and e['label'] == label)
        prior = next((s for s in reversed(self.samples) if s['local_ns'] < sent['local_ns']), None)
        if prior is None:
            return None
        for feedback in self.events:
            if (feedback['kind'] != 'goal_feedback' or feedback['label'] != label or
                    feedback['uuid'] != uuid):
                continue
            desired = feedback['feedback']['desired']
            stamp = ns_dict(feedback['feedback']['header']['stamp'])
            for sample in self.samples:
                if (sample['local_ns'] >= sent['local_ns'] and
                        sample['source_ns'] > prior['source_ns'] and
                        sample['cursor_ns'] < prior['cursor_ns'] and
                        sample['source_ns'] == stamp and all(
                        sample['reference'][field] == desired[field]
                        for field in ('positions', 'velocities', 'accelerations'))):
                    return sample
        return None


def ns_dict(stamp: dict) -> int:
    return stamp['sec'] * 1_000_000_000 + stamp['nanosec']


def wire_points(trajectory: dict) -> list:
    # Serialization only: Core has already authored path-time PVA and signed times.
    return [point(p['t'], p['p'][0], p['v'][0], p['a'][0])
            for p in trajectory['points']]


def export_core(directory: Path, name: str, value: dict) -> tuple[dict, dict]:
    request = directory / (name + '.input.json')
    temporary = directory / (name + '.input.tmp')
    written = time.monotonic_ns()
    temporary.write_bytes(rfc8785.dumps(value))
    temporary.replace(request)
    response = directory / (name + '.receipt.json')
    deadline = time.monotonic() + 5  # Fixture process watchdog, not a lane allowance.
    while not response.exists():
        if time.monotonic() >= deadline:
            raise TimeoutError('frozen Core exporter worker did not answer')
        time.sleep(0.001)
    receipt = json.loads(response.read_text())
    receipt.update(request_written_local_ns=written, response_read_local_ns=time.monotonic_ns())
    if receipt['returncode'] != 0:
        raise RuntimeError('Core exporter failed: ' + receipt['stderr'])
    return json.loads((directory / (name + '.output.json')).read_text()), receipt


def experiment(probe: Q42Probe, factor: float, requests: Path, reject: bool) -> dict:
    probe.start()
    probe.set_scale(factor)
    q = probe.samples[-1]['reference']['positions'][0]
    active = dict(joint_names=['joint1'], points=[
        dict(t=0, p=[q], v=[1], a=[0]),
        dict(t=3, p=[q + 3], v=[1], a=[0]),
        dict(t=4, p=[q + 3.5], v=[0], a=[0])])
    handle = probe.goal('moving', wire_points(active))
    if not handle.accepted:
        raise RuntimeError('Q42 initial moving goal refused')
    probe.wait(lambda: abs(probe.samples[-1]['reference']['velocities'][0] - 1) < 1e-10
               and probe.samples[-1]['cursor_ns'] >= 100_000_000,
               3, 'Q42 accepted moving precondition')
    probe.event('q42_moving_precondition', sample=probe.samples[-1],
                physical_velocity_equivalent=factor, actual_applied_factor=factor,
                scope='one-joint fixture, path speed1 and lane deceleration1; no production defaults')

    # One independent calibration: preserve all original PVA, rebase only time.
    # It is an identical-path replacement, not another brake or policy.
    elapsed = probe.samples[-1]['cursor_ns'] / 1e9
    active = dict(joint_names=active['joint_names'], points=[
        dict(p, t=p['t'] - elapsed) for p in active['points']])
    calibrated = probe.goal('calibration', wire_points(active))
    if not calibrated.accepted:
        raise RuntimeError('identical-path calibration goal refused')
    first = probe.wait(lambda: probe.consumed('calibration'), 3,
                       'calibration UUID/source-stamp/PVA consumption')
    sent = next(e for e in probe.events if e['kind'] == 'goal_send' and
                e['label'] == 'calibration')
    allowance = (first['local_ns'] - sent['local_ns']) / 1e9
    if allowance <= 0:
        raise RuntimeError('invalid measured consumption receipt allowance')
    probe.event('q42_allowance_calibration', sample=first, allowance_s=allowance,
                goal_send_local_ns=sent['local_ns'],
                scope='single observed send-to-correlated-consumption receipt upper bound; not a guarantee')
    probe.wait(lambda: probe.samples[-1]['cursor_ns'] >= 80_000_000, 3,
               'calibration successor progress before second observable cursor restart')
    sample = probe.samples[-1]
    if abs(sample['reference']['velocities'][0] - 1) > 1e-10 or 'calibration' in probe.results:
        raise RuntimeError('calibration is not still the active linear moving goal')
    frozen = sample['factor']
    if frozen != factor:
        raise RuntimeError('requested factor was not actually applied')
    probe.event('q42_stop_trigger', sample=sample, frozen_applied_factor=frozen)
    probe.event('q42_freeze_pin', value=frozen, pin_publications=1,
                caveat='one identical-value resend; no generation acknowledgment')
    probe.scale(frozen)  # One identical pin; no factor=1 workaround.
    decision = time.monotonic_ns()
    input_value = dict(active=active, controller_period_s=0.01,
                       frozen_factor=frozen, max_deceleration_rad_s2={'joint1': 1},
                       sample=dict(cursor_s=sample['cursor_ns'] / 1e9,
                                   reference_path_velocities=sample['reference']['velocities'],
                                   reference_positions_rad=sample['reference']['positions']),
                       start_allowance_s=allowance,
                       wall_since_sample_s=(decision - sample['local_ns']) / 1e9)
    output, export_receipt = export_core(requests, probe.case, input_value)
    probe.event('q42_core_export', input=input_value, output=output,
                export_receipt=export_receipt,
                caveat='worker/file/process time after input decision is separate fixture overhead')
    brake = output.get('brake', {})
    replacement = brake.get('replace', {})
    if 'error' in brake or 'error' in replacement or 'points' not in replacement:
        raise RuntimeError('actual Core refused the fixture brake/join: ' + json.dumps(output))
    handle = probe.goal('replacement', wire_points(replacement), invalid_joint=reject)
    if reject:
        if handle.accepted:
            raise RuntimeError('deliberately invalid replacement unexpectedly accepted')
        probe.event('q42_refused_replace_fallback', active_label='calibration',
                    active_uuid=list(probe.handles['calibration'].goal_id.uuid),
                    scope='fixture exercises refusal then cancel; not integrated Core Stop-stage proof')
        probe.cancel('calibration')
        probe.spin(1.5)
        sent = next(e for e in probe.events if e['kind'] == 'cancel_send')
        prior = next(s for s in reversed(probe.samples) if s['local_ns'] < sent['local_ns'])
        first = next((s for s in probe.samples if s['local_ns'] >= sent['local_ns'] and
                      s['source_ns'] > prior['source_ns'] and s['cursor_ns'] < prior['cursor_ns'] and
                      s['reference']['accelerations'] and s['reference']['accelerations'][0] < -.9), None)
        exact_zero = next((s for s in probe.samples if first and s['source_ns'] >= first['source_ns'] and
                           s['reference']['velocities'] == [0.0]), None)
        if first is None or exact_zero is None:
            raise RuntimeError('refusal fallback lacks associated cancel restart/exact-zero endpoint')
        probe.event('q42_cancel_finish_evidence', restart_sample=first, exact_zero_sample=exact_zero,
                    caveat='controlled fixture association, not physical rest')
        return dict(outcome='observed', factor=factor, rejected=True,
                    active_result=probe.results.get('calibration'), final_sample=probe.samples[-1])
    if not handle.accepted:
        raise RuntimeError('actual Core replacement refused by JTC')
    first = probe.wait(lambda: probe.consumed('replacement'), 3,
                       'Core replacement UUID/source-stamp/PVA consumption')
    probe.event('q42_first_correlated_consumption', sample=first)
    probe.wait(lambda: 'replacement' in probe.results, 3, 'Core brake terminal result')
    probe.spin(0.15)
    return dict(outcome='observed', factor=factor, rejected=False,
                replacement_result=probe.results['replacement'], final_sample=probe.samples[-1])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--requests', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    args.requests.mkdir(parents=True, exist_ok=True)
    packages = {}
    for package in ('joint_trajectory_controller', 'controller_manager', 'hardware_interface',
                    'rclpy', 'control_msgs', 'controller_manager_msgs', 'realtime_tools'):
        path = Path('/opt/ros') / os.environ['ROS_DISTRO'] / 'share' / package / 'package.xml'
        packages[package] = dict(version=ET.parse(path).findtext('version'),
                                package_xml_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    manifest = dict(source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    base_probe_sha256=hashlib.sha256((Path(__file__).parent / 'jtc_characterization.py').read_bytes()).hexdigest(),
                    packages=packages, python=platform.python_version(),
                    scope='Core exporter + stock JTC position GenericSystem, no physical/runtime policy proof', cases=[])
    failed = False
    for name, factor, reject in [('quarter', .25, False), ('half', .5, False),
                                  ('three_quarters', .75, False), ('one', 1.0, False),
                                  ('refused_half', .5, True)]:
        case = 'q42_' + name
        print('START ' + case, flush=True)
        rclpy.init()
        probe = Q42Probe(args.output / case, case)
        try:
            outcome = experiment(probe, factor, args.requests, reject)
        except Exception as error:
            failed = True
            outcome = dict(outcome='harness_failed', error=repr(error), traceback=traceback.format_exc())
        finally:
            probe.close()
            rclpy.shutdown()
        (args.output / case / 'outcome.json').write_text(json.dumps(safe(outcome), indent=2) + '\n')
        manifest['cases'].append(dict(case=case, outcome=outcome['outcome']))
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print('END ' + case + ' ' + outcome['outcome'], flush=True)
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
