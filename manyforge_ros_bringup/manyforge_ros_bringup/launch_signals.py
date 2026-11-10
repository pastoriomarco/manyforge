"""A repeated SIGINT must not abort ``ros2 launch``'s own shutdown.

LaunchService handles SIGINT through ``signal.set_wakeup_fd`` and an asyncio
callback: the first SIGINT starts its shutdown, later ones are logged and
ignored. Python's default handler additionally raises KeyboardInterrupt in the
main thread. A SIGINT that arrives while LaunchService is busy (for example a
second parent-death SIGINT, which the kernel can send again when the launch is
reparented while a multi-threaded host dies) then aborts whichever asyncio task
is running, so the managed nodes may never be interrupted and the launch never
exits.

The handler installed here is a callable that does nothing: the C-level signal
handler still writes the wakeup fd, so LaunchService sees every SIGINT, but no
KeyboardInterrupt is raised. ``SIG_IGN`` would also stop the wakeup write and
must not be used.

Scope: installed only while a LaunchService is running in this process (from
the launch file's OpaqueFunction, never from ``generate_launch_description``,
which ``ros2 launch --print``/``--show-args`` load without one), only from the
main thread, and only over Python's default handler. It stays installed until
the process exits; under ``ros2 launch`` the process exits right after
LaunchService returns. Embedding these launch files in a longer-lived Python
process that keeps running after its LaunchService returns is not covered.
"""

from __future__ import annotations

import signal
import threading


def _launch_service_sigint(_signum: int, _frame: object) -> None:
    """Leave SIGINT to LaunchService's wakeup-fd handler; raise nothing."""


def keep_sigint_out_of_tasks(context: object) -> bool:
    """Install the no-op SIGINT handler for a running LaunchService.

    Returns True only when this call installed it.
    """
    if getattr(context, "asyncio_loop", None) is None:
        return False  # No LaunchService is running (description inspection, tests).
    if threading.current_thread() is not threading.main_thread():
        return False
    if signal.getsignal(signal.SIGINT) is not signal.default_int_handler:
        return False  # Never replace a handler someone else chose.
    signal.signal(signal.SIGINT, _launch_service_sigint)
    return True
