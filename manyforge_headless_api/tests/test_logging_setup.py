"""The structured console line shared by the ManyForge Python processes."""

from __future__ import annotations

from datetime import datetime, timezone
import io
import json
import logging
import os
import logging.handlers
import re
import sys
import threading
import time

import pytest

from manyforge_headless_api.logging_setup import (
    JSONFormatter,
    setup_logging,
    stop_logging,
)


def _record(msg: str = "Execution event pump recovered", **extra: object) -> logging.LogRecord:
    record = logging.LogRecord(
        name="manyforge_headless_api.execution",
        level=logging.WARNING,
        pathname=__file__,
        lineno=1,
        msg=msg,
        args=(),
        exc_info=None,
    )
    for key, value in extra.items():
        setattr(record, key, value)
    return record


def test_a_line_carries_utc_time_role_and_pid() -> None:
    before = time.monotonic_ns()
    record = _record()
    entry = json.loads(JSONFormatter("headless_api").format(record))

    assert re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{6}Z", entry["timestamp"])
    parsed = datetime.strptime(entry["timestamp"], "%Y-%m-%dT%H:%M:%S.%fZ").replace(
        tzinfo=timezone.utc
    )
    assert abs(parsed.timestamp() - record.created) < 1e-5
    assert before <= entry["mono_ns"] <= time.monotonic_ns()
    assert entry["level"] == "WARNING"
    assert entry["process"] == "headless_api"
    assert entry["pid"] == os.getpid()
    assert entry["logger"] == "manyforge_headless_api.execution"
    assert entry["event"] == "Execution event pump recovered"
    assert list(entry)[:7] == [
        "timestamp", "mono_ns", "level", "process", "pid", "logger", "event"
    ]


def test_extras_pass_through_and_a_colliding_one_is_prefixed() -> None:
    entry = json.loads(
        JSONFormatter("program_runner").format(
            _record(
                program_run_id="run-1",
                suppressed_since_last=3,
                event="manyforge.override",
                pid=7,
                skipped=None,
                _private="hidden",
            )
        )
    )
    assert entry["program_run_id"] == "run-1"
    assert entry["suppressed_since_last"] == 3
    assert entry["event"] == "Execution event pump recovered"
    assert entry["extra_event"] == "manyforge.override"
    assert entry["pid"] == os.getpid()
    assert entry["extra_pid"] == 7
    assert "skipped" not in entry and "_private" not in entry


def test_an_exception_is_one_field_of_the_line() -> None:
    try:
        raise RuntimeError("journal closed")
    except RuntimeError:
        record = _record()
        record.exc_info = sys.exc_info()
    line = JSONFormatter("composer").format(record)
    assert "\n" not in line
    entry = json.loads(line)
    assert "RuntimeError: journal closed" in entry["exception"]
    assert entry["exception"].startswith("Traceback")


@pytest.fixture
def root_logging():
    """Restore every logger the setup or uvicorn's config touches."""

    loggers = [
        logging.getLogger(name)
        for name in ("", "uvicorn", "uvicorn.error", "uvicorn.access", "httpx", "httpcore")
    ]
    saved = [(item, item.level, list(item.handlers), item.propagate) for item in loggers]
    yield
    stop_logging()
    for item, level, handlers, propagate in saved:
        item.setLevel(level)
        item.handlers[:] = handlers
        item.propagate = propagate


def _stderr(monkeypatch: pytest.MonkeyPatch) -> io.StringIO:
    # Inside the test: pytest's capture resets sys.stderr between phases.
    stream = io.StringIO()
    monkeypatch.setattr(sys, "stderr", stream)
    return stream


@pytest.mark.usefixtures("root_logging")
def test_setup_replaces_the_root_handler_at_the_given_level(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    stderr = _stderr(monkeypatch)
    setup_logging("WARNING", process="program_runner")
    root = logging.getLogger()
    assert len(root.handlers) == 1
    assert root.level == logging.WARNING
    assert logging.getLogger("httpx").level == logging.WARNING

    logger = logging.getLogger("manyforge_program_runner.diagnostics")
    logger.info("Program forensic delivery recovered")
    logger.warning("Program forensic delivery failed", extra={"program_run_id": "run-1"})
    stop_logging()

    lines = stderr.getvalue().splitlines()
    assert len(lines) == 1
    entry = json.loads(lines[0])
    assert entry["process"] == "program_runner"
    assert entry["event"] == "Program forensic delivery failed"
    assert entry["program_run_id"] == "run-1"


@pytest.mark.usefixtures("root_logging")
def test_setup_keeps_its_handler_through_uvicorns_logging_config(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    import uvicorn

    stderr = _stderr(monkeypatch)
    setup_logging("INFO", process="headless_api")
    uvicorn.Config(object(), access_log=False).configure_logging()
    logging.getLogger("manyforge_headless_api.control_state").info(
        "Control incident recorder recovered"
    )
    stop_logging()
    entry = json.loads(stderr.getvalue().splitlines()[-1])
    assert entry["process"] == "headless_api"
    assert entry["event"] == "Control incident recorder recovered"
    assert logging.getLogger("uvicorn.access").handlers == []


class _HeldStream(io.StringIO):
    """stderr whose writes wait until released, noting the writing thread."""

    def __init__(self) -> None:
        super().__init__()
        self.entered = threading.Event()
        self.release = threading.Event()
        self.writers: set[int] = set()

    def write(self, text: str) -> int:
        self.writers.add(threading.get_ident())
        self.entered.set()
        assert self.release.wait(30), "the test never released the writer"
        return super().write(text)


@pytest.mark.usefixtures("root_logging")
def test_a_log_call_only_queues_and_a_stuck_writer_does_not_hold_the_caller(
    monkeypatch: pytest.MonkeyPatch, request: pytest.FixtureRequest
) -> None:
    stream = _HeldStream()
    # A failed assertion below must not leave the writer held at teardown.
    request.addfinalizer(stream.release.set)
    monkeypatch.setattr(sys, "stderr", stream)
    setup_logging("INFO", process="program_runner")
    root = logging.getLogger()
    assert [type(handler) for handler in root.handlers] == [
        logging.handlers.QueueHandler
    ]

    logger = logging.getLogger("manyforge_program_runner.runtime")
    before = time.monotonic_ns()
    try:
        raise RuntimeError("journal closed")
    except RuntimeError:
        logger.exception("first", extra={"program_run_id": "run-1"})
    after_first = time.monotonic_ns()
    # The writer thread holds the first line in its write; the caller goes on.
    assert stream.entered.wait(30)
    for index in range(100):
        logger.info("line %d", index)
    assert not stream.release.is_set()
    assert threading.get_ident() not in stream.writers

    stream.release.set()
    stop_logging()
    assert threading.get_ident() not in stream.writers
    entries = [json.loads(line) for line in stream.getvalue().splitlines()]
    assert [entry["event"] for entry in entries] == [
        "first", *(f"line {index}" for index in range(100))
    ]
    # Formatted at the call: the caller's time, fields and traceback.
    assert before <= entries[0]["mono_ns"] <= after_first
    assert entries[0]["program_run_id"] == "run-1"
    assert "RuntimeError: journal closed" in entries[0]["exception"]
    assert entries[0]["process"] == "program_runner"


@pytest.mark.usefixtures("root_logging")
def test_after_stop_lines_are_written_directly_in_the_same_format(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    stderr = _stderr(monkeypatch)
    setup_logging("INFO", process="headless_api")
    logger = logging.getLogger("manyforge_headless_api.execution")
    logger.info("queued")
    stop_logging()
    stop_logging()  # a second stop is a no-op
    logger.info("direct", extra={"cell_id": "cell-a"})

    entries = [json.loads(line) for line in stderr.getvalue().splitlines()]
    assert [entry["event"] for entry in entries] == ["queued", "direct"]
    assert entries[1]["process"] == "headless_api"
    assert entries[1]["cell_id"] == "cell-a"
    assert [type(handler) for handler in logging.getLogger().handlers] == [
        logging.StreamHandler
    ]


@pytest.mark.usefixtures("root_logging")
def test_a_repeated_setup_writes_the_earlier_writers_lines_first(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    stderr = _stderr(monkeypatch)
    setup_logging("INFO", process="composer")
    logging.getLogger("manyforge_composer").info("before")
    setup_logging("INFO", process="composer")
    logging.getLogger("manyforge_composer").info("after")
    stop_logging()

    events = [json.loads(line)["event"] for line in stderr.getvalue().splitlines()]
    assert events == ["before", "after"]
    assert len(logging.getLogger().handlers) == 1
