#!/usr/bin/env python3
"""Live M1 seam-gate validation for the modified-Option-A controlled stop.

Modified Option A authors the soft-stop trajectory in the backend and dispatches
it to STOCK ``joint_trajectory_controller/JointTrajectoryController`` as a
*preempting* ``FollowJointTrajectory`` goal. Continuity across the swap depends
on a "seam" anchor: the replacement's leading point reproduces the current
(q, qdot) so the controller's spline starts from the live state instead of
snapping or decelerating-on-cancel.

This probe is the M1 gate. It drives the arm, then preempts the in-flight goal
on stock JTC and measures the COMMANDED-reference continuity at the seam from
``<controller>/controller_state``. It contrasts three preempt strategies:

  anchored          Direct preempt; leading point at a small NEGATIVE
                    ``time_from_start`` carrying the current (q, qdot) so the
                    spline is pinned through the live state. The production
                    technique.
  naive_hold        Direct preempt; leading point at +t with ZERO velocity at
                    the current position. Tests whether JTC's own
                    before-trajectory interpolation alone keeps the command
                    continuous, or whether the velocity step shows up as a bump.
  cancel_then_send  Cancel the active goal, wait, then send the ramp. Reproduces
                    the classic decelerate-on-cancel discontinuity. Negative
                    control.

Pass (anchored): the goal is accepted AND the worst single-sample reference
velocity step around the seam is comparable to the steady-motion step (no
command discontinuity) AND the reference velocity does not reverse against the
travel direction (no backward bump).

Stock JTC only: ``FollowJointTrajectory`` action + ``controller_state`` topic;
no controller-native plugin messages.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
from dataclasses import asdict, dataclass, field
from typing import Any, Callable, Sequence

import rclpy
from action_msgs.msg import GoalStatus
from control_msgs.action import FollowJointTrajectory
from control_msgs.msg import JointTrajectoryControllerState, SpeedScalingFactor
from rclpy.action import ActionClient
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import JointState
from trajectory_msgs.msg import JointTrajectoryPoint

ARM_JOINTS = [
    "shoulder_pan_joint",
    "shoulder_lift_joint",
    "elbow_joint",
    "wrist_1_joint",
    "wrist_2_joint",
    "wrist_3_joint",
]

STRATEGIES = ["anchored", "naive_hold", "cancel_then_send"]


@dataclass
class StateSample:
    """One ``controller_state`` observation, stamped on monotonic receipt."""

    t_mono: float
    ref_pos: list[float]
    ref_vel: list[float]
    fb_pos: list[float]
    fb_vel: list[float]


@dataclass
class CaseResult:
    name: str
    strategy: str
    passed: bool
    accepted: bool
    details: dict[str, Any] = field(default_factory=dict)
    samples: list[dict[str, Any]] = field(default_factory=list)


class SeamProbe(Node):
    def __init__(self, args: argparse.Namespace) -> None:
        super().__init__("manyforge_isaac_seam_probe")
        self.args = args
        self.joints = list(args.required_joints)
        self.latest_joint_state: dict[str, float] = {}
        self.latest_isaac_joint_state: dict[str, float] = {}
        self.latest_state: StateSample | None = None
        self.recording = False
        self.record: list[StateSample] = []
        self._dispatch_latency_s = 0.0

        self.action_client = ActionClient(
            self, FollowJointTrajectory, args.action_name
        )
        scaling_qos = QoSProfile(
            depth=1,
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.TRANSIENT_LOCAL,
        )
        self.scaling_pub = self.create_publisher(
            SpeedScalingFactor, args.speed_scaling_topic, scaling_qos
        )
        self.create_subscription(
            JointState, args.joint_state_topic, self._on_joint_state, 10
        )
        self.create_subscription(
            JointState,
            args.isaac_joint_state_topic,
            self._on_isaac_joint_state,
            10,
        )
        self.create_subscription(
            JointTrajectoryControllerState,
            args.controller_state_topic,
            self._on_controller_state,
            50,
        )

    # ---- subscriptions -----------------------------------------------------

    def _positions_by_name(self, msg: JointState) -> dict[str, float]:
        values = dict(zip(msg.name, msg.position, strict=False))
        if all(joint in values for joint in self.joints):
            return {joint: float(values[joint]) for joint in self.joints}
        return {}

    def _on_joint_state(self, msg: JointState) -> None:
        positions = self._positions_by_name(msg)
        if positions:
            self.latest_joint_state = positions

    def _on_isaac_joint_state(self, msg: JointState) -> None:
        positions = self._positions_by_name(msg)
        if positions:
            self.latest_isaac_joint_state = positions

    def _ordered(self, names: Sequence[str], values: Sequence[float]) -> list[float]:
        index = {name: i for i, name in enumerate(names)}
        if not all(joint in index for joint in self.joints):
            return []
        return [float(values[index[joint]]) for joint in self.joints]

    def _on_controller_state(self, msg: JointTrajectoryControllerState) -> None:
        ref_pos = self._ordered(msg.joint_names, msg.reference.positions)
        ref_vel = self._ordered(msg.joint_names, msg.reference.velocities)
        fb_pos = self._ordered(msg.joint_names, msg.feedback.positions)
        fb_vel = self._ordered(msg.joint_names, msg.feedback.velocities)
        if not ref_pos or not ref_vel:
            return
        sample = StateSample(
            t_mono=time.monotonic(),
            ref_pos=ref_pos,
            ref_vel=ref_vel,
            fb_pos=fb_pos or [0.0] * len(self.joints),
            fb_vel=fb_vel or [0.0] * len(self.joints),
        )
        self.latest_state = sample
        if self.recording:
            self.record.append(sample)

    # ---- spin helpers ------------------------------------------------------

    def spin_until(self, predicate: Callable[[], bool], timeout_s: float) -> bool:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            rclpy.spin_once(self, timeout_sec=0.02)
            if predicate():
                return True
        return predicate()

    def spin_for(self, duration_s: float) -> None:
        deadline = time.monotonic() + duration_s
        while time.monotonic() < deadline:
            rclpy.spin_once(
                self, timeout_sec=min(0.02, max(0.0, deadline - time.monotonic()))
            )

    def require_ready(self) -> None:
        checks = [
            (
                "FollowJointTrajectory action server",
                lambda: self.action_client.wait_for_server(0.0),
            ),
            (
                "controller_state publisher",
                lambda: self.count_publishers(self.args.controller_state_topic) > 0,
            ),
            ("controller_state sample", lambda: self.latest_state is not None),
            ("aggregated joint-state sample", lambda: bool(self.latest_joint_state)),
        ]
        deadline = time.monotonic() + self.args.ready_timeout_s
        missing: list[str] = []
        while time.monotonic() < deadline:
            rclpy.spin_once(self, timeout_sec=0.1)
            missing = [label for label, pred in checks if not pred()]
            if not missing:
                return
        raise RuntimeError("Readiness failed: " + ", ".join(missing))

    # ---- trajectory building ----------------------------------------------

    def _point(
        self,
        positions: Sequence[float],
        t_s: float,
        velocities: Sequence[float] | None = None,
    ) -> JointTrajectoryPoint:
        point = JointTrajectoryPoint()
        point.positions = [float(v) for v in positions]
        point.velocities = [
            float(v)
            for v in (velocities if velocities is not None else [0.0] * len(positions))
        ]
        # ``builtin_interfaces/Duration`` is signed in sec; negative anchors carry
        # a negative sec and a positive nanosec remainder that sums correctly.
        sec = int(t_s // 1)
        nsec = int(round((t_s - sec) * 1_000_000_000))
        point.time_from_start.sec = sec
        point.time_from_start.nanosec = nsec
        return point

    def _goal(self, points: Sequence[JointTrajectoryPoint]) -> FollowJointTrajectory.Goal:
        goal = FollowJointTrajectory.Goal()
        goal.trajectory.joint_names = list(self.joints)
        goal.trajectory.points = list(points)
        return goal

    def send_motion_goal(self) -> Any:
        """A short ramp-then-cruise so the arm has a clean nonzero mid velocity."""
        start = [self.latest_joint_state[joint] for joint in self.joints]
        scale = float(self.args.motion_scale)
        dur = float(self.args.trajectory_duration_s)
        # Lead joints move; keep amplitudes modest for a sim that is being watched.
        delta = [0.0] * len(self.joints)
        for idx, amp in ((0, 0.18), (1, -0.12), (2, 0.10)):
            if idx < len(delta):
                delta[idx] = amp * scale
        mid = [s + d * 0.5 for s, d in zip(start, delta, strict=False)]
        end = [s + d for s, d in zip(start, delta, strict=False)]
        # Cruise velocity at the midpoint ~ chord / duration.
        cruise = [d / dur for d in delta]
        points = [
            self._point(start, 0.0, [0.0] * len(self.joints)),
            self._point(mid, dur * 0.5, cruise),
            self._point(end, dur, [0.0] * len(self.joints)),
        ]
        future = self.action_client.send_goal_async(self._goal(points))
        rclpy.spin_until_future_complete(
            self, future, timeout_sec=self.args.command_timeout_s
        )
        handle = future.result()
        if handle is None or not handle.accepted:
            raise RuntimeError("motion goal was not accepted")
        return handle

    def _advance_decel_state(
        self,
        q: Sequence[float],
        qd: Sequence[float],
        dt: float,
        decel_s: float,
    ) -> tuple[list[float], list[float]]:
        """Advance (q, qd) along the constant-decel model by ``dt`` seconds."""
        q_adv = [
            qj + qdj * dt - 0.5 * (qdj / decel_s) * dt * dt
            for qj, qdj in zip(q, qd, strict=False)
        ]
        qd_adv = [qdj * (1.0 - dt / decel_s) for qdj in qd]
        return q_adv, qd_adv

    def _decel_ramp(
        self,
        q_now: Sequence[float],
        qd_now: Sequence[float],
        anchor_t: float,
        decel_s: float | None = None,
    ) -> list[JointTrajectoryPoint]:
        """Constant-deceleration ramp from (q_now, qd_now) to rest over decel_s.

        ``v(t) = qd*(1 - t/T)``; ``q(t) = q + qd*t - 0.5*(qd/T)*t^2``. The leading
        point sits at ``anchor_t`` (negative for the anchored strategy) carrying
        the live (q, qdot); the remainder ramp to zero velocity at ``T``.
        """
        decel_s = float(self.args.deceleration_time_s if decel_s is None else decel_s)
        steps = max(2, int(self.args.ramp_points))
        points = [self._point(q_now, anchor_t, qd_now)]
        for k in range(1, steps + 1):
            t = decel_s * k / steps
            frac = 1.0 - t / decel_s
            pos = [
                q + qd * t - 0.5 * (qd / decel_s) * t * t
                for q, qd in zip(q_now, qd_now, strict=False)
            ]
            vel = [qd * frac for qd in qd_now]
            points.append(self._point(pos, t, vel))
        return points

    # ---- case execution ----------------------------------------------------

    def _current_ref(self) -> tuple[list[float], list[float]]:
        if self.latest_state is None:
            raise RuntimeError("no controller_state sample available")
        src = self.latest_state
        if self.args.anchor_source == "feedback":
            return list(src.fb_pos), list(src.fb_vel)
        return list(src.ref_pos), list(src.ref_vel)

    def run_case(self, name: str, strategy: str) -> CaseResult:
        if self.args.scaling_factor is not None:
            self._publish_scaling(self.args.scaling_factor, 0.3)

        handle = self.send_motion_goal()
        # Let the motion reach a clean nonzero velocity before preempting.
        self.spin_for(self.args.pre_stop_delay_s)

        self.record = []
        self.recording = True
        # A short pre-roll establishes the steady-motion baseline step.
        self.spin_for(self.args.pre_roll_s)

        q_now, qd_now = self._current_ref()
        max_speed = max((abs(v) for v in qd_now), default=0.0)

        if strategy == "cancel_then_send":
            cancel_future = handle.cancel_goal_async()
            rclpy.spin_until_future_complete(self, cancel_future, timeout_sec=1.0)
            self.spin_for(self.args.cancel_settle_s)

        t_preempt = time.monotonic()
        accepted, preempt_handle = self._send_preempt(strategy, q_now, qd_now)

        self.spin_for(self.args.capture_s)
        self.recording = False

        details = self._analyze(t_preempt, qd_now, max_speed)
        details["strategy"] = strategy
        details["preempt_accepted"] = accepted
        details["anchor_source"] = self.args.anchor_source
        details["max_speed_at_preempt_rad_s"] = round(max_speed, 6)
        details["dispatch_send_accept_latency_s"] = round(self._dispatch_latency_s, 5)
        details["dispatch_latency_comp_s"] = float(self.args.dispatch_latency_comp_s)
        details["sample_count"] = len(self.record)

        passed = self._verdict(strategy, accepted, details, max_speed)
        samples = self._sample_export(t_preempt) if self.args.emit_samples else []

        # Best-effort: stop any lingering goal so the next case starts clean.
        self._safe_cancel(preempt_handle)
        self._safe_cancel(handle)
        return CaseResult(
            name=name,
            strategy=strategy,
            passed=passed,
            accepted=accepted,
            details=details,
            samples=samples,
        )

    def _send_preempt(
        self, strategy: str, q_now: Sequence[float], qd_now: Sequence[float]
    ) -> tuple[bool, Any]:
        if strategy == "naive_hold":
            points = [
                self._point(q_now, self.args.naive_hold_t, [0.0] * len(self.joints))
            ]
        elif strategy == "anchored":
            comp = float(self.args.dispatch_latency_comp_s)
            decel_s = float(self.args.deceleration_time_s)
            if 0.0 < comp < decel_s:
                # Fast-forward the plan by the estimated trip->dispatch gap so the
                # anchor lands on the state the controller actually holds at swap,
                # not the (stale) trip state. The production path does this with a
                # measured latency and a re-sampled dispatch state.
                q_now, qd_now = self._advance_decel_state(
                    q_now, qd_now, comp, decel_s
                )
                points = self._decel_ramp(
                    q_now,
                    qd_now,
                    self.args.anchor_negative_time_s,
                    decel_s=decel_s - comp,
                )
            else:
                points = self._decel_ramp(
                    q_now, qd_now, self.args.anchor_negative_time_s
                )
        else:  # cancel_then_send: motion is already arrested; ramp from rest.
            points = self._decel_ramp(
                q_now, [0.0] * len(self.joints), self.args.naive_hold_t
            )
        t_send = time.monotonic()
        future = self.action_client.send_goal_async(self._goal(points))
        rclpy.spin_until_future_complete(
            self, future, timeout_sec=self.args.command_timeout_s
        )
        self._dispatch_latency_s = time.monotonic() - t_send
        handle = future.result()
        accepted = bool(handle is not None and handle.accepted)
        return accepted, handle

    def _safe_cancel(self, handle: Any) -> None:
        if handle is None or not getattr(handle, "accepted", False):
            return
        try:
            status = getattr(handle, "status", None)
            if status in (GoalStatus.STATUS_SUCCEEDED, GoalStatus.STATUS_CANCELED):
                return
            future = handle.cancel_goal_async()
            rclpy.spin_until_future_complete(self, future, timeout_sec=0.5)
        except Exception:
            pass

    def _publish_scaling(self, factor: float, duration_s: float) -> None:
        msg = SpeedScalingFactor()
        msg.factor = float(factor)
        deadline = time.monotonic() + duration_s
        while time.monotonic() < deadline:
            self.scaling_pub.publish(msg)
            rclpy.spin_once(self, timeout_sec=0.02)

    # ---- analysis ----------------------------------------------------------

    def _analyze(
        self, t_preempt: float, qd_now: Sequence[float], max_speed: float
    ) -> dict[str, Any]:
        rec = self.record
        if len(rec) < 3:
            return {"error": "insufficient samples", "samples": len(rec)}

        def max_step(samples: list[StateSample]) -> float:
            worst = 0.0
            for i in range(1, len(samples)):
                step = max(
                    abs(a - b)
                    for a, b in zip(
                        samples[i].ref_vel, samples[i - 1].ref_vel, strict=False
                    )
                )
                worst = max(worst, step)
            return worst

        eps = float(self.args.seam_window_s)
        pre = [s for s in rec if s.t_mono < t_preempt - 1e-4]
        seam = [s for s in rec if t_preempt - eps <= s.t_mono <= t_preempt + eps]
        post = [s for s in rec if s.t_mono > t_preempt + 1e-4]

        baseline_step = max_step(pre) if len(pre) >= 2 else 0.0
        seam_step = max_step(seam) if len(seam) >= 2 else 0.0
        ratio = seam_step / baseline_step if baseline_step > 1e-6 else float("inf")

        # Backward reversal: reference velocity going against travel direction.
        signs = [1.0 if v >= 0 else -1.0 for v in qd_now]
        reversal = 0.0
        for s in post:
            for j, v in enumerate(s.ref_vel):
                against = -v * signs[j]  # >0 when v opposes the travel direction
                reversal = max(reversal, against)

        # Settle time: first post-preempt instant the command is at rest.
        tol = float(self.args.stopped_velocity_tol_rad_s)
        settle_s: float | None = None
        for s in post:
            if max(abs(v) for v in s.ref_vel) <= tol:
                settle_s = s.t_mono - t_preempt
                break

        # Reference vs feedback gap at preempt (does anchoring on feedback inject
        # a command step the reference would not otherwise have?).
        ref_fb_gap = 0.0
        if self.latest_state is not None:
            ref_fb_gap = max(
                (
                    abs(a - b)
                    for a, b in zip(
                        self.latest_state.ref_pos,
                        self.latest_state.fb_pos,
                        strict=False,
                    )
                ),
                default=0.0,
            )

        publish_dt = 0.0
        if len(rec) >= 2:
            publish_dt = statistics.median(
                rec[i].t_mono - rec[i - 1].t_mono for i in range(1, len(rec))
            )

        return {
            "baseline_ref_vel_step_rad_s": round(baseline_step, 6),
            "seam_ref_vel_step_rad_s": round(seam_step, 6),
            "seam_step_ratio": (round(ratio, 3) if ratio != float("inf") else None),
            "backward_reversal_rad_s": round(reversal, 6),
            "settle_time_s": (round(settle_s, 4) if settle_s is not None else None),
            "ref_feedback_pos_gap_rad": round(ref_fb_gap, 6),
            "controller_state_dt_s": round(publish_dt, 5),
            "pre_samples": len(pre),
            "seam_samples": len(seam),
            "post_samples": len(post),
        }

    def _verdict(
        self,
        strategy: str,
        accepted: bool,
        details: dict[str, Any],
        max_speed: float,
    ) -> bool:
        if strategy != "anchored":
            # Contrast strategies are informative, not pass/fail gates.
            return accepted
        if not accepted:
            return False
        if max_speed < float(self.args.min_preempt_speed_rad_s):
            # No real motion to preempt: the case is inconclusive, not a pass.
            return False
        ratio = details.get("seam_step_ratio")
        reversal = details.get("backward_reversal_rad_s", 0.0)
        step = details.get("seam_ref_vel_step_rad_s", 0.0)
        ratio_ok = ratio is not None and ratio <= float(self.args.max_seam_ratio)
        # Absolute floor so a tiny baseline does not make the ratio explode on a
        # physically negligible step.
        step_ok = step <= float(self.args.max_seam_abs_step_rad_s)
        reversal_ok = reversal <= float(self.args.max_backward_reversal_rad_s)
        return bool((ratio_ok or step_ok) and reversal_ok)

    def _sample_export(self, t_preempt: float) -> list[dict[str, Any]]:
        out = []
        for s in self.record:
            out.append(
                {
                    "t_rel_s": round(s.t_mono - t_preempt, 5),
                    "ref_vel": [round(v, 5) for v in s.ref_vel],
                    "fb_vel": [round(v, 5) for v in s.fb_vel],
                    "ref_pos": [round(v, 5) for v in s.ref_pos],
                    "fb_pos": [round(v, 5) for v in s.fb_pos],
                }
            )
        return out


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--action-name", default="/isaac_arm_controller/follow_joint_trajectory"
    )
    parser.add_argument(
        "--controller-state-topic",
        default="/isaac_arm_controller/controller_state",
    )
    parser.add_argument("--joint-state-topic", default="/joint_states")
    parser.add_argument("--isaac-joint-state-topic", default="/isaac_joint_states")
    parser.add_argument(
        "--speed-scaling-topic", default="/isaac_arm_controller/speed_scaling_input"
    )
    parser.add_argument("--required-joints", nargs="+", default=ARM_JOINTS)
    parser.add_argument(
        "--cases", nargs="+", choices=STRATEGIES, default=["anchored"]
    )
    parser.add_argument(
        "--anchor-source",
        choices=["feedback", "reference"],
        default="feedback",
        help="State carried in the seam anchor: measured feedback (production) "
        "or commanded reference.",
    )
    parser.add_argument("--ready-timeout-s", type=float, default=20.0)
    parser.add_argument("--command-timeout-s", type=float, default=5.0)
    parser.add_argument("--trajectory-duration-s", type=float, default=3.0)
    parser.add_argument("--motion-scale", type=float, default=1.0)
    parser.add_argument("--pre-stop-delay-s", type=float, default=0.6)
    parser.add_argument("--pre-roll-s", type=float, default=0.25)
    parser.add_argument("--capture-s", type=float, default=1.8)
    parser.add_argument("--cancel-settle-s", type=float, default=0.25)
    parser.add_argument("--deceleration-time-s", type=float, default=0.8)
    parser.add_argument("--ramp-points", type=int, default=24)
    parser.add_argument("--anchor-negative-time-s", type=float, default=-0.01)
    parser.add_argument(
        "--dispatch-latency-comp-s",
        type=float,
        default=0.0,
        help="Anchored only: fast-forward the plan by this estimated "
        "trip->dispatch gap (s) before sending, so the anchor matches the "
        "controller's swap-time state. 0 disables (uncompensated baseline).",
    )
    parser.add_argument("--naive-hold-t", type=float, default=0.03)
    parser.add_argument("--seam-window-s", type=float, default=0.06)
    parser.add_argument("--scaling-factor", type=float, default=None)
    # Verdict thresholds (anchored).
    parser.add_argument("--min-preempt-speed-rad-s", type=float, default=0.01)
    parser.add_argument("--max-seam-ratio", type=float, default=4.0)
    parser.add_argument("--max-seam-abs-step-rad-s", type=float, default=0.05)
    parser.add_argument("--max-backward-reversal-rad-s", type=float, default=0.02)
    parser.add_argument("--stopped-velocity-tol-rad-s", type=float, default=0.01)
    parser.add_argument(
        "--emit-samples",
        action="store_true",
        help="Include the per-sample reference/feedback time series in output.",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    rclpy.init()
    node = SeamProbe(args)
    results: list[CaseResult] = []
    try:
        node.require_ready()
        for strategy in args.cases:
            result = node.run_case(strategy, strategy)
            results.append(result)
            print(
                f"[{strategy}] accepted={result.accepted} passed={result.passed} "
                + json.dumps(result.details),
                file=sys.stderr,
                flush=True,
            )
        passed = all(r.passed for r in results)
        print(
            json.dumps(
                {"passed": passed, "cases": [asdict(r) for r in results]}, indent=2
            )
        )
        return 0 if passed else 1
    except Exception as exc:  # noqa: BLE001 - surface any live-lane failure as JSON
        print(
            json.dumps(
                {
                    "passed": False,
                    "error": str(exc),
                    "cases": [asdict(r) for r in results],
                },
                indent=2,
            ),
            file=sys.stderr,
        )
        return 1
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
