"""Structured JSON console logging shared by the ManyForge Python processes.

The Headless API, the Program runner and Composer each call
:func:`setup_logging` once at start with their role name; every record then
reaches stderr as one JSON object per line with the same keys as the C++
processes' structured lines (``timestamp``, ``mono_ns``, ``level``,
``process``, ``pid``, ``event``).

A log call never writes (OD-382): the root logger's only handler is a
:class:`logging.handlers.QueueHandler` that formats the line in memory and
puts it on an unbounded queue; one :class:`logging.handlers.QueueListener`
thread writes the lines to stderr in queue order. Nothing is dropped. The
console's disk bound stays where stderr goes (Docker's json-file rotation,
or journald for the operator launcher).
"""
from __future__ import annotations

import atexit
from datetime import datetime, timezone
import json
import logging
import logging.handlers
import queue
import sys
import time
from typing import Any


# The --log-level choices of the processes that call setup_logging.
LOG_LEVELS = ("DEBUG", "INFO", "WARNING", "ERROR")


# Attributes every LogRecord carries; anything else on the record was
# attached by the caller via `extra={...}` and must reach the JSON output.
_STANDARD_RECORD_ATTRS = frozenset(
    {
        "name",
        "msg",
        "args",
        "levelname",
        "levelno",
        "pathname",
        "filename",
        "module",
        "exc_info",
        "exc_text",
        "stack_info",
        "lineno",
        "funcName",
        "created",
        "msecs",
        "relativeCreated",
        "thread",
        "threadName",
        "processName",
        "process",
        "taskName",
        "message",
        "asctime",
    }
)


class JSONFormatter(logging.Formatter):
    """Emit log records as single-line JSON objects.

    ``timestamp`` is UTC ISO-8601 with microseconds and a trailing ``Z``;
    ``mono_ns`` is CLOCK_MONOTONIC (``time.monotonic_ns()``) when the line is
    formatted; ``process`` is the role name given at setup and ``pid`` the
    OS process id.

    All caller-attached `extra={...}` fields pass through generically —
    the structured-logging standard is only worth anything if throttle
    keys, suppressed counts, and call-site context actually reach the
    pipeline. An extra whose name collides with a core output field is
    kept under an `extra_` prefix instead of being dropped.
    """

    def __init__(self, process: str | None = None) -> None:
        super().__init__()
        self._process = process

    def format(self, record: logging.LogRecord) -> str:
        entry: dict[str, Any] = {
            "timestamp": datetime.fromtimestamp(record.created, timezone.utc)
            .strftime("%Y-%m-%dT%H:%M:%S.%fZ"),
            "mono_ns": time.monotonic_ns(),
            "level": record.levelname,
            "process": self._process,
            "pid": record.process,
            "logger": record.name,
            "event": record.getMessage(),
        }
        if record.exc_info and record.exc_info[0] is not None:
            entry["exception"] = self.formatException(record.exc_info)
        for key, val in record.__dict__.items():
            if key in _STANDARD_RECORD_ATTRS or key.startswith("_") or val is None:
                continue
            if key in entry:
                entry[f"extra_{key}"] = val
            else:
                entry[key] = val
        return json.dumps(entry, default=str)


# The running writer: (queue handler on the root logger, listener, its
# stderr handler). Set and cleared on the main thread (setup, exit).
_writer: tuple[
    logging.handlers.QueueHandler,
    logging.handlers.QueueListener,
    logging.StreamHandler,
] | None = None
_stop_registered = False


def setup_logging(level: str = "WARNING", *, process: str) -> None:
    """Configure the root logger with the JSON formatter on stderr.

    ``process`` names the role (``headless_api``, ``program_runner``,
    ``composer``). Uvicorn's own logging config later configures only the
    ``uvicorn*`` loggers and leaves this root handler in place.

    The calling thread only formats the line and queues it; the writer
    thread started here writes it. :func:`stop_logging` (also run at exit)
    writes what is still queued.
    """
    global _stop_registered, _writer
    stop_logging()
    # The line is formatted on the calling thread (same mono_ns and content
    # as a direct write); the writer only copies the finished line.
    stream_handler = logging.StreamHandler(sys.stderr)
    stream_handler.setFormatter(logging.Formatter("%(message)s"))
    lines: queue.SimpleQueue[logging.LogRecord] = queue.SimpleQueue()
    queue_handler = logging.handlers.QueueHandler(lines)
    queue_handler.setFormatter(JSONFormatter(process))
    listener = logging.handlers.QueueListener(lines, stream_handler)
    root = logging.getLogger()
    root.setLevel(getattr(logging, level.upper(), logging.INFO))
    # Remove any pre-existing handlers (e.g. uvicorn defaults)
    root.handlers.clear()
    listener.start()
    _writer = (queue_handler, listener, stream_handler)
    root.addHandler(queue_handler)
    if not _stop_registered:
        atexit.register(stop_logging)
        _stop_registered = True
    # Quiet noisy libraries
    logging.getLogger("uvicorn.access").setLevel(logging.WARNING)
    logging.getLogger("httpx").setLevel(logging.WARNING)
    logging.getLogger("httpcore").setLevel(logging.WARNING)


def stop_logging() -> None:
    """Write every queued line, stop the writer thread, write directly after.

    Run at exit and before an immediate process end (``os._exit``). Lines
    logged afterwards go straight to stderr, so nothing is lost. A no-op
    when no writer runs.
    """
    global _writer
    if _writer is None:
        return
    queue_handler, listener, stream_handler = _writer
    _writer = None
    # Writes every line queued before its stop marker.
    listener.stop()
    root = logging.getLogger()
    if queue_handler in root.handlers:
        direct = logging.StreamHandler(stream_handler.stream)
        direct.setFormatter(queue_handler.formatter)
        root.removeHandler(queue_handler)
        root.addHandler(direct)
    # Lines queued between the stop marker and the handler swap (already
    # formatted: written as they are).
    while True:
        try:
            record = listener.queue.get_nowait()
        except queue.Empty:
            break
        stream_handler.handle(record)
    stream_handler.flush()

__all__ = ["LOG_LEVELS", "JSONFormatter", "setup_logging", "stop_logging"]
