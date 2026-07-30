from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import time


def child_pids(pid: int) -> list[int]:
    children: set[int] = set()
    try:
        tasks = pathlib.Path(f"/proc/{pid}/task").iterdir()
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return []
    for task in tasks:
        try:
            raw = (task / "children").read_text(encoding="ascii").strip()
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
        if raw:
            children.update(int(value) for value in raw.split())
    return sorted(children)


def process_tree(pid: int) -> list[int]:
    pending = [pid]
    found: list[int] = []
    while pending:
        current = pending.pop()
        if current in found:
            continue
        found.append(current)
        pending.extend(child_pids(current))
    return found


def rss_kib(pid: int) -> int:
    try:
        for line in pathlib.Path(f"/proc/{pid}/status").read_text(
            encoding="ascii"
        ).splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return 0
    return 0


if len(sys.argv) < 2:
    raise SystemExit("usage: measure_command.py COMMAND [ARG...]")

started = time.perf_counter_ns()
process = subprocess.Popen(sys.argv[1:])
peak_tree_rss_kib = 0
samples = 0
while process.poll() is None:
    peak_tree_rss_kib = max(
        peak_tree_rss_kib,
        sum(rss_kib(pid) for pid in process_tree(process.pid)),
    )
    samples += 1
    time.sleep(0.002)
return_code = process.wait()
print(
    json.dumps(
        {
            "command": sys.argv[1:],
            "elapsed_ns": time.perf_counter_ns() - started,
            "peak_process_tree_rss_kib": peak_tree_rss_kib,
            "samples": samples,
            "return_code": return_code,
        },
        sort_keys=True,
    )
)
raise SystemExit(return_code)
