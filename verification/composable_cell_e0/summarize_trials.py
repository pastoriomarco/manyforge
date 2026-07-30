from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--trials", type=int, default=8)
    args = parser.parse_args()

    records: dict[str, dict[str, list[float]]] = {}
    raw_trials: list[dict[str, object]] = []
    verifier = Path(__file__).resolve().with_name("verify.py")
    for index in range(args.trials):
        command = [
            sys.executable,
            str(verifier),
            "--source-root",
            str(args.source_root),
        ]
        if index % 2:
            command.append("--reverse")
        result = json.loads(subprocess.check_output(command, text=True))
        raw_trials.append(result)
        for category in ("exercise", "import"):
            for item in result[category]:
                module = item["module"]
                metrics = records.setdefault(module, {})
                for key, value in item.items():
                    if key != "module":
                        metrics.setdefault(key, []).append(float(value))

    summary = {
        "raw_trials": raw_trials,
        "summary": {
            module: {
                key: {
                    "median": statistics.median(values),
                    "minimum": min(values),
                    "maximum": max(values),
                }
                for key, values in metrics.items()
            }
            for module, metrics in records.items()
        },
    }
    print(json.dumps(summary, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
