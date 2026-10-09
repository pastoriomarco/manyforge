"""Pure schema/decision tests. No simulator, ROS, wall-clock sleeps or effects."""
from __future__ import annotations

from dataclasses import FrozenInstanceError, replace
import json
from pathlib import Path
import sys
import threading
import tomllib
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from manyforge.isaac.command_guard.frame import (  # noqa: E402
    Binding, BoundaryAddress, CommandFrame, GoalKey, GuardConfig, UINT64_MAX, decode_frame, encode_frame,
)
from manyforge.isaac.command_guard import guard as guard_module  # noqa: E402
from manyforge.isaac.command_guard.guard import (  # noqa: E402
    BoundaryOutcome, FenceBoundaryOutcome, CommandGuard, OwnedOperation,
)


def address() -> BoundaryAddress:
    return BoundaryAddress("binding", "route", "configuration", "provider-instance",
                           "provider-unit", "command-target")


def config(**changes) -> GuardConfig:
    values = dict(owner_epoch="owner", occurrence="receiver", binding=Binding(1, "provider"),
                  joint_names=("j1", "j2"), address=address(), max_encoded_bytes=4096,
                  max_identity_bytes=64, max_command_age_ns=100)
    return GuardConfig(**(values | changes))


def key(number=1) -> GoalKey:
    return GoalKey("operation", f"goal-{number}", number, 42, Binding(1, "provider"))


def frame(number=1, sequence=1, **changes) -> CommandFrame:
    value = CommandFrame("owner", "receiver", key(number), ("j1", "j2"), (0.1, 0.2),
                         sequence, number, 0, 0, False, address())
    return replace(value, **changes)


class CodecTest(unittest.TestCase):
    def test_canonical_complete_tuple_and_full_width_counters(self):
        value = frame(sequence=UINT64_MAX, consumed_scale_generation=(1 << 53) + 1,
                      consumed_stop_generation=UINT64_MAX, final_command=True)
        encoded = encode_frame(value, config())
        self.assertEqual(decode_frame(encoded, config()), value)
        self.assertEqual(encode_frame(decode_frame(encoded, config()), config()), encoded)
        self.assertIs(type(json.loads(encoded)["producer_sequence"]), int)
        with self.assertRaises(FrozenInstanceError):
            value.producer_sequence = 1

    def test_full_key_and_binding_max_counters(self):
        binding = Binding(UINT64_MAX, "provider")
        cfg = config(binding=binding)
        full = replace(key(), binding=binding, controller_goal_generation=UINT64_MAX,
                       prepared_execution_id=UINT64_MAX)
        value = replace(frame(), key=full, consumed_goal_generation=UINT64_MAX)
        self.assertEqual(decode_frame(encode_frame(value, cfg), cfg), value)

    def test_counter_types_ranges_and_lossy_values_refused(self):
        original = json.loads(encode_frame(frame(), config()))
        for field in ("producer_sequence", "consumed_goal_generation",
                      "consumed_scale_generation", "consumed_stop_generation"):
            for bad in (True, False, -1, 1.0, "1", UINT64_MAX + 1, None):
                with self.subTest(field=field, value=bad), self.assertRaises(ValueError):
                    raw = dict(original, **{field: bad})
                    decode_frame(json.dumps(raw), config())
        for field in ("producer_sequence", "consumed_goal_generation"):
            with self.subTest(field=field), self.assertRaises(ValueError):
                decode_frame(json.dumps(dict(original, **{field: 0})), config())

    def test_full_key_required_counter_fields_refuse_unsafe_values(self):
        for field in ("controller_goal_generation", "prepared_execution_id",
                      "active_control_configuration_generation"):
            for bad in (True, 0, -1, 1.0, UINT64_MAX + 1):
                raw = json.loads(encode_frame(frame(), config()))
                target = (raw["key"]["binding"] if field == "active_control_configuration_generation"
                          else raw["key"])
                target[field] = bad
                with self.subTest(field=field, bad=bad), self.assertRaises(ValueError):
                    decode_frame(json.dumps(raw), config())

    def test_unknown_missing_duplicate_fields_and_version_refused(self):
        encoded = encode_frame(frame(), config())
        raw = json.loads(encoded)
        variants = [dict(raw, extra=1), dict(raw, schema="future"),
                    {k: v for k, v in raw.items() if k != "final_command"}]
        for value in variants:
            with self.subTest(value=value), self.assertRaises(ValueError):
                decode_frame(json.dumps(value), config())
        for duplicate in ('"producer_sequence":1,', '"operation_id":"operation",'):
            anchor = duplicate.split(":", 1)[0] + ":"
            with self.subTest(duplicate=duplicate), self.assertRaises(ValueError):
                decode_frame(encoded.replace(anchor, duplicate + anchor, 1), config())
        for field in ("key", "binding"):
            changed = json.loads(encoded)
            target = changed["key"] if field == "key" else changed["key"]["binding"]
            target["extra"] = "unreviewed"
            with self.assertRaises(ValueError):
                decode_frame(json.dumps(changed), config())

    def test_nonfinite_unsafe_positions_joint_order_and_marker_refused(self):
        raw = json.loads(encode_frame(frame(), config()))
        for bad in (float("nan"), float("inf"), -float("inf"), True, None, "0.1", (1 << 53) + 1):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                decode_frame(json.dumps(dict(raw, positions_rad=[bad, 0.2])), config())
        for changed in ({"joint_names": ["j2", "j1"]}, {"joint_names": ["j1", "j1"]},
                        {"positions_rad": [0.1]}, {"final_command": 1},
                        {"consumed_goal_generation": 2}):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                decode_frame(json.dumps(raw | changed), config())

    def test_config_and_encoded_identity_bounds_required(self):
        for changed in ({"joint_names": ()}, {"joint_names": ("j1", "j1")},
                        {"joint_names": tuple(f"j{i}" for i in range(17))},
                        {"max_command_age_ns": 0}, {"max_command_age_ns": True},
                        {"max_identity_bytes": 1}, {"max_encoded_bytes": 0}):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                config(**changed)
        with self.assertRaises(ValueError):
            encode_frame(frame(), config(max_encoded_bytes=50))
        for payload in (" " * 4097, "{", "[]", '{"a":NaN}', "[" * 2000):
            with self.subTest(payload=payload[:10]), self.assertRaises(ValueError):
                decode_frame(payload, config())
        large = replace(frame(), key=replace(key(), operation_id="é" * 33))
        with self.assertRaises(ValueError):
            encode_frame(large, config())

    def test_foreign_scope_refused(self):
        for value in (replace(frame(), owner_epoch="other"), replace(frame(), occurrence="old"),
                      replace(frame(), key=replace(key(), binding=Binding(2, "provider"))),
                      replace(frame(), key=replace(key(), binding=Binding(1, "old"))),
                      replace(frame(), key=replace(key(), operation_id=""))):
            with self.subTest(value=value), self.assertRaises(ValueError):
                decode_frame(json.dumps({"schema": "manyforge.isaac.command_frame.v1",
                                       **__import__("dataclasses").asdict(value)}), config())

    def test_each_exact_material_boundary_address_field_is_fenced(self):
        for field in address().__dict__:
            changed = replace(address(), **{field: "different"})
            with self.subTest(field=field), self.assertRaises(ValueError):
                decode_frame(encode_frame(replace(frame(), address=changed),
                                          config(address=changed)), config())
        raw = json.loads(encode_frame(frame(), config()))
        raw["address"]["unknown"] = 1
        with self.assertRaises(ValueError):
            decode_frame(json.dumps(raw), config())


class ManualClock:
    def __init__(self):
        self.value = 0

    def __call__(self):
        return self.value

    def at(self, value, method, *args):
        self.value = value
        return method(*args)


class GuardTest(unittest.TestCase):
    def setUp(self):
        self.clock = ManualClock()
        self.guard = CommandGuard(config(), self.clock)

    def start(self, number=1, now=0, operation=OwnedOperation.EXECUTING):
        self.assertTrue(self.guard.admit(key(number)))
        self.assertTrue(self.clock.at(now, self.guard.arm, key(number), operation))

    def test_startup_idle_and_unadmitted_frames_never_arm(self):
        self.assertFalse(self.clock.at(1, self.guard.offer, frame(), 1))
        self.assertIsNone(self.clock.at(1000, self.guard.begin_effect))
        self.assertFalse(self.guard.status().closed)
        self.assertTrue(self.guard.admit(key()))
        self.assertFalse(self.clock.at(1001, self.guard.offer, frame(), 1001))
        self.assertIsNone(self.guard.status().owned_key)
        with self.assertRaises(ValueError):
            self.clock.at(1001, self.guard.arm, key(), "executing")

    def test_all_owned_modes_arm_including_zero_scale_holding(self):
        for operation in OwnedOperation:
            with self.subTest(operation=operation):
                guard = CommandGuard(config(), self.clock)
                guard.admit(key())
                self.assertTrue(self.clock.at(0, guard.arm, key(), operation))
                self.assertEqual(self.clock.at(100, guard.monitor).trip_reason, "command_age_expired")

    def test_snapshot_coalescing_and_token_correlate_exact_frame(self):
        self.start()
        self.assertTrue(self.clock.at(1, self.guard.offer, frame(sequence=1), 1))
        latest = frame(sequence=2, consumed_scale_generation=UINT64_MAX,
                       consumed_stop_generation=3, final_command=True)
        self.assertTrue(self.clock.at(2, self.guard.offer, latest, 2))
        token = self.clock.at(2, self.guard.begin_effect)
        self.assertEqual(token.frame, latest)
        self.assertIsNone(self.clock.at(2, self.guard.begin_effect))
        fact = self.guard.finish_effect(token, BoundaryOutcome.RETURNED)
        self.assertIs(fact.token, token)
        self.assertIsNone(self.clock.at(3, self.guard.begin_effect))
        with self.assertRaises(ValueError):
            self.guard.finish_effect(token, BoundaryOutcome.RETURNED)

    def test_malformed_snapshot_has_no_effect_or_progress_refresh(self):
        self.start()
        accepted = frame()
        self.clock.at(1, self.guard.offer, accepted, 1)
        with self.assertRaises(ValueError):
            self.clock.at(99, self.guard.offer, replace(frame(sequence=2), final_command=1), 99)
        self.assertEqual(self.clock.at(99, self.guard.begin_effect).frame, accepted)
        self.assertEqual(self.clock.at(101, self.guard.monitor).trip_reason, "command_age_expired")

    def test_republication_and_repeated_arm_do_not_refresh_actual_progress(self):
        self.start()
        self.clock.at(1, self.guard.offer, frame(sequence=2), 1)
        self.assertFalse(self.clock.at(50, self.guard.offer, frame(sequence=2), 50))
        self.assertFalse(self.clock.at(60, self.guard.offer, frame(sequence=1), 60))
        self.assertTrue(self.clock.at(99, self.guard.arm, key(), OwnedOperation.HOLDING))
        self.assertEqual(self.clock.at(101, self.guard.monitor).trip_reason, "command_age_expired")

    def test_long_gap_trips_before_new_sequence_or_resumed_effect(self):
        for attempt in ("offer", "begin"):
            with self.subTest(attempt=attempt):
                guard = CommandGuard(config(), self.clock)
                guard.admit(key())
                self.clock.at(0, guard.arm, key(), OwnedOperation.BRAKING)
                self.clock.at(1, guard.offer, frame(), 1)
                if attempt == "offer":
                    self.assertFalse(self.clock.at(101, guard.offer, frame(sequence=2), 101))
                else:
                    self.assertIsNone(self.clock.at(101, guard.begin_effect))
                self.assertTrue(guard.status().closed)
                self.assertIsNone(self.clock.at(102, guard.begin_effect))
                self.assertFalse(self.clock.at(102, guard.arm, key(), OwnedOperation.EXECUTING))

    def test_invalid_monotonic_domain_or_receipt_never_refreshes(self):
        for receipt, now in ((11, 10), (-1, 10), (True, 10), (10, 9)):
            with self.subTest(receipt=receipt, now=now):
                guard = CommandGuard(config(), self.clock)
                guard.admit(key())
                self.clock.at(10, guard.arm, key(), OwnedOperation.EXECUTING)
                self.assertFalse(self.clock.at(now, guard.offer, frame(), receipt))
                self.assertTrue(guard.status().closed)
        self.start()
        self.assertEqual(self.clock.at(-1, self.guard.monitor).trip_reason, "local_clock_regression")

    def test_callback_sample_and_lock_acquisition_inversion_is_not_clock_regression(self):
        self.start()
        entered_validation = threading.Event()
        release_validation = threading.Event()
        original_validate = guard_module.validate_frame
        results = []

        def delayed_validate(value, cfg):
            original_validate(value, cfg)
            entered_validation.set()
            if not release_validation.wait(timeout=2):
                raise AssertionError("test validation gate not released")

        self.clock.value = 10
        callback_entry_receipt = self.clock()
        with mock.patch.object(guard_module, "validate_frame", delayed_validate):
            worker = threading.Thread(target=lambda: results.append(
                self.guard.offer(frame(), callback_entry_receipt)))
            worker.start()
            try:
                self.assertTrue(entered_validation.wait(timeout=2))
                self.assertFalse(self.clock.at(11, self.guard.monitor).closed)
            finally:
                release_validation.set()
                worker.join(timeout=2)
            self.assertFalse(worker.is_alive())
        self.assertEqual(results, [True])
        self.assertFalse(self.guard.status().closed)
        # Receipt remains 10; serialized observation at 11 does not renew age.
        self.assertEqual(self.clock.at(110, self.guard.monitor).trip_reason, "command_age_expired")

    def test_reversed_decode_completion_refuses_stale_sequence_without_trip(self):
        self.start()
        first_entry_receipt = 10
        self.assertTrue(self.clock.at(11, self.guard.offer, frame(sequence=2), 11))
        self.assertFalse(self.clock.at(12, self.guard.offer, frame(sequence=1), first_entry_receipt))
        self.assertFalse(self.guard.status().closed)
        token = self.clock.at(12, self.guard.begin_effect)
        self.assertEqual(token.frame.producer_sequence, 2)
        self.guard.finish_effect(token, BoundaryOutcome.RETURNED)
        self.assertEqual(self.clock.at(111, self.guard.monitor).trip_reason, "command_age_expired")

    def test_pre_arm_callback_completing_later_is_stale_without_trip(self):
        callback_entry_receipt = 1
        self.start(now=10)
        self.assertFalse(self.clock.at(11, self.guard.offer, frame(), callback_entry_receipt))
        self.assertFalse(self.guard.status().closed)
        self.assertIsNone(self.clock.at(11, self.guard.begin_effect))
        self.assertTrue(self.clock.at(12, self.guard.offer, frame(sequence=2), 12))
        self.assertEqual(self.clock.at(12, self.guard.begin_effect).frame.producer_sequence, 2)

    def test_actual_serialized_underlying_clock_reversal_still_trips(self):
        self.start()
        self.assertFalse(self.clock.at(11, self.guard.monitor).closed)
        self.assertEqual(self.clock.at(10, self.guard.monitor).trip_reason, "local_clock_regression")
        self.assertFalse(self.clock.at(12, self.guard.offer, frame(), 12))

    def test_callback_entry_age_checked_even_while_operation_progress_current(self):
        self.start()
        self.clock.at(1, self.guard.offer, frame(), 1)
        self.assertFalse(self.clock.at(2, self.guard.offer, frame(sequence=2), 0))
        self.assertFalse(self.guard.status().closed)
        self.assertEqual(self.clock.at(101, self.guard.monitor).trip_reason, "command_age_expired")

    def test_fence_overtakes_unknown_admission_and_pending_frame(self):
        self.assertTrue(self.guard.request_fence(key(), 1))
        self.assertFalse(self.guard.admit(key()))
        self.assertFalse(self.clock.at(0, self.guard.arm, key(), OwnedOperation.EXECUTING))
        self.assertFalse(self.clock.at(0, self.guard.offer, frame(), 0))
        self.assertTrue(self.guard.request_fence(key(), 1))
        self.assertFalse(self.guard.request_fence(key(), 2))
        other = CommandGuard(config(), self.clock)
        other.admit(key())
        self.clock.at(0, other.arm, key(), OwnedOperation.EXECUTING)
        self.clock.at(1, other.offer, frame(), 1)
        self.assertTrue(other.request_fence(key(), 1))
        self.assertIsNone(self.clock.at(1, other.begin_effect))

    def test_stalled_effect_never_holds_monitor_or_fence_lock(self):
        self.start()
        self.clock.at(1, self.guard.offer, frame(), 1)
        token = self.clock.at(1, self.guard.begin_effect)
        # The SDK would be running outside the guard now. Other threads remain free.
        observed = []
        def request():
            observed.append(self.guard.request_fence(key(), 1))
            observed.append(self.clock.at(101, self.guard.monitor).trip_reason)
        worker = threading.Thread(target=request)
        worker.start()
        worker.join(timeout=2)
        self.assertFalse(worker.is_alive())
        self.assertEqual(observed, [True, "command_age_expired"])
        self.assertIsNone(self.clock.at(101, self.guard.begin_fence_effect, key()))
        self.guard.finish_effect(token, BoundaryOutcome.RETURNED)
        self.assertIsNotNone(self.clock.at(101, self.guard.begin_fence_effect, key()))
        self.assertTrue(self.guard.status().closed)

    def test_exact_correlated_boundary_fence_fact_required_before_owner_release(self):
        self.start()
        self.clock.at(1, self.guard.offer, frame(), 1)
        token = self.clock.at(1, self.guard.begin_effect)
        self.guard.request_fence(key(), 7)
        self.assertFalse(self.guard.release_after_retirement(key(), token))
        self.guard.finish_effect(token, BoundaryOutcome.RETURNED)
        fence = self.clock.at(2, self.guard.begin_fence_effect, key())
        self.assertEqual(fence.fence_sequence, 7)
        self.assertIsNone(fence.frame)
        self.assertFalse(self.guard.release_after_retirement(key(), fence))
        self.guard.finish_fence_effect(fence, FenceBoundaryOutcome.FENCED)
        self.assertFalse(self.guard.release_after_retirement(key(), replace(fence)))
        self.assertTrue(self.guard.release_after_retirement(key(), fence))
        self.assertFalse(self.clock.at(3, self.guard.offer, frame(sequence=2), 3))
        self.assertEqual(self.guard.status().retained_keys, 0)

    def test_fence_fact_remains_observable_after_another_key_effect(self):
        self.guard.request_fence(key(), 1)
        fence = self.clock.at(0, self.guard.begin_fence_effect, key())
        original = self.guard.finish_fence_effect(fence, FenceBoundaryOutcome.FENCED)
        self.start(number=2, now=1)
        self.clock.at(1, self.guard.offer, frame(number=2), 1)
        normal = self.clock.at(1, self.guard.begin_effect)
        self.guard.finish_effect(normal, BoundaryOutcome.RETURNED)
        self.assertIsNot(self.guard.status().returned_boundary, original)
        self.assertIs(self.guard.read_fence_fact(key()), original)
        self.assertIsNone(self.guard.read_fence_fact(key(3)))
        self.assertTrue(self.guard.release_after_retirement(key(), original.token))
        self.assertIsNone(self.guard.read_fence_fact(key()))

    def test_boundary_failed_unknown_and_foreign_tokens_never_prove_fence(self):
        for result in (FenceBoundaryOutcome.RETURNED, FenceBoundaryOutcome.FAILED,
                       FenceBoundaryOutcome.UNKNOWN):
            with self.subTest(result=result):
                guard = CommandGuard(config(), self.clock)
                guard.request_fence(key(), 1)
                token = self.clock.at(0, guard.begin_fence_effect, key())
                guard.finish_fence_effect(token, result)
                self.assertFalse(guard.release_after_retirement(key(), token))
                self.assertIsNone(self.clock.at(1, guard.begin_fence_effect, key()))
        self.start()
        self.clock.at(1, self.guard.offer, frame(), 1)
        token = self.clock.at(1, self.guard.begin_effect)
        with self.assertRaises(ValueError):
            self.guard.finish_effect(replace(token), BoundaryOutcome.RETURNED)
        with self.assertRaises(ValueError):
            self.guard.finish_effect(token, "applied")
        self.guard.finish_effect(token, BoundaryOutcome.UNKNOWN)
        self.assertTrue(self.guard.status().closed)

    def test_frame_and_fence_finish_methods_refuse_cross_kind_tokens(self):
        self.start()
        self.clock.at(1, self.guard.offer, frame(), 1)
        ordinary = self.clock.at(1, self.guard.begin_effect)
        with self.assertRaises(ValueError):
            self.guard.finish_fence_effect(ordinary, FenceBoundaryOutcome.FENCED)
        self.guard.finish_effect(ordinary, BoundaryOutcome.RETURNED)
        self.guard.request_fence(key(), 1)
        fence = self.clock.at(2, self.guard.begin_fence_effect, key())
        with self.assertRaises(ValueError):
            self.guard.finish_effect(fence, BoundaryOutcome.RETURNED)
        self.assertIsNone(self.clock.at(2, self.guard.begin_effect))
        self.guard.finish_fence_effect(fence, FenceBoundaryOutcome.RETURNED)
        self.assertFalse(self.guard.release_after_retirement(key(), fence))

    def test_old_goal_sequence_cannot_act_after_new_goal_selection(self):
        self.start()
        self.clock.at(1, self.guard.offer, frame(sequence=100), 1)
        old = self.clock.at(1, self.guard.begin_effect)
        self.guard.finish_effect(old, BoundaryOutcome.RETURNED)
        self.start(number=2, now=2)
        self.assertFalse(self.clock.at(3, self.guard.offer, frame(sequence=101), 3))
        self.assertTrue(self.clock.at(3, self.guard.offer, frame(number=2, sequence=1), 3))
        new = self.clock.at(3, self.guard.begin_effect)
        self.assertEqual(new.key, key(2))
        self.guard.finish_effect(new, BoundaryOutcome.RETURNED)
        self.assertFalse(self.clock.at(4, self.guard.offer, frame(sequence=102), 4))
        self.assertIsNone(self.clock.at(4, self.guard.begin_effect))

    def test_four_outstanding_records_and_fence_exhaustion_close_occurrence(self):
        for number in range(1, 5):
            self.assertTrue(self.guard.admit(key(number)))
        self.assertFalse(self.guard.request_fence(key(5), 1))
        self.assertEqual(self.guard.status().trip_reason, "key_capacity_exhausted")
        self.assertFalse(self.guard.admit(key(5)))
        self.assertFalse(self.clock.at(0, self.guard.arm, key(), OwnedOperation.EXECUTING))

    def test_owner_observed_retirement_reuses_budget_without_rearming_old_frames(self):
        for number in range(1, 10):
            self.assertTrue(self.guard.admit(key(number)))
            self.assertTrue(self.clock.at(number, self.guard.arm, key(number), OwnedOperation.HOLDING))
            self.assertTrue(self.guard.request_fence(key(number), number))
            token = self.clock.at(number, self.guard.begin_fence_effect, key(number))
            self.guard.finish_fence_effect(token, FenceBoundaryOutcome.DRAINED)
            # Fake explicit boundary contract supplies the existing port's actual fact.
            self.assertTrue(self.guard.release_after_retirement(key(number), token))
        self.assertFalse(self.clock.at(10, self.guard.offer, frame(sequence=UINT64_MAX), 10))
        self.assertEqual(self.guard.status().retained_keys, 0)

    def test_close_is_sticky_even_after_owner_release(self):
        self.start()
        self.guard.close_occurrence()
        self.guard.request_fence(key(), 1)
        token = self.clock.at(1, self.guard.begin_fence_effect, key())
        self.guard.finish_fence_effect(token, FenceBoundaryOutcome.FENCED)
        self.assertTrue(self.guard.release_after_retirement(key(), token))
        self.assertFalse(self.guard.admit(key(2)))
        self.assertTrue(self.guard.status().closed)

    def test_packaged_foundation_import_has_no_startup_component(self):
        root = Path(__file__).resolve().parents[1]
        packaging = tomllib.loads((root / "config/extension.toml").read_text())
        self.assertFalse(packaging["settings"]["exts"]["manyforge.isaac.command_guard"]["enabled"])
        self.assertFalse((root / "manyforge/isaac/command_guard/extension.py").exists())


if __name__ == "__main__":
    unittest.main()
