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


class _CapturingInfoLogger(_CapturingLogger):
    def info(self, msg, *args, extra=None):
        self.records.append((msg % args if args else msg, dict(extra or {})))


def test_info_throttles_like_warning():
    throttle = ThrottledLog(min_interval_s=60.0)
    logger = _CapturingInfoLogger()

    assert throttle.info(logger, "req", "GET /x %d", 200, extra={"path": "/x"})
    for _ in range(3):
        assert not throttle.info(logger, "req", "GET /x %d", 200)
    assert len(logger.records) == 1
    assert throttle.suppressed_count("req") == 3

    throttle._last_emit_monotonic["req"] = float("-inf")
    assert throttle.info(logger, "req", "GET /x %d", 200)
    message, extra = logger.records[-1]
    assert "suppressed_since_last=3" in message
    assert extra["suppressed_since_last"] == 3


def test_json_formatter_passes_structured_extras_through():
    """The structured-logging standard requires throttle metadata and
    call-site extras to reach the JSON pipeline, not be dropped by a
    hardcoded field allowlist."""
    import json as json_module

    from manyforge_composer.backend.logging_config import JSONFormatter

    record = logging.LogRecord(
        name="manyforge.test",
        level=logging.INFO,
        pathname=__file__,
        lineno=1,
        msg="request logged",
        args=(),
        exc_info=None,
    )
    record.throttle_key = "request:GET:/api/scene/state"
    record.suppressed_since_last = 42
    record.request_id = "abc123"
    record.duration_ms = 1.5
    # A caller extra colliding with a core output field must be preserved
    # under a prefix, not silently dropped.
    record.event = "manyforge.request"

    entry = json_module.loads(JSONFormatter().format(record))
    assert entry["event"] == "request logged"  # core message field wins
    assert entry["extra_event"] == "manyforge.request"
    assert entry["throttle_key"] == "request:GET:/api/scene/state"
    assert entry["suppressed_since_last"] == 42
    assert entry["request_id"] == "abc123"
    assert entry["duration_ms"] == 1.5
