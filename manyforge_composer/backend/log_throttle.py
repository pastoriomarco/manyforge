"""Rate-limited structured logging for hot paths (audit Phase 6F).

Hot-path diagnostics must be structured and bounded: a sustained condition
(slow publisher tick, repeatedly failing worker) emits one record per
throttle window plus a suppressed-occurrence count, never one record per
occurrence. Keys partition independent conditions so one noisy condition
cannot mask another.

Usage:

    _throttle = ThrottledLog(min_interval_s=5.0)
    ...
    _throttle.warning(
        logger,
        "visual.snapshot_build_slow",
        "visual-state publisher slow: duration_ms=%.1f",
        duration_ms,
        extra={"event": "manyforge.visual.snapshot_build_slow"},
    )
"""

from __future__ import annotations

import threading
import time
from typing import Any, Optional


class ThrottledLog:
    """Per-key rate limiter for structured log records.

    Thread-safe; emission decisions and suppressed-count bookkeeping happen
    under a small internal lock, the actual logging call outside any
    caller-visible critical section concerns (callers should not hold hot
    locks while logging regardless).
    """

    def __init__(self, min_interval_s: float = 5.0) -> None:
        self._min_interval_s = float(min_interval_s)
        self._lock = threading.Lock()
        self._last_emit_monotonic: dict[str, float] = {}
        self._suppressed: dict[str, int] = {}

    def warning(
        self,
        logger,
        key: str,
        msg: str,
        *args: Any,
        extra: Optional[dict[str, Any]] = None,
    ) -> bool:
        """Emit at most one warning per key per window.

        Returns True when a record was emitted. The emitted record carries
        `suppressed_since_last` (occurrences swallowed since the previous
        emission for this key) so operators can read the true event rate
        from a bounded stream.
        """
        now = time.monotonic()
        with self._lock:
            last = self._last_emit_monotonic.get(key, float("-inf"))
            if now - last < self._min_interval_s:
                self._suppressed[key] = self._suppressed.get(key, 0) + 1
                return False
            suppressed = self._suppressed.pop(key, 0)
            self._last_emit_monotonic[key] = now
        payload = dict(extra or {})
        payload["throttle_key"] = key
        payload["suppressed_since_last"] = suppressed
        logger.warning(
            msg + " (suppressed_since_last=%d)",
            *args,
            suppressed,
            extra=payload,
        )
        return True

    def suppressed_count(self, key: str) -> int:
        """Currently swallowed occurrences for a key (test/health surface)."""
        with self._lock:
            return self._suppressed.get(key, 0)
