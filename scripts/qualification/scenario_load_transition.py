#!/usr/bin/env python3
"""Explicit live, non-motion Scenario Load regression; run inside the dev container.

Each PROJECT is a path visible to Composer. This mutates the simulated workbench:
it requires an idle, ready baseline and stops on the first failure, never retries
a Load or restarts a component. It does not qualify motion or external providers.
"""
from __future__ import annotations

import argparse
import json
import time
import uuid
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

        baseline = read(f"{args.composer}/api/scenarios/current")
        if baseline.get("ready") is not True:
            raise RuntimeError("A ready baseline is required; no Load was issued")
        for project in args.projects:
            workspace = read(f"{args.composer}/api/authoring/workspace")
            admission = read(f"{args.headless}/api/cell/authoring-admission")
            if admission.get("authoring_allowed") is not True:
                raise RuntimeError("Authoring is not admitted; no Load was issued")
            request_id = str(uuid.uuid4())
            response = client.post(
                f"{args.composer}/api/scenario-documents/load",
                timeout=args.timeout,
                json={
                    "path": str(Path(project).resolve()),
                    "confirmDiscard": False,
                    "draftId": workspace["program"]["draftId"],
                    "expectedDraftRevision": workspace["program"]["draftRevision"],
                    "deploymentDraftId": workspace["deployment"]["draftId"],
                    "expectedDeploymentDraftRevision": workspace["deployment"]["draftRevision"],
                    "expectedWorkspaceEpoch": workspace["workspaceEpoch"],
                    "expectedWorkspaceRevision": workspace["workspaceRevision"],
                    "expectedAdmissionGeneration": admission["admission_generation"],
                    "requestOwnerEpoch": "scenario-load-regression",
                    "requestId": request_id,
                },
            )
            if response.status_code != 200:
                raise RuntimeError(f"Load {request_id}: {response.status_code} {response.text}")
            accepted = response.json()
            if accepted.get("status") != "accepted":
                raise RuntimeError(f"Load did not accept activation: {accepted}")
            selected = accepted["activation"]["prepared_activation"]
            deadline = time.monotonic() + args.timeout
            current: dict = {}
            while time.monotonic() < deadline:
                observation = client.get(f"{args.composer}/api/scenarios/current")
                current = observation.json()
                if observation.status_code == 200 and current.get("ready") is True:
                    actual = current["current_scenario"]
                    if all(actual.get(key) == selected[key] for key in (
                        "scenario_id", "program_revision", "program_binding_id",
                        "retained_bundle_id", "graph_revision",
                    )):
                        settled = read(f"{args.headless}/api/cell/authoring-admission")
                        if settled.get("authoring_allowed") is True:
                            break
                # Bounded polling of a real process, never a repeated effect.
                time.sleep(0.2)
            else:
                manager = read(f"{args.headless}/api/manager/status")
                apply = manager["status"]["apply"]
                summary = {
                    "unavailable_reason_code": apply.get("unavailable_reason_code"),
                    "records": [{key: row.get(key) for key in (
                        "apply_id", "phase", "terminal_state", "incomplete",
                    )} for row in apply.get("records", [])],
                }
                raise RuntimeError(f"Load {request_id} never became ready: "
                                   f"{current}; Apply={summary}")
            print(json.dumps({"request_id": request_id, "project": project,
                              "current_scenario": current["current_scenario"],
                              "ready": True}), flush=True)


if __name__ == "__main__":
    main()
