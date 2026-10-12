from __future__ import annotations

from pathlib import Path
import json
import sys
from threading import Event, Thread
from types import SimpleNamespace as NS
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from manyforge.isaac.command_guard.frame import project_joint_state
from manyforge.isaac.command_guard.runtime import GuardRuntime, RuntimeConfig, status_fields


class Clock:
    now = 10

    def __call__(self) -> int:
        return self.now


def ros_time(ns: int) -> NS:
    sec, nano = divmod(ns, 1_000_000_000)
    return NS(sec=sec, nanosec=nano)


def message(ns: int, names: tuple[str, ...] = ("a", "b"),
            positions: tuple[float, ...] = (0.1, 0.2), **fields: object) -> NS:
    return NS(header=NS(stamp=ros_time(ns)), name=names, position=positions,
              velocity=fields.get("velocity", ()), effort=fields.get("effort", ()))


def config(age: int = 100) -> RuntimeConfig:
    return RuntimeConfig("/World/arm", ("a", "b"), "/command", "/authorize", "/status", age)


class RuntimeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.clock = Clock()
        self.statuses = []
        self.writes = []
        self.runtime = GuardRuntime(config(), self.clock, self.statuses.append)
        self.runtime.bind_writer(self.writes.append)

    def authorize(self, value: int = 1000) -> None:
        self.assertTrue(self.runtime.receive_authorization(ros_time(value)))
        self.assertFalse(self.runtime.step())

    def test_source_projection_superset_joint_order_and_position_only(self) -> None:
        command = project_joint_state(message(1001, ("finger", "b", "a"), (9.0, 0.2, 0.1),
                                              velocity=(3.0, 4.0, 5.0), effort=(6.0, 7.0, 8.0)),
                                      config().joint_names)
        self.assertEqual(command.names, ("a", "b"))
        self.assertEqual(command.positions, (0.1, 0.2))
        self.assertEqual(command.velocities, ())
        self.assertEqual(command.efforts, ())
        self.authorize()
        self.runtime.receive_command(message(1001, ("finger", "b", "a"), (9.0, 0.2, 0.1)))
        self.assertEqual(self.writes, [])  # ROS callback never calls SDK.
        self.assertTrue(self.runtime.step())
        self.assertEqual(self.writes, [(0.1, 0.2)])

    def test_disjoint_gripper_does_not_refresh_or_trip(self) -> None:
        self.authorize()
        self.clock.now = 109
        self.assertFalse(self.runtime.receive_command(message(9000, ("finger",), (0.5,))))
        self.assertFalse(self.runtime.monitor().tripped)
        self.assertEqual(self.runtime.monitor().progress_receipt_ns, 10)
        self.clock.now = 110
        self.assertTrue(self.runtime.monitor().tripped)

    def test_partial_duplicate_and_invalid_arrays_refuse_without_sdk(self) -> None:
        self.authorize()
        for frame in (message(1001, ("a",), (0.2,)),
                      message(1001, ("a", "a", "b"), (0.1, 0.1, 0.2)),
                      message(1001, positions=(float("nan"), 0.2)),
                      message(1001, velocity=(0.1,))):
            with self.subTest(frame=frame), self.assertRaises(ValueError):
                self.runtime.receive_command(frame)
        self.assertFalse(self.runtime.monitor().tripped)
        self.assertEqual(self.runtime.monitor().progress_receipt_ns, 10)
        self.assertFalse(self.runtime.step())
        self.assertEqual(self.writes, [])

    def test_ready_precedes_authorization_and_no_early_time_is_buffered(self) -> None:
        runtime = GuardRuntime(config(), self.clock, self.statuses.append)
        self.assertFalse(runtime.receive_authorization(ros_time(2000)))
        runtime.bind_writer(self.writes.append)
        self.assertFalse(runtime.step())
        self.assertFalse(runtime.guard.monitor().authorized)

    def test_latest_pending_time_keeps_max_not_last_arrival(self) -> None:
        self.runtime.receive_authorization(ros_time(2000))
        self.runtime.receive_authorization(ros_time(1000))
        self.runtime.step()
        self.assertEqual(self.runtime.monitor().authorization_stamp_ns, 2000)

    def test_exact_status_negative_stamp_nonzero_nanoseconds_and_absence(self) -> None:
        self.runtime.publish_status()
        self.assertEqual(status_fields(self.statuses[-1], config().joint_names),
                         (("state", "closed"), ("joint_names", '["a","b"]')))
        self.authorize(-1_000_000_007)
        fields = dict(status_fields(self.statuses[-1], config().joint_names))
        self.assertEqual(fields["state"], "open")
        self.assertEqual(fields["authorized_after.sec"], "-2")
        self.assertEqual(fields["authorized_after.nanosec"], "999999993")

    def test_joint_name_status_preserves_jtc_order_and_json_escaping_on_every_state(self) -> None:
        names = ('joint\\2', 'joint"1', 'joint_\u03bc')
        cfg = RuntimeConfig("/World/arm", names, "/commands", "/authorize", "/status", 100)
        statuses = []
        runtime = GuardRuntime(cfg, self.clock, statuses.append)
        runtime.publish_status()
        runtime.bind_writer(self.writes.append)
        runtime.receive_authorization(ros_time(1000))
        runtime.step()
        self.clock.now = 110
        runtime.monitor()
        runtime.publish_status()
        self.assertEqual([dict(status_fields(status, names))["state"] for status in statuses],
                         ["closed", "open", "tripped"])
        for status in statuses:
            encoded = dict(status_fields(status, names))["joint_names"]
            self.assertEqual(json.loads(encoded), list(names))
            self.assertEqual(encoded, json.dumps(names, separators=(",", ":")))

    def test_new_authorization_publishes_even_open_to_open_without_command_flood(self) -> None:
        self.authorize()
        count = len(self.statuses)
        for value in (1001, 1002, 1003):
            self.runtime.receive_command(message(value))
            self.runtime.step()
            self.runtime.publish_status()
        self.assertEqual(len(self.statuses), count)
        self.authorize(2000)
        self.assertEqual(len(self.statuses), count + 1)
        self.assertEqual(self.statuses[-1].authorization_stamp_ns, 2000)

    def test_trip_retains_exact_authorization_and_first_reason(self) -> None:
        self.authorize()
        self.clock.now = 110
        self.runtime.monitor()
        self.runtime.publish_status()
        self.clock.now = 9
        self.runtime.monitor()
        self.runtime.publish_status()
        self.assertEqual(self.statuses[-1].reason, "command_age_expired")
        self.assertEqual(self.statuses[-1].authorization_stamp_ns, 1000)

    def test_atomic_trip_before_authorization_is_reported_before_new_open(self) -> None:
        self.authorize()
        self.clock.now = 110
        self.runtime.monitor()  # Trip not yet published.
        self.authorize(2000)
        self.assertTrue(self.statuses[-2].tripped)
        self.assertEqual(self.statuses[-2].authorization_stamp_ns, 1000)
        self.assertTrue(self.statuses[-1].authorized)
        self.assertEqual(self.statuses[-1].authorization_stamp_ns, 2000)

    def test_monitor_trip_independent_of_blocked_sdk_reserved_call_can_finish(self) -> None:
        entered, release, done = Event(), Event(), Event()

        def writer(positions: tuple[float, ...]) -> None:
            entered.set()
            if not release.wait(2):
                raise AssertionError("test SDK release deadline")
            self.writes.append(positions)

        self.runtime.bind_writer(writer)
        self.authorize()
        self.runtime.receive_command(message(1001))
        worker = Thread(target=lambda: (self.runtime.step(), done.set()))
        worker.start()
        try:
            self.assertTrue(entered.wait(2))
            self.clock.now = 110
            self.assertTrue(self.runtime.monitor().tripped)
            self.assertFalse(done.is_set())
        finally:
            release.set()
            worker.join(2)
        self.assertFalse(worker.is_alive())
        self.assertEqual(self.writes, [(0.1, 0.2)])  # One already reserved effect only.
        self.assertFalse(self.runtime.step())

    def test_monitor_trip_during_blocked_status_publish_preserves_open_then_trip(self) -> None:
        entered, release = Event(), Event()

        def publish(status: object) -> None:
            if status.authorized:
                entered.set()
                if not release.wait(2):
                    raise AssertionError("test publisher release deadline")
            self.statuses.append(status)

        self.runtime._publish = publish
        self.runtime.receive_authorization(ros_time(1000))
        worker = Thread(target=self.runtime.step)
        worker.start()
        try:
            self.assertTrue(entered.wait(2))
            self.clock.now = 110
            self.assertTrue(self.runtime.monitor().tripped)
        finally:
            release.set()
            worker.join(2)
        self.assertFalse(worker.is_alive())
        self.runtime.publish_status()
        self.assertTrue(self.statuses[-2].authorized)
        self.assertTrue(self.statuses[-1].tripped)
        self.assertEqual(self.writes, [])

    def test_no_publication_occurs_after_final_take_before_sdk_entry(self) -> None:
        self.authorize()
        self.runtime.receive_command(message(1001))
        taken = []
        original_take, original_emit = self.runtime.guard.take_latest, self.runtime._emit

        def take() -> object:
            result = original_take()
            taken.append(result)
            return result

        def emit(statuses: object) -> None:
            self.assertEqual(taken, [])
            original_emit(statuses)

        self.runtime.guard.take_latest = take
        self.runtime._emit = emit
        self.assertTrue(self.runtime.step())
        self.assertEqual(self.writes, [(0.1, 0.2)])

    def test_publish_error_closes_without_retry_and_retains_failed_exact_facts(self) -> None:
        self.authorize()
        self.clock.now = 110
        self.runtime.monitor()
        calls = []

        def fail(status: object) -> None:
            calls.append(status)
            raise RuntimeError("unknown delivery")

        self.runtime._publish = fail
        self.runtime.receive_authorization(ros_time(2000))
        with self.assertRaisesRegex(RuntimeError, "unknown delivery"):
            self.runtime.step()
        self.assertFalse(self.runtime.ready)
        self.assertFalse(self.runtime.monitor().authorized)
        self.assertTrue(self.runtime.failed_statuses[0].tripped)
        self.assertEqual(self.runtime.failed_statuses[0].authorization_stamp_ns, 1000)
        self.assertEqual(self.runtime.failed_statuses[1].authorization_stamp_ns, 2000)
        self.runtime.publish_status()
        self.assertFalse(self.runtime.step())
        self.assertEqual(len(calls), 1)
        with self.assertRaises(RuntimeError):
            self.runtime.bind_writer(self.writes.append)

    def test_stage_close_discards_pending_preserves_floors_and_requires_new_time(self) -> None:
        self.authorize()
        self.runtime.receive_command(message(1001))
        self.runtime.receive_authorization(ros_time(2000))
        self.runtime.close()
        self.assertFalse(self.runtime.receive_authorization(ros_time(3000)))
        self.runtime.bind_writer(self.writes.append)
        self.assertFalse(self.runtime.step())
        self.assertEqual(self.runtime.monitor().authorization_stamp_ns, 1000)
        self.runtime.receive_authorization(ros_time(1000))
        self.runtime.step()
        self.assertFalse(self.runtime.monitor().authorized)
        self.authorize(4000)
        self.runtime.receive_command(message(4001))
        self.assertTrue(self.runtime.step())

    def test_close_retains_trip_and_restarted_instance_is_closed(self) -> None:
        self.authorize()
        self.clock.now = 110
        self.runtime.monitor()
        self.runtime.close()
        self.assertTrue(self.runtime.monitor().tripped)
        self.assertEqual(self.runtime.monitor().reason, "command_age_expired")
        restarted = GuardRuntime(config(), self.clock, self.statuses.append)
        restarted.bind_writer(self.writes.append)
        self.assertFalse(restarted.step())
        self.assertIsNone(restarted.monitor().authorization_stamp_ns)


if __name__ == "__main__":
    unittest.main()
