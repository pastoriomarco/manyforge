#!/usr/bin/env python3
"""Bounded stock-JTC/GenericSystem experiment; never a product executor."""
from __future__ import annotations

import argparse
from collections import deque
import hashlib
import json
import math
import os
from numbers import Integral, Real
from pathlib import Path
import platform
import signal
import subprocess
import time
import traceback
import xml.etree.ElementTree as ET

from old_main_stop_join import linear_stop, shift_latency, wire_points

import rclpy
from rclpy.action import ActionClient
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
from builtin_interfaces.msg import Duration
from control_msgs.action import FollowJointTrajectory
from control_msgs.msg import JointTolerance, JointTrajectoryControllerState, SpeedScalingFactor
from controller_manager_msgs.srv import (
    ConfigureController, ListHardwareComponents, LoadController,
    SetHardwareComponentState, SwitchController,
)
from lifecycle_msgs.msg import State
from rosidl_runtime_py.convert import message_to_ordereddict
from std_msgs.msg import String
from trajectory_msgs.msg import JointTrajectoryPoint


def ns(stamp: object) -> int:
    return stamp.sec * 1_000_000_000 + stamp.nanosec


def duration(seconds: float) -> Duration:
    value = round(seconds * 1_000_000_000)
    return Duration(sec=value // 1_000_000_000, nanosec=value % 1_000_000_000)


def safe(value: object) -> object:
    if isinstance(value, bool):
        return value
    if isinstance(value, dict):
        return {str(key): safe(item) for key, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [safe(item) for item in value]
    if isinstance(value, float) and not math.isfinite(value):
        return str(value)
    if isinstance(value, Integral) and not isinstance(value, bool):
        return int(value)
    if isinstance(value, Real):
        return float(value) if math.isfinite(value) else str(value)
    if hasattr(value, 'tolist'):
        return safe(value.tolist())
    return value


def point(t: float, q: float, v: float = 0.0, a: float = 0.0) -> JointTrajectoryPoint:
    return JointTrajectoryPoint(
        positions=[q], velocities=[v], accelerations=[a], time_from_start=duration(t))


def fixture(initial: float, stuck: bool, fixed_velocity: float = 0.0) -> tuple[str, str]:
    urdf = f'''<robot name="jtc_probe">
  <link name="base"/><link name="tip"/>
  <joint name="joint1" type="revolute"><parent link="base"/><child link="tip"/>
    <axis xyz="0 0 1"/><limit lower="-10" upper="10" velocity="2" effort="10"/>
  </joint>
  <ros2_control name="MockProbe" type="system"><hardware>
    <plugin>mock_components/GenericSystem</plugin>
    <param name="calculate_dynamics">true</param>
    <param name="disable_commands">{'true' if stuck else 'false'}</param>
  </hardware><joint name="joint1"><command_interface name="position"/>
    <state_interface name="position"><param name="initial_value">{initial}</param></state_interface>
    <state_interface name="velocity"><param name="initial_value">{fixed_velocity}</param></state_interface>
  </joint></ros2_control></robot>'''
    yaml = '''controller_manager:
  ros__parameters:
    update_rate: 100
    arm:
      type: joint_trajectory_controller/JointTrajectoryController
arm:
  ros__parameters:
    joints: [joint1]
    command_interfaces: [position]
    state_interfaces: [position, velocity]
    interpolation_method: splines
    interpolate_from_desired_state: true
    action_monitor_rate: 100.0
    speed_scaling:
      initial_scaling_factor: 1.0
    constraints:
      decelerate_on_cancel: true
      stopped_velocity_tolerance: 0.01
      goal_time: 0.25
      joint1:
        trajectory: 0.0
        goal: 0.002
        max_deceleration_on_cancel: 1.0
'''
    return urdf, yaml


class Probe(Node):
    def __init__(self, output: Path, case: str, initial: float = 0.0,
                 real_like: bool = False) -> None:
        super().__init__('jtc_characterization')
        self.output, self.case, self.real_like = output, case, real_like
        output.mkdir(parents=True)
        self.trace = (output / 'trace.jsonl').open('w')
        self.samples: deque[dict] = deque(maxlen=2000)
        self.handles: dict[str, object] = {}
        self.results: dict[str, dict] = {}
        self.manager = None
        self.manager_log = (output / 'controller-manager.log').open('w')
        fixed_feedback = case == 'cancel_zero_fixed_feedback'
        self.urdf, yaml = fixture(initial, case == 'real_goal_tolerance' or fixed_feedback,
                                  fixed_velocity=0.2 if fixed_feedback else 0.0)
        (output / 'robot.urdf').write_text(self.urdf)
        (output / 'controllers.yaml').write_text(yaml)
        self.sub = self.create_subscription(
            JointTrajectoryControllerState, '/arm/controller_state', self.on_sample,
            QoSProfile(depth=1000, reliability=ReliabilityPolicy.RELIABLE))
        self.scale_pub = self.create_publisher(
            SpeedScalingFactor, '/arm/speed_scaling_input',
            QoSProfile(depth=1, durability=DurabilityPolicy.TRANSIENT_LOCAL))
        self.description_pub = self.create_publisher(
            String, '/robot_description',
            QoSProfile(depth=1, durability=DurabilityPolicy.TRANSIENT_LOCAL))
        self.action = ActionClient(self, FollowJointTrajectory, '/arm/follow_joint_trajectory')
        self.event('fixture', real_like=real_like, initial_position=initial, update_rate=100,
                   fixed_velocity_feedback=0.2 if fixed_feedback else None,
                   command_propagation_disabled=case == 'real_goal_tolerance' or fixed_feedback,
                   qualification='functional mock observation; no timing/hardware qualification')

    def event(self, kind: str, **fields: object) -> None:
        record = safe({'kind': kind, 'case': self.case, 'local_ns': time.monotonic_ns(),
                       'ros_ns': self.get_clock().now().nanoseconds, **fields})
        self.trace.write(json.dumps(record, allow_nan=False) + '\n')
        self.trace.flush()

    def on_sample(self, msg: JointTrajectoryControllerState) -> None:
        record = dict(local_ns=time.monotonic_ns(), source_ns=ns(msg.header.stamp),
                      cursor_ns=ns(msg.reference.time_from_start),
                      factor=msg.speed_scaling_factor,
                      reference=message_to_ordereddict(msg.reference),
                      output=message_to_ordereddict(msg.output),
                      measured=message_to_ordereddict(msg.feedback),
                      error=message_to_ordereddict(msg.error))
        self.samples.append(record)
        self.event('sample', **record)

    def spin(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            rclpy.spin_once(self, timeout_sec=min(0.01, max(0.0, deadline - time.monotonic())))
            if self.manager is not None and self.manager.poll() is not None:
                raise RuntimeError(f'controller manager exited {self.manager.returncode}')

    def wait(self, predicate: object, timeout: float, description: str) -> object:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = predicate()
            if result:
                return result
            self.spin(0.01)
        raise TimeoutError(description)

    def future(self, future: object, timeout: float, description: str) -> object:
        self.wait(future.done, timeout, description)
        return future.result()

    def service(self, service_type: object, name: str, request: object) -> object:
        client = self.create_client(service_type, '/controller_manager/' + name)
        try:
            self.wait(lambda: client.service_is_ready(), 15, name + ' discovery')
            response = self.future(client.call_async(request), 5, name + ' response')
            self.event('service', service=name, request=message_to_ordereddict(request),
                       response=message_to_ordereddict(response))
            return response
        finally:
            self.destroy_client(client)

    def switch(self, activate: bool) -> None:
        response = self.service(SwitchController, 'switch_controller', SwitchController.Request(
            activate_controllers=['arm'] if activate else [],
            deactivate_controllers=[] if activate else ['arm'], strictness=2,
            activate_asap=True, timeout=duration(3)))
        if not response.ok:
            raise RuntimeError('controller switch refused')

    def start(self) -> None:
        self.manager = subprocess.Popen([
            'ros2', 'run', 'controller_manager', 'ros2_control_node', '--ros-args',
            '--params-file', str(self.output / 'controllers.yaml'),
            '-r', '~/robot_description:=/robot_description'],
            stdout=self.manager_log, stderr=subprocess.STDOUT, start_new_session=True)
        self.event('manager_start', pid=self.manager.pid)
        self.description_pub.publish(String(data=self.urdf))
        for type_, name in [(LoadController, 'load_controller'),
                            (ConfigureController, 'configure_controller')]:
            response = self.service(type_, name, type_.Request(name='arm'))
            if not response.ok:
                raise RuntimeError(name + ' refused')
        self.event('controller_configured_inactive')
        self.switch(True)
        self.wait(lambda: bool(self.samples), 5, 'first controller sample')
        self.wait(lambda: self.action.server_is_ready(), 5, 'FJT discovery')
        self.spin(0.1)

    def scale(self, value: float) -> None:
        self.event('scale_send', value=value)
        self.scale_pub.publish(SpeedScalingFactor(factor=value))

    def set_scale(self, value: float) -> None:
        sent = time.monotonic_ns()
        self.scale(value)
        self.wait(lambda: self.samples and self.samples[-1]['local_ns'] >= sent
                  and abs(self.samples[-1]['factor'] - value) < 1e-10, 3, 'factor observation')
        self.event('scale_value_observed', value=value,
                   caveat='value equality is not a command-generation acknowledgement')

    def goal(self, label: str, points: list[JointTrajectoryPoint],
             start_after: float = 0.0, invalid_joint: bool = False) -> object:
        goal = FollowJointTrajectory.Goal()
        goal.trajectory.joint_names = ['unknown'] if invalid_joint else ['joint1']
        goal.trajectory.points = points
        if start_after:
            goal.trajectory.header.stamp = (
                self.get_clock().now() + rclpy.duration.Duration(seconds=start_after)).to_msg()
        if not self.real_like:
            goal.goal_tolerance = [JointTolerance(
                name='joint1', position=-1.0, velocity=-1.0, acceleration=-1.0)]
        self.event('goal_send', label=label, goal=message_to_ordereddict(goal))

        def feedback(msg: object) -> None:
            self.event('goal_feedback', label=label, uuid=list(msg.goal_id.uuid),
                       feedback=message_to_ordereddict(msg.feedback))

        handle = self.future(self.action.send_goal_async(goal, feedback_callback=feedback),
                             5, 'goal acceptance')
        self.handles[label] = handle
        self.event('goal_response', label=label, accepted=handle.accepted,
                   uuid=list(handle.goal_id.uuid), acceptance_ns=ns(handle.stamp))
        if handle.accepted:
            def terminal(future: object) -> None:
                response = future.result()
                fact = dict(status=response.status,
                            error_code=response.result.error_code,
                            error_string=response.result.error_string,
                            local_ns=time.monotonic_ns())
                self.results[label] = fact
                self.event('goal_result', label=label, **fact)
            handle.get_result_async().add_done_callback(terminal)
        return handle

    def cancel(self, label: str) -> int:
        sent = time.monotonic_ns()
        self.event('cancel_send', label=label, uuid=list(self.handles[label].goal_id.uuid))
        response = self.future(self.handles[label].cancel_goal_async(), 5, 'cancel response')
        self.event('cancel_response', label=label, response=message_to_ordereddict(response))
        goal_id = list(self.handles[label].goal_id.uuid)
        if response.return_code != 0 or not any(
                list(goal.goal_id.uuid) == goal_id for goal in response.goals_canceling):
            raise RuntimeError('cancel did not acknowledge this accepted goal')
        return sent

    def moving(self, scale: float = 1.0) -> None:
        self.set_scale(scale)
        q = self.samples[-1]['reference']['positions'][0]
        handle = self.goal('moving', [point(0, q, 0.2), point(3, q + 0.6, 0.2),
                                      point(4, q + 0.7, 0)])
        if not handle.accepted:
            raise RuntimeError('moving precondition: goal rejected')
        self.wait(lambda: self.samples and self.samples[-1]['reference']['velocities']
                  and self.samples[-1]['reference']['velocities'][0] > 0.1
                  and self.samples[-1]['reference']['positions'][0] > q + 0.01,
                  3, 'moving precondition: reference movement not observed')
        if 'moving' in self.results:
            raise RuntimeError('moving precondition: goal already terminal')
        self.event('moving_precondition', accepted=True, sample=self.samples[-1],
                   association='reference movement consistent with accepted fixture trajectory')

    def close(self) -> None:
        if self.manager is not None:
            os_group = self.manager.pid
            try:
                import os
                os.killpg(os_group, signal.SIGTERM)
                self.manager.wait(timeout=5)
            except ProcessLookupError:
                pass
            except subprocess.TimeoutExpired:
                os.killpg(os_group, signal.SIGKILL)
                self.manager.wait(timeout=3)
        self.manager_log.close()
        self.trace.close()
        self.destroy_node()


def experiment(probe: Probe, name: str) -> dict:
    probe.start()
    if name.startswith('manymove_join_'):
        factor = 0.5 if name.endswith('half') else 1.0
        probe.moving(factor)
        trip = probe.samples[-1]
        t0 = time.monotonic_ns()
        deadline = t0 + 1_000_000_000
        cursor = trip['cursor_ns'] / 1e9
        lead, ramp = 0.15, 0.5
        planned = linear_stop(cursor, factor, lead, ramp)
        # The actual original moving goal has just its t=0 point before this
        # cursor. Keep that ORIGINAL q/v/a and shift its time by elapsed.
        # This probes ManyMove's past-point shape plus the approved analytical
        # lead/ramp fixture, not ManyMove's complete Stop destination policy.
        past = dict(t=-cursor / factor, q=0.0, v=0.2 * factor, a=0.0)
        authored = [past, *planned]
        exported = wire_points(authored, old_main=False, frozen_factor=factor)
        probe.event('main_join_trip', sample=trip, stop_request_local_ns=t0,
                    unchanged_deadline_local_ns=deadline, lead_in_wall_s=lead,
                    ramp_wall_s=ramp, planned_points=planned,
                    fixture='original past point shifted by controller cursor; analytical lead/ramp; not full ManyMove/Core planner')
        probe.event('main_join_export', measured_compute_latency_s=0.0,
                    authored_points=authored, exported_points=exported,
                    old_main_wire_clamps_negative=False,
                    old_main_unit_factor_assertion=False,
                    frozen_factor_conversion=True,
                    past_point_provenance='moving goal original t=0 q=0 v=.2 a=0; no fake past pin',
                    elapsed_basis='observed controller path cursor converted to wall time; not ManyMove pre-dispatch ROS clock origin',
                    unchanged_deadline_local_ns=deadline)
        handle = probe.goal('replacement', [point(wp['t'], wp['q'], wp['v'], wp['a'])
                                            for wp in exported])
        accepted_local_ns = time.monotonic_ns()
        while time.monotonic_ns() < deadline and 'replacement' not in probe.results:
            probe.spin(min(0.01, (deadline - time.monotonic_ns()) / 1e9))
        probe.event('main_join_observation_end', unchanged_deadline_local_ns=deadline,
                    replacement_accepted=handle.accepted,
                    acceptance_within_original_lead_in=(accepted_local_ns-t0)/1e9 <= lead,
                    acceptance_elapsed_s=(accepted_local_ns-t0)/1e9,
                    results=probe.results,
                    caveat='shape characterization; no full planner, transport budget or hard-floor qualification')
        return dict(outcome='observed', replacement_accepted=handle.accepted,
                    results=probe.results, trip=trip, exported_points=exported,
                    unchanged_deadline_local_ns=deadline,
                    final_sample=probe.samples[-1])
    if name.startswith('main_join_') or name.startswith('negative_join_'):
        factor = 0.5 if name.endswith('half') else 1.0
        old_main = name.startswith('main_join_')
        probe.moving(factor)
        trip = probe.samples[-1]
        t0 = time.monotonic_ns()
        deadline = t0 + 1_000_000_000
        lead, ramp = 0.15, 0.5
        cursor = trip['cursor_ns'] / 1e9
        planned = linear_stop(cursor, factor, lead, ramp)
        probe.event('main_join_trip', sample=trip, stop_request_local_ns=t0,
                    unchanged_deadline_local_ns=deadline, lead_in_wall_s=lead,
                    ramp_wall_s=ramp, planned_points=planned,
                    fixture='exact analytical linear-path candidate and historical shift; not full planner')
        probe.spin(0.035)
        latency = (time.monotonic_ns() - t0) / 1e9
        shifted = shift_latency(planned, latency, lead, cursor, factor)
        exported = wire_points(shifted, old_main=old_main, frozen_factor=factor)
        probe.event('main_join_export', measured_compute_latency_s=latency,
                    authored_points=shifted, exported_points=exported,
                    old_main_wire_clamps_negative=old_main,
                    old_main_unit_factor_assertion=old_main,
                    frozen_factor_conversion=not old_main,
                    unchanged_deadline_local_ns=deadline,
                    remaining_deadline_s=(deadline - time.monotonic_ns()) / 1e9)
        if old_main:
            # Old backend asserts unit scale immediately before send; no applied-factor barrier.
            probe.scale(1.0)
        handle = probe.goal('replacement', [point(wp['t'], wp['q'], wp['v'], wp['a'])
                                            for wp in exported])
        accepted_local_ns = time.monotonic_ns()
        acceptance_within_lead = (accepted_local_ns - t0) / 1e9 <= lead
        # The original observation cutoff is never renewed at acceptance.
        while time.monotonic_ns() < deadline and 'replacement' not in probe.results:
            probe.spin(min(0.01, (deadline - time.monotonic_ns()) / 1e9))
        probe.event('main_join_observation_end', unchanged_deadline_local_ns=deadline,
                    replacement_accepted=handle.accepted,
                    acceptance_within_original_lead_in=acceptance_within_lead,
                    acceptance_elapsed_s=(accepted_local_ns - t0) / 1e9,
                    results=probe.results,
                    caveat='fixed harness cutoff; Kernel hard-floor actuation not exercised')
        return dict(outcome='observed', replacement_accepted=handle.accepted,
                    acceptance_within_original_lead_in=acceptance_within_lead,
                    results=probe.results, trip=trip,
                    unchanged_deadline_local_ns=deadline,
                    measured_compute_latency_s=latency,
                    exported_points=exported, final_sample=probe.samples[-1])
    if name in ('ordinary', 'ordinary_real_like', 'real_goal_tolerance'):
        handle = probe.goal('ordinary', [point(0, 0), point(1.0, 0.2)])
        if not handle.accepted:
            raise RuntimeError('ordinary goal rejected')
        probe.wait(lambda: 'ordinary' in probe.results, 3, 'ordinary result')
        probe.spin(0.2)
        return dict(outcome='observed', result=probe.results['ordinary'],
                    final_sample=probe.samples[-1])
    if name.startswith('replace_'):
        scale = 0.5 if name.endswith('half') else 1.0
        probe.moving(scale)
        trip = probe.samples[-1]
        q = trip['reference']['positions'][0]
        lead_in = 0.15
        # Fixture only: copy a linear old path, then a finite braking segment.
        # Does not stand in for the production Stop planner or certify its limits.
        replacement = [point(lead_in * scale, q + 0.2 * lead_in * scale, 0.2),
                       point(0.5 * scale, q + 0.2 * lead_in * scale + 0.03, 0)]
        probe.event('replacement_trip', sample=trip, lead_in_wall_s=lead_in,
                    fixture='linear continuation plus test segment; not production Stop math')
        handle = probe.goal('replacement', replacement,
                            start_after=0.08 if 'absolute' in name else 0.0)
        if not handle.accepted:
            raise RuntimeError('replacement rejected')
        probe.wait(lambda: 'replacement' in probe.results, 4, 'replacement result')
        probe.spin(0.2)
        return dict(outcome='observed', results=probe.results, trip=trip,
                    final_sample=probe.samples[-1])
    if name.startswith('q35_cancel_'):
        requested = {'q35_cancel_quarter': 0.25, 'q35_cancel_half': 0.5,
                     'q35_cancel_three_quarters': 0.75}[name]
        probe.moving(requested)
        probe.wait(lambda: probe.samples[-1]['measured']['velocities'] and
                   probe.samples[-1]['measured']['velocities'][0] > 0.0 and
                   abs(probe.samples[-1]['factor'] - requested) < 1e-10,
                   3, 'Q35 fixture: nonzero mock measured velocity at applied factor')
        if 'moving' in probe.results:
            raise RuntimeError('Q35 fixture: moving goal already terminal before Stop')
        before = probe.samples[-1]
        frozen = before['factor']
        trigger = time.monotonic_ns()
        probe.event('q35_stop_trigger', stop_request_local_ns=trigger,
                    sample=before, frozen_applied_factor=frozen,
                    policy='Q35 actual-factor freeze, one identical pin, always cancel',
                    remedy_chosen=False)
        probe.event('q35_freeze_pin', value=frozen, pin_publications=1,
                    source_stamp_used=before['source_ns'], no_further_factor_publications=True,
                    caveat='stable factor already observed; identical-value resend has no generation ACK')
        probe.scale(frozen)  # Exactly one pin; no ramp or factor remedy.
        marker = probe.cancel('moving')  # Exact accepted UUID, never skip Cancel.
        probe.spin(1.3)
        probe.event('q35_observation_end', observation_after_ack_s=1.3,
                    stop_request_local_ns=trigger, cancel_send_local_ns=marker,
                    result=probe.results.get('moving'),
                    caveat='bounded stock position-mock observation, not physical stopping/deadline proof')
        return dict(outcome='observed', requested_factor=requested,
                    frozen_applied_factor=frozen, before_cancel=before,
                    final_sample=probe.samples[-1], result=probe.results.get('moving'),
                    remedy_chosen=False)
    if name.startswith('cancel_'):
        probe.moving(1.0)
        factor = {'cancel_one': 1.0, 'cancel_half': 0.5, 'cancel_zero': 0.0,
                  'cancel_zero_fixed_feedback': 0.0}[name]
        probe.set_scale(factor)
        before = probe.samples[-1]
        marker = probe.cancel('moving')
        probe.spin(1.3)
        samples = [s for s in probe.samples if s['local_ns'] > marker]
        zeros = [s for s in samples if s['reference']['velocities'] and
                 abs(s['reference']['velocities'][0]) < 1e-8]
        return dict(outcome='observed', factor=factor, result=probe.results.get('moving'),
                    before_cancel=before, after_cancel=probe.samples[-1],
                    zero_reference_seen=bool(zeros),
                    first_zero_local_ns=zeros[0]['local_ns'] if zeros else None,
                    association='post-request observation only; no assumed cancel consumption proof')
    if name == 'ramp_freeze':
        probe.moving()
        for value in [1.0 - 0.01 * i for i in range(1, 51)]:
            probe.scale(value)
            probe.spin(0.01)
        frozen_sent = time.monotonic_ns()
        probe.event('freeze', last_sent=0.5, no_more_publications=True)
        probe.spin(0.3)
        return dict(outcome='observed', last_sent=0.5, freeze_local_ns=frozen_sent,
                    final_factor=probe.samples[-1]['factor'])
    if name == 'rejected_replacement':
        probe.moving()
        before = probe.samples[-1]
        rejected = probe.goal('invalid', [point(0.2, 0.5)], invalid_joint=True)
        probe.spin(0.3)
        return dict(outcome='observed', invalid_accepted=rejected.accepted,
                    original_terminal=probe.results.get('moving'),
                    before=before, after=probe.samples[-1])
    if name == 'deactivation':
        probe.moving()
        probe.switch(False)
        probe.wait(lambda: 'moving' in probe.results, 3, 'deactivation result')
        return dict(outcome='observed', result=probe.results['moving'])
    if name == 'mock_reactivation':
        handle = probe.goal('move', [point(0, 0), point(0.8, 0.25)])
        if not handle.accepted:
            raise RuntimeError('mock reactivation movement rejected')
        probe.wait(lambda: 'move' in probe.results, 3, 'mock movement result')
        if probe.results['move']['error_code'] != 0:
            raise RuntimeError('mock reactivation movement failed')
        before = probe.samples[-1]
        probe.switch(False)
        transition = probe.service(SetHardwareComponentState, 'set_hardware_component_state',
            SetHardwareComponentState.Request(name='MockProbe', target_state=State(id=1)))
        if transition.ok:
            probe.service(SetHardwareComponentState, 'set_hardware_component_state',
                SetHardwareComponentState.Request(name='MockProbe', target_state=State(id=3)))
            probe.switch(True)
            probe.spin(0.2)
        return dict(outcome='observed', before=before, after=probe.samples[-1],
                    movement_result=probe.results['move'],
                    unconfigure_ok=transition.ok,
                    reseed='hardware lifecycle only; fixture initial value remains zero')
    if name == 'seeded_pose':
        return dict(outcome='observed', expected_initial=0.37, sample=probe.samples[-1])
    raise ValueError(name)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--cases', default='ordinary,ordinary_real_like,real_goal_tolerance,replace_zero_one,'
                        'replace_absolute_one,replace_zero_half,replace_absolute_half,'
                        'cancel_one,cancel_half,cancel_zero,cancel_zero_fixed_feedback,ramp_freeze,'
                        'rejected_replacement,deactivation,mock_reactivation,seeded_pose')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    versions = {}
    for package in ('joint_trajectory_controller', 'controller_manager', 'hardware_interface',
                    'rclpy', 'control_msgs', 'controller_manager_msgs', 'realtime_tools'):
        path = Path('/opt/ros') / os.environ['ROS_DISTRO'] / 'share' / package / 'package.xml'
        versions[package] = dict(version=ET.parse(path).findtext('version'),
                                 package_xml_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    manifest = dict(packages=versions, python=platform.python_version(),
                    source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    source_inputs={name: hashlib.sha256((Path(__file__).parent / name).read_bytes()).hexdigest()
                                   for name in ('jtc_characterization.py', 'old_main_stop_join.py')},
                    limitations=['isolated stock mock only', 'not physical stopping proof',
                                 'no timing qualification under shared host compiler load',
                                 'Isaac fork/guard not run; not implemented here'], cases=[])
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    failed = False
    for case in args.cases.split(','):
        print('START ' + case, flush=True)
        rclpy.init()
        probe = Probe(args.output / case, case, initial=0.37 if case == 'seeded_pose' else 0,
                      real_like=case in ('ordinary_real_like', 'real_goal_tolerance'))
        try:
            result = experiment(probe, case)
        except Exception as error:
            failed = True
            result = dict(outcome='harness_error', error=str(error), traceback=traceback.format_exc())
            probe.event('harness_error', **result)
        finally:
            probe.close()
            rclpy.shutdown()
        (args.output / case / 'outcome.json').write_text(
            json.dumps(safe(result), indent=2, allow_nan=False) + '\n')
        manifest['cases'].append(dict(case=case, **result))
        (args.output / 'manifest.json').write_text(
            json.dumps(safe(manifest), indent=2, allow_nan=False) + '\n')
        print('END ' + case + ' ' + result['outcome'], flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
