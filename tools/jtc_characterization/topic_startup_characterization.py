#!/usr/bin/env python3
"""Bounded stock topic-hardware startup observations; never a host/guard proof."""
from __future__ import annotations

import argparse
from collections import deque
import hashlib
import json
from pathlib import Path
import subprocess
import time
import traceback

import rclpy
from controller_manager_msgs.srv import (
    ConfigureController, ListControllers, ListHardwareComponents, LoadController,
    SwitchController,
)
from rosidl_runtime_py.convert import message_to_ordereddict
from sensor_msgs.msg import JointState
from std_msgs.msg import String

from jtc_characterization import Probe, duration, ns, safe


def run(probe: Probe, activation_from_command: bool) -> dict:
    # No GenericSystem runs: replace its authored fixture before starting CM.
    probe.urdf = probe.urdf.replace(
        '<plugin>mock_components/GenericSystem</plugin>',
        '<plugin>topic_based_ros2_control/TopicBasedSystem</plugin>\n'
        '<param name="joint_commands_topic">/startup_commands</param>\n'
        '<param name="joint_states_topic">/startup_feedback</param>\n'
        '<param name="trigger_joint_command_threshold">-1</param>')
    (probe.output / 'robot.urdf').write_text(probe.urdf)
    yaml_path = probe.output / 'controllers.yaml'
    yaml_path.write_text(yaml_path.read_text().replace(
        '    arm:\n',
        '    joint_state_broadcaster:\n'
        '      type: joint_state_broadcaster/JointStateBroadcaster\n'
        '    arm:\n', 1).replace(
        '    interpolation_method: splines\n',
        '    interpolation_method: splines\n'
        f'    set_last_command_interface_value_as_state_on_activation: {str(activation_from_command).lower()}\n'))
    commands: deque[dict] = deque(maxlen=1000)
    hardware_states: deque[dict] = deque(maxlen=1000)

    def record(kind: str, storage: deque, msg: JointState) -> None:
        sample = dict(local_ns=time.monotonic_ns(), source_ns=ns(msg.header.stamp),
                      message=message_to_ordereddict(msg))
        storage.append(sample)
        probe.event(kind, **sample)

    # Subscribers exist before CM can publish any seeded command.
    probe.create_subscription(JointState, '/startup_commands',
                              lambda msg: record('raw_command', commands, msg), 1000)
    probe.create_subscription(JointState, '/joint_states',
                              lambda msg: record('hardware_exported_state', hardware_states, msg), 1000)
    feedback = probe.create_publisher(JointState, '/startup_feedback', 10)
    probe.event('startup_fixture', urdf_seed=1.2, actual_feedback=0.25,
                guard='not wired; CLOSED is a required external startup premise',
                interpolate_from_desired_state=True,
                set_last_command_interface_value_as_state_on_activation=activation_from_command,
                startup_policy='pending maintainer; comparison is fixture data only',
                scope='raw source characterization only; no host/guard/effect qualification')
    probe.manager = subprocess.Popen([
        'ros2', 'run', 'controller_manager', 'ros2_control_node', '--ros-args',
        '--params-file', str(yaml_path), '-r', '~/robot_description:=/robot_description'],
        stdout=probe.manager_log, stderr=subprocess.STDOUT, start_new_session=True)
    probe.event('manager_start', pid=probe.manager.pid)
    probe.description_pub.publish(String(data=probe.urdf))
    for controller in ('joint_state_broadcaster', 'arm'):
        for type_, name in ((LoadController, 'load_controller'),
                            (ConfigureController, 'configure_controller')):
            result = probe.service(type_, name, type_.Request(name=controller))
            if not result.ok:
                raise RuntimeError(f'{name} refused {controller}')
    switched = probe.service(SwitchController, 'switch_controller', SwitchController.Request(
        activate_controllers=['joint_state_broadcaster'], strictness=2,
        activate_asap=True, timeout=duration(3)))
    if not switched.ok:
        raise RuntimeError('joint state broadcaster activation refused')
    controllers = probe.service(ListControllers, 'list_controllers', ListControllers.Request())
    hardware = probe.service(ListHardwareComponents, 'list_hardware_components',
                             ListHardwareComponents.Request())
    if not any(c.name == 'arm' and c.state == 'inactive' for c in controllers.controller):
        raise RuntimeError('arm did not remain inactive before actual feedback')
    probe.event('inactive_phase', controllers=message_to_ordereddict(controllers),
                hardware=message_to_ordereddict(hardware))
    probe.spin(0.3)  # Fixture observation window, not a production timing policy.

    feedback_position = [0.25]

    def publish_feedback() -> None:
        msg = JointState(name=['joint1'], position=feedback_position.copy(), velocity=[0.0])
        msg.header.stamp = probe.get_clock().now().to_msg()
        probe.event('feedback_publish', source_ns=ns(msg.header.stamp),
                    message=message_to_ordereddict(msg))
        feedback.publish(msg)

    feedback_start = time.monotonic_ns()
    publish_feedback()
    probe.create_timer(0.01, publish_feedback)

    def actual_export() -> dict | None:
        for sample in reversed(hardware_states):
            msg = sample['message']
            if sample['local_ns'] >= feedback_start and 'joint1' in msg['name']:
                index = msg['name'].index('joint1')
                if len(msg['position']) > index and abs(msg['position'][index] - feedback_position[0]) < 1e-10:
                    return sample
        return None

    actual = probe.wait(actual_export, 5, 'actual feedback was not exported by hardware/JSB')
    probe.event('actual_hardware_feedback_observed', sample=actual)

    def activate_and_observe_hold(phase: str) -> dict:
        activation_request = time.monotonic_ns()
        activation_request_source = probe.get_clock().now().nanoseconds
        probe.switch(True)
        controllers = probe.service(ListControllers, 'list_controllers', ListControllers.Request())
        if not any(c.name == 'arm' and c.state == 'active' for c in controllers.controller):
            raise RuntimeError('JTC activation was not observed')
        probe.event('jtc_active_observed', phase=phase, controllers=message_to_ordereddict(controllers),
                    activation_request_local_ns=activation_request,
                    activation_request_source_ns=activation_request_source)

        def stable_hold() -> dict | None:
            samples = [s for s in probe.samples if s['local_ns'] >= activation_request
                       and s['source_ns'] >= activation_request_source]
            if len(samples) < 3:
                return None
            last = samples[-3:]
            if not all(last[i]['source_ns'] < last[i + 1]['source_ns'] for i in range(2)):
                return None
            if all(len(s['reference']['positions']) == 1 and
                   len(s['reference']['velocities']) == 1 and
                   len(s['output']['positions']) == 1 and
                   abs(s['reference']['velocities'][0]) < 1e-10 for s in last):
                if all(abs(s['output']['positions'][0] - last[-1]['output']['positions'][0]) < 1e-10
                       and abs(s['reference']['positions'][0] - last[-1]['reference']['positions'][0]) < 1e-10
                       for s in last):
                    return last[-1]
            return None

        hold = probe.wait(stable_hold, 5, 'no three-sample stationary JTC hold was observed')
        probe.event('fixture_hold_observed', phase=phase, sample=hold,
                    criterion='three fresh stationary reference/output samples; fixture only')
        cutoff = hold['source_ns']
        marker_local = time.monotonic_ns()
        probe.event('fixture_authorization_marker', phase=phase, cutoff_source_ns=cutoff,
                    authority='characterization marker only; no host/guard authorization was sent')
        probe.spin(0.2)
        later = [s for s in commands if s['source_ns'] > cutoff]
        return dict(phase=phase, actual_feedback=feedback_position[0], sample=hold,
                    hold_output_matches_actual=abs(hold['output']['positions'][0] - feedback_position[0]) < 1e-10,
                    authorization_marker_local_ns=marker_local, cutoff_source_ns=cutoff,
                    first_raw_command_newer_than_cutoff=later[0] if later else None,
                    raw_commands_newer_than_cutoff=later)

    startup = activate_and_observe_hold('startup')
    probe.event('fixture_guard_closed_marker', scope='no actual guard wired',
                reason='begin independent controller-reactivation characterization')
    probe.switch(False)
    controllers = probe.service(ListControllers, 'list_controllers', ListControllers.Request())
    if not any(c.name == 'arm' and c.state == 'inactive' for c in controllers.controller):
        raise RuntimeError('JTC did not become inactive for reactivation fixture')
    probe.event('jtc_inactive_observed', controllers=message_to_ordereddict(controllers))
    # Synthetic changed state while inactive distinguishes measured hold from
    # the last stored finite command. This is no physical displacement/effect.
    feedback_position[0] = 0.6
    feedback_start = time.monotonic_ns()
    publish_feedback()
    actual_reactivation = probe.wait(actual_export, 5, 'new actual fixture state not exported')
    probe.event('actual_hardware_feedback_observed', phase='reactivation',
                sample=actual_reactivation)
    reactivation = activate_and_observe_hold('reactivation')
    return dict(outcome='observed', actual_hardware_sample=actual,
                reactivation_hardware_sample=actual_reactivation,
                startup=startup, reactivation=reactivation,
                set_last_command_interface_value_as_state_on_activation=activation_from_command,
                startup_policy='pending maintainer; comparison data only',
                total_raw_commands=len(commands),
                guard_effect_qualification=False, host_startup_qualification=False)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--activation-from-command', choices=('true', 'false'), required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    rclpy.init()
    probe = Probe(args.output / 'startup', 'stock_topic_startup_' + args.activation_from_command, initial=1.2)
    result = {}
    try:
        result = run(probe, args.activation_from_command == 'true')
    except Exception as error:
        result = dict(outcome='harness_error', error=str(error), traceback=traceback.format_exc())
        probe.event('harness_error', **result)
    finally:
        probe.close()
        rclpy.shutdown()
    result['source_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    (args.output / 'outcome.json').write_text(json.dumps(safe(result), indent=2, allow_nan=False) + '\n')
    return 0 if result['outcome'] == 'observed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
