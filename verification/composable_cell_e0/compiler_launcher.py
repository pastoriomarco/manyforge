from __future__ import annotations

import json
import resource
import subprocess
import sys
import time


started = time.perf_counter_ns()
completed = subprocess.run(sys.argv[1:], check=False)
usage = resource.getrusage(resource.RUSAGE_CHILDREN)
print(
    "E0_COMPILER_METRIC "
    + json.dumps(
        {
            "source": next(
                (
                    value
                    for value in reversed(sys.argv[1:])
                    if value.endswith(".cpp")
                ),
                "",
            ),
            "elapsed_ns": time.perf_counter_ns() - started,
            "compiler_maxrss_kib": usage.ru_maxrss,
            "return_code": completed.returncode,
        },
        sort_keys=True,
    ),
    file=sys.stderr,
)
raise SystemExit(completed.returncode)
