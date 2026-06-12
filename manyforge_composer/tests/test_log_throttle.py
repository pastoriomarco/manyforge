"""Audit 6F: rate-limited structured hot-path logging."""

from __future__ import annotations

import logging

from manyforge_composer.backend.log_throttle import ThrottledLog


class _CapturingLogger:
    def __init__(self):
        self.records = []

    def warning(self, msg, *args, extra=None):
        self.records.append((msg % args if args else msg, dict(extra or {})))


def test_emits_once_per_window_and_counts_suppressed():
    throttle = ThrottledLog(min_interval_s=60.0)
    logger = _CapturingLogger()

    assert throttle.warning(logger, "k", "slow tick %d", 1, extra={"event": "e"})
    for index in range(5):
        assert not throttle.warning(logger, "k", "slow tick %d", index)
    assert len(logger.records) == 1
    assert throttle.suppressed_count("k") == 5

    # Force the window to elapse; the next emission carries the count.
    throttle._last_emit_monotonic["k"] = float("-inf")
    assert throttle.warning(logger, "k", "slow tick %d", 99)
    assert len(logger.records) == 2
    message, extra = logger.records[-1]
    assert "suppressed_since_last=5" in message
    assert extra["suppressed_since_last"] == 5
    assert extra["throttle_key"] == "k"
    assert throttle.suppressed_count("k") == 0


def test_keys_are_independent():
    throttle = ThrottledLog(min_interval_s=60.0)
    logger = _CapturingLogger()
    assert throttle.warning(logger, "a", "a happened")
    assert throttle.warning(logger, "b", "b happened")
    assert not throttle.warning(logger, "a", "a happened")
    assert len(logger.records) == 2


def test_works_with_stdlib_logger():
    throttle = ThrottledLog(min_interval_s=60.0)
    logger = logging.getLogger("manyforge.test.log_throttle")
    assert throttle.warning(logger, "k", "value=%d", 7, extra={"event": "e"})
