"""Structured JSON logging configuration for ManyForge Composer."""
from __future__ import annotations

import json
import logging
import sys
import time
from typing import Any


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

    All caller-attached `extra={...}` fields pass through generically —
    the structured-logging standard is only worth anything if throttle
    keys, suppressed counts, and call-site context actually reach the
    pipeline. An extra whose name collides with a core output field is
    kept under an `extra_` prefix instead of being dropped.
    """

    def format(self, record: logging.LogRecord) -> str:
        entry: dict[str, Any] = {
            "timestamp": self.formatTime(record, self.datefmt),
            "level": record.levelname,
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


def setup_logging(level: str = "INFO") -> None:
    """Configure root logger with JSON formatter on stderr."""
    handler = logging.StreamHandler(sys.stderr)
    handler.setFormatter(JSONFormatter())
    root = logging.getLogger()
    root.setLevel(getattr(logging, level.upper(), logging.INFO))
    # Remove any pre-existing handlers (e.g. uvicorn defaults)
    root.handlers.clear()
    root.addHandler(handler)
    # Quiet noisy libraries
    logging.getLogger("uvicorn.access").setLevel(logging.WARNING)
    logging.getLogger("httpx").setLevel(logging.WARNING)
    logging.getLogger("httpcore").setLevel(logging.WARNING)
