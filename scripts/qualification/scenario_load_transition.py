#!/usr/bin/env python3
"""Explicit live, non-motion Scenario Load regression; run inside the dev container.

Each PROJECT is a path visible to Composer. This mutates the simulated workbench:
it requires an idle baseline (which may be empty) and stops on the first failure, never retries
a Load or restarts a component. It does not qualify motion or external providers.
"""
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import httpx


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("projects", nargs="+")
    parser.add_argument("--composer", default="http://127.0.0.1:9020")
    parser.add_argument("--headless", default="http://127.0.0.1:9010")
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--confirm-simulated-workbench", action="store_true", required=True)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    with httpx.Client(timeout=10) as client:
        def read(url: str) -> dict:
            response = client.get(url)
            response.raise_for_status()
            return response.json()

        baseline_cell_id = read(f"{args.headless}/api/manager/status")["status"]["cell_id"]
        for project in args.projects:
            admission = read(f"{args.headless}/api/cell/authoring-admission")
            if admission.get("authoring_allowed") is not True:
                raise RuntimeError("Authoring is not admitted; no Load was issued")
            # CONTRACTS §11: Load is Close, then Apply of the file; the server
            # runs it and returns ApplyResult once the Apply is terminal.
            response = client.post(
                f"{args.composer}/api/scenario/load",
                timeout=args.timeout,
                json={"path": str(Path(project).resolve())},
            )
            if response.status_code != 200:
                raise RuntimeError(f"Load {project}: {response.status_code} {response.text}")
            result = response.json()
            if result.get("outcome") != "succeeded":
                raise RuntimeError(f"Load {project} failed: {result.get('error')}")
            deadline = time.monotonic() + args.timeout
            current: dict = {}
            while time.monotonic() < deadline:
                observation = client.get(f"{args.composer}/api/scenarios/current")
                current = observation.json()
                if (observation.status_code == 200 and current.get("ready") is True
                        and current["current_scenario"].get("scenario_id") == result["scenario_id"]):
                    break
                # Bounded polling of a real process, never a repeated effect.
                time.sleep(0.2)
            else:
                raise RuntimeError(f"Load {project} never became current: {current}")
            cell_id = read(f"{args.headless}/api/manager/status")["status"]["cell_id"]
            if cell_id != baseline_cell_id:
                raise RuntimeError("Scenario Load changed the runtime identity")
            print(json.dumps({"project": project,
                              "runtime_cell_id": cell_id,
                              "current_scenario": current["current_scenario"],
                              "ready": True}), flush=True)


if __name__ == "__main__":
    main()
