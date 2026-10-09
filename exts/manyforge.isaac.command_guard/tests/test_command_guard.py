from __future__ import annotations

from dataclasses import FrozenInstanceError
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from manyforge.isaac.command_guard.frame import JointCommand, stamp_ns
from manyforge.isaac.command_guard.guard import CommandGuard, GuardConfig


class Clock:
    def __init__(self, now: int = 10) -> None:
        self.now = now

    def __call__(self) -> int:
        return self.now


def command(stamp: int, position: float = 0.2) -> JointCommand:
    return JointCommand(stamp, ("joint",), (position,))


class GuardTest(unittest.TestCase):
    def setUp(self) -> None:
        self.clock = Clock()
        self.guard = CommandGuard(GuardConfig(("joint",), 100), self.clock)

    def arm(self) -> None:
        self.assertTrue(self.guard.authorize(1000))

    def test_startup_closed_and_incoming_frames_never_authorize(self) -> None:
        self.assertFalse(self.guard.offer(command(1001), 10))
        self.assertIsNone(self.guard.take_latest())
        self.assertFalse(self.guard.monitor().authorized)
        self.assertFalse(self.guard.monitor().tripped)

    def test_authorization_accepts_only_strictly_newer_stamps(self) -> None:
        self.arm()
        self.assertFalse(self.guard.authorize(1000))
        self.assertFalse(self.guard.authorize(999))
        self.assertTrue(self.guard.authorize(1002))
        self.assertFalse(self.guard.offer(command(1002), 10))
        self.assertTrue(self.guard.offer(command(1003), 10))

    def test_whole_immutable_frame_and_one_pending_latest_snapshot(self) -> None:
        self.arm()
        first, second = command(1001, 0.1), command(1002, 0.2)
        self.assertTrue(self.guard.offer(first, 10))
        self.assertTrue(self.guard.offer(second, 10))
        self.assertIs(self.guard.take_latest(), second)
        self.assertIsNone(self.guard.take_latest())
        with self.assertRaises(FrozenInstanceError):
            second.positions = (0.0,)

    def test_repeat_and_reordered_commands_do_not_refresh_age(self) -> None:
        self.arm()
        self.assertTrue(self.guard.offer(command(1002), 10))
        self.clock.now = 99
        self.assertFalse(self.guard.offer(command(1002), 99))
        self.assertFalse(self.guard.offer(command(1001), 99))
        self.clock.now = 110
        self.assertTrue(self.guard.monitor().tripped)

    def test_remote_stamp_is_order_only_not_compared_to_local_age(self) -> None:
        self.arm()
        self.assertTrue(self.guard.offer(command(1_791_570_081_270_016_345), 10))
        self.assertFalse(self.guard.monitor().tripped)
        self.clock.now = 110
        self.assertTrue(self.guard.monitor().tripped)

    def test_expiry_is_sticky_until_newer_authorization(self) -> None:
        self.arm()
        self.clock.now = 110
        self.assertFalse(self.guard.offer(command(1001), 110))
        self.assertFalse(self.guard.authorize(1000))
        self.assertTrue(self.guard.monitor().tripped)
        self.assertTrue(self.guard.authorize(2000))
        self.assertFalse(self.guard.offer(command(1999), 110))
        self.assertTrue(self.guard.offer(command(2001), 110))

    def test_reset_discards_old_pending_commands(self) -> None:
        self.arm()
        self.assertTrue(self.guard.offer(command(1001), 10))
        self.assertTrue(self.guard.authorize(2000))
        self.assertIsNone(self.guard.take_latest())
        self.assertFalse(self.guard.offer(command(1001), 10))
        self.assertTrue(self.guard.offer(command(2001), 10))

    def test_reset_does_not_regress_last_accepted_command_stamp(self) -> None:
        self.arm()
        self.assertTrue(self.guard.offer(command(3000), 10))
        self.assertTrue(self.guard.authorize(2000))
        self.assertFalse(self.guard.offer(command(2999), 10))
        self.assertTrue(self.guard.offer(command(3001), 10))

    def test_restarted_instance_starts_closed(self) -> None:
        self.arm()
        self.assertTrue(self.guard.offer(command(1001), 10))
        restarted = CommandGuard(self.guard.config, self.clock)
        self.assertFalse(restarted.offer(command(1002), 10))
        self.assertIsNone(restarted.take_latest())

    def test_queued_frame_cannot_hide_long_simulator_pause(self) -> None:
        self.arm()
        self.assertTrue(self.guard.offer(command(1001), 10))
        self.clock.now = 111
        self.assertIsNone(self.guard.take_latest())
        self.assertTrue(self.guard.monitor().tripped)

    def test_callback_samples_may_complete_out_of_order_without_clock_fault(self) -> None:
        self.arm()
        earlier_receipt = self.clock.now
        self.clock.now = 11
        self.guard.monitor()
        self.assertTrue(self.guard.offer(command(1002), 11))
        self.assertFalse(self.guard.offer(command(1001), earlier_receipt))
        self.assertFalse(self.guard.monitor().tripped)
        self.assertEqual(self.guard.monitor().progress_receipt_ns, 11)

    def test_earlier_callback_receipt_with_newer_stamp_is_stale_not_clock_fault(self) -> None:
        self.arm()
        self.clock.now = 11
        self.assertTrue(self.guard.offer(command(1001), 11))
        self.clock.now = 12
        self.assertFalse(self.guard.offer(command(1002), 10))
        self.assertFalse(self.guard.monitor().tripped)
        self.assertEqual(self.guard.monitor().progress_receipt_ns, 11)

    def test_pre_authorization_decode_finishes_later_without_refresh(self) -> None:
        earlier = 9
        self.arm()
        self.assertFalse(self.guard.offer(command(1001), earlier))
        self.assertFalse(self.guard.monitor().tripped)
        self.assertEqual(self.guard.monitor().progress_receipt_ns, 10)

    def test_actual_local_clock_reversal_closes_guard(self) -> None:
        self.arm()
        self.clock.now = 9
        self.assertTrue(self.guard.monitor().tripped)
        self.assertFalse(self.guard.authorize(2000))
        self.clock.now = 11
        self.assertTrue(self.guard.authorize(2000))

    def test_future_invalid_and_expired_receipts_fail_closed(self) -> None:
        for receipt in (11, -1, True, 10.0):
            with self.subTest(receipt=receipt):
                guard = CommandGuard(self.guard.config, self.clock)
                guard.authorize(1000)
                self.assertFalse(guard.offer(command(1001), receipt))
                self.assertTrue(guard.monitor().tripped)
        self.clock.now = 20
        guard = CommandGuard(self.guard.config, self.clock)
        guard.authorize(1000)
        self.assertFalse(guard.offer(command(1001), 0))
        self.assertFalse(guard.monitor().tripped)  # Older than authorization, no refresh.

    def test_holds_with_new_production_stamps_keep_local_age_alive(self) -> None:
        self.arm()
        for index in range(1, 6):
            self.clock.now += 90
            self.assertTrue(self.guard.offer(command(1000 + index, 0.2), self.clock.now))
            self.assertFalse(self.guard.monitor().tripped)
            self.assertEqual(self.guard.take_latest().positions, (0.2,))

    def test_configured_joint_count_has_no_six_or_sixteen_joint_limit(self) -> None:
        for count in (1, 2, 6, 7, 8, 18):
            names = tuple("joint" + str(index) for index in range(count))
            guard = CommandGuard(GuardConfig(names, 100), self.clock)
            guard.authorize(1000)
            frame = JointCommand(1001, names[::-1], tuple(float(index) for index in range(count)))
            self.assertTrue(guard.offer(frame, 10))
            self.assertIs(guard.take_latest(), frame)

    def test_malformed_joint_arrays_never_replace_pending_frame(self) -> None:
        self.arm()
        valid = command(1001)
        self.guard.offer(valid, 10)
        for bad in (JointCommand(1002, ("joint", "joint"), (0.0, 0.1)),
                    JointCommand(1002, ("unknown",), (0.0,)),
                    JointCommand(1002, ("joint",), (float("nan"),)),
                    JointCommand(1002, ("joint",), (float("inf"),)),
                    JointCommand(1002, ("joint",), (True,)),
                    JointCommand(1002, ("joint",), (), (0.0,)),
                    JointCommand(True, ("joint",), (0.0,))):
            with self.subTest(command=bad), self.assertRaises(ValueError):
                self.guard.offer(bad, 10)
        self.assertIs(self.guard.take_latest(), valid)

    def test_stamp_decode_preserves_integer_nanoseconds(self) -> None:
        self.assertEqual(stamp_ns(SimpleNamespace(sec=1_791_570_081, nanosec=270_016_345)),
                         1_791_570_081_270_016_345)
        self.assertEqual(stamp_ns(SimpleNamespace(sec=-1, nanosec=995_000_000)), -5_000_000)
        for sec, nsec in ((True, 0), (0, True), (0, -1), (0, 1_000_000_000),
                          (2**31, 0), (1.5, 0)):
            with self.subTest(sec=sec, nsec=nsec), self.assertRaises(ValueError):
                stamp_ns(SimpleNamespace(sec=sec, nanosec=nsec))

    def test_standard_message_projection_copies_arrays_before_effect(self) -> None:
        msg = SimpleNamespace(header=SimpleNamespace(stamp=SimpleNamespace(sec=1, nanosec=1)),
                              name=["joint"], position=[0.2], velocity=[], effort=[])
        frame = JointCommand.from_joint_state(msg)
        msg.position[0] = 9.0
        self.assertEqual(frame.positions, (0.2,))

    def test_no_invented_age_default_or_duplicate_configured_joints(self) -> None:
        for names, age in ((("joint",), 0), (("joint",), True), (("joint", "joint"), 100), ((), 100)):
            with self.subTest(names=names, age=age), self.assertRaises(ValueError):
                GuardConfig(names, age)


if __name__ == "__main__":
    unittest.main()
