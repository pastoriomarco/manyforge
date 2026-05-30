"""HTTP-driven qualification scenario runner for ManyForge."""
from __future__ import annotations

from dataclasses import dataclass, field, replace
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import socket
import subprocess
import tarfile
import time
from typing import Any

import yaml


_DEFAULT_SCENARIO_TIMEOUT_S = 60.0
_DEFAULT_POLL_INTERVAL_S = 0.05
_REPO_ROOT = Path(__file__).resolve().parents[2]
_PATH_TOKEN_RE = re.compile(r"([^[\]]+)|\[(\d+)\]")
_MISSING = object()


class ScenarioExecutionError(RuntimeError):
    """Raised when a scenario step fails to execute."""


class ScenarioAssertionError(AssertionError):
    """Raised when a scenario assertion fails."""


@dataclass(slots=True)
class ScenarioDefinition:
    """Structured scenario artifact."""

    scenario_id: str
    description: str = ""
    program: str | None = None
    deployment: str | None = None
    timeouts: dict[str, Any] = field(default_factory=dict)
    steps: list[dict[str, Any]] = field(default_factory=list)
    assertions: list[dict[str, Any]] = field(default_factory=list)
    forbidden: list[dict[str, Any]] = field(default_factory=list)
    tags: list[str] = field(default_factory=list)
    source_path: Path | None = None


@dataclass(slots=True)
class ScenarioMatrixDefinition:
    """Collection of qualification scenarios."""

    matrix_id: str
    description: str = ""
    scenarios: list[dict[str, Any]] = field(default_factory=list)
    source_path: Path | None = None


def load_scenario_definition(path: str | Path) -> ScenarioDefinition:
    """Load one scenario YAML file from disk."""
    scenario_path = Path(path).resolve()
    raw = yaml.safe_load(scenario_path.read_text(encoding="utf-8")) or {}
    scenario_id = str(raw.get("scenario_id") or "").strip()
    if not scenario_id:
        raise ValueError(f"{scenario_path} is missing required 'scenario_id'")
    return ScenarioDefinition(
        scenario_id=scenario_id,
        description=str(raw.get("description") or ""),
        program=str(raw["program"]) if raw.get("program") else None,
        deployment=str(raw["deployment"]) if raw.get("deployment") else None,
        timeouts=dict(raw.get("timeouts") or {}),
        steps=list(raw.get("steps") or []),
        assertions=list(raw.get("assertions") or []),
        forbidden=list(raw.get("forbidden") or []),
        tags=list(raw.get("tags") or []),
        source_path=scenario_path,
    )


def load_matrix_definition(path: str | Path) -> ScenarioMatrixDefinition:
    """Load one scenario matrix YAML file from disk."""
    matrix_path = Path(path).resolve()
    raw = yaml.safe_load(matrix_path.read_text(encoding="utf-8")) or {}
    matrix_id = str(raw.get("matrix_id") or "").strip()
    if not matrix_id:
        raise ValueError(f"{matrix_path} is missing required 'matrix_id'")
    scenarios: list[dict[str, Any]] = []
    for item in raw.get("scenarios") or []:
        if isinstance(item, str):
            scenarios.append({"path": item, "repeat": 1})
            continue
        if isinstance(item, dict) and item.get("path"):
            repeat = int(item.get("repeat", 1))
            if repeat <= 0:
                raise ValueError(f"{matrix_path} has non-positive repeat for {item['path']}")
            scenarios.append(
                {
                    "path": str(item["path"]),
                    "repeat": repeat,
                    "tags": list(item.get("tags") or []),
                }
            )
            continue
        raise ValueError(f"{matrix_path} has invalid scenario entry: {item!r}")
    if not scenarios:
        raise ValueError(f"{matrix_path} does not declare any scenarios")
    return ScenarioMatrixDefinition(
        matrix_id=matrix_id,
        description=str(raw.get("description") or ""),
        scenarios=scenarios,
        source_path=matrix_path,
    )


def expand_matrix_scenarios(
    matrix_definition: ScenarioMatrixDefinition,
    *,
    workspace_root: str | Path | None = None,
) -> list[ScenarioDefinition]:
    """Expand one scenario matrix into concrete scenario definitions."""
    root = Path(workspace_root).resolve() if workspace_root else _REPO_ROOT
    definitions: list[ScenarioDefinition] = []
    for item in matrix_definition.scenarios:
        definition = load_scenario_definition((root / item["path"]).resolve())
        tags = list(item.get("tags") or [])
        repeat = int(item.get("repeat", 1))
        if repeat == 1:
            definitions.append(replace(definition, tags=tags))
            continue
        for repeat_index in range(repeat):
            definitions.append(
                replace(
                    definition,
                    scenario_id=f"{definition.scenario_id}__repeat_{repeat_index + 1:02d}",
                    description=(
                        f"{definition.description} "
                        f"[repeat {repeat_index + 1}/{repeat}]"
                    ).strip(),
                    tags=tags,
                )
            )
    return definitions


class ScenarioRunner:
    """Execute qualification scenarios against the existing ManyForge HTTP API."""

    def __init__(self, client: Any, *, workspace_root: str | Path | None = None) -> None:
        self._client = client
        self._workspace_root = Path(workspace_root).resolve() if workspace_root else _REPO_ROOT
        self._git_commit = _git_rev_parse(self._workspace_root)

    def run_from_path(
        self,
        path: str | Path,
        *,
        output_dir: str | Path | None = None,
    ) -> dict[str, Any]:
        definition = load_scenario_definition(path)
        return self.run(definition, output_dir=output_dir)

    def run_many(
        self,
        definitions: list[ScenarioDefinition],
        *,
        matrix_definition: ScenarioMatrixDefinition | None = None,
        output_dir: str | Path | None = None,
        archive_path: str | Path | None = None,
    ) -> dict[str, Any]:
        started_at_ns = time.monotonic_ns()
        results: list[dict[str, Any]] = []
        failures: list[dict[str, Any]] = []
        for definition in definitions:
            try:
                results.append(self.run(definition, output_dir=output_dir))
            except Exception as exc:
                failures.append(
                    {
                        "scenarioId": definition.scenario_id,
                        "scenarioPath": str(definition.source_path) if definition.source_path else None,
                        "error": {
                            "type": exc.__class__.__name__,
                            "message": str(exc),
                        },
                    }
                )
        summary = {
            "success": not failures,
            "matrixId": matrix_definition.matrix_id if matrix_definition is not None else None,
            "description": matrix_definition.description if matrix_definition is not None else "",
            "matrixPath": (
                str(matrix_definition.source_path)
                if matrix_definition is not None and matrix_definition.source_path is not None
                else None
            ),
            "startedAtNs": started_at_ns,
            "finishedAtNs": time.monotonic_ns(),
            "scenarioCount": len(definitions),
            "passedCount": len(results),
            "failedCount": len(failures),
            "results": results,
            "failures": failures,
            "metadata": self._build_run_metadata(matrix_definition=matrix_definition),
            "archivePath": str(Path(archive_path).resolve()) if archive_path is not None else None,
        }
        self._write_matrix_artifacts(summary, output_dir=output_dir)
        return summary

    def run(
        self,
        definition: ScenarioDefinition,
        *,
        output_dir: str | Path | None = None,
    ) -> dict[str, Any]:
        started_at_ns = time.monotonic_ns()
        scenario_timeout_s = float(definition.timeouts.get("scenario_s", _DEFAULT_SCENARIO_TIMEOUT_S))
        scenario_deadline = time.monotonic() + scenario_timeout_s
        step_results: list[dict[str, Any]] = []
        metadata = self._build_run_metadata(definition=definition)

        try:
            for index, step in enumerate(definition.steps):
                self._ensure_before_deadline(
                    scenario_deadline,
                    f"Scenario '{definition.scenario_id}' exceeded timeout before step {index}",
                )
                step_results.append(
                    self._execute_step(
                        definition,
                        step,
                        step_index=index,
                        scenario_deadline=scenario_deadline,
                    )
                )

            assertion_results = self._evaluate_assertions(definition.assertions)
            forbidden_results = self._evaluate_forbidden(definition.forbidden)
            final_artifacts = self._collect_final_artifacts()
        except Exception as exc:
            failure_artifacts = self._collect_final_artifacts()
            result = {
                "success": False,
                "scenarioId": definition.scenario_id,
                "description": definition.description,
                "scenarioPath": str(definition.source_path) if definition.source_path is not None else None,
                "startedAtNs": started_at_ns,
                "finishedAtNs": time.monotonic_ns(),
                "stepResults": step_results,
                "assertions": [],
                "forbidden": [],
                "metadata": metadata,
                "tags": definition.tags,
                "error": {
                    "type": exc.__class__.__name__,
                    "message": str(exc),
                },
                "finalArtifacts": failure_artifacts,
            }
            self._write_artifacts(result, output_dir=output_dir)
            raise

        result = {
            "success": True,
            "scenarioId": definition.scenario_id,
            "description": definition.description,
            "scenarioPath": str(definition.source_path) if definition.source_path is not None else None,
            "startedAtNs": started_at_ns,
            "finishedAtNs": time.monotonic_ns(),
            "stepResults": step_results,
            "assertions": assertion_results,
            "forbidden": forbidden_results,
            "metadata": metadata,
            "tags": definition.tags,
            "finalArtifacts": final_artifacts,
        }
        self._write_artifacts(result, output_dir=output_dir)
        return result

    def _execute_step(
        self,
        definition: ScenarioDefinition,
        step: dict[str, Any],
        *,
        step_index: int,
        scenario_deadline: float,
    ) -> dict[str, Any]:
        action = str(step.get("action") or "").strip()
        if not action:
            raise ScenarioExecutionError(f"Step {step_index} is missing required 'action'")

        started_at_ns = time.monotonic_ns()
        response_payload: dict[str, Any] | None = None
        if action == "load_program":
            response_payload = self._load_program(definition, step)
        elif action == "cycle_run":
            response_payload = self._post_json("/api/cycle/run")
        elif action == "cycle_stop":
            response_payload = self._post_json("/api/cycle/stop")
        elif action == "cycle_reset":
            response_payload = self._post_json("/api/cycle/reset")
        elif action == "publish_signal":
            response_payload = self._publish_signal(step)
        elif action == "sleep":
            seconds = float(step.get("seconds", 0.0))
            self._ensure_before_deadline(
                scenario_deadline,
                f"Scenario deadline reached before sleep step {step_index}",
            )
            time.sleep(max(0.0, seconds))
            response_payload = {"sleptSeconds": seconds}
        elif action == "wait_for":
            condition = dict(step)
            condition.pop("action", None)
            response_payload = self._wait_for_condition(
                condition,
                scenario_deadline=scenario_deadline,
            )
        else:
            raise ScenarioExecutionError(f"Unsupported scenario action '{action}'")

        return {
            "stepIndex": step_index,
            "action": action,
            "label": str(step.get("label") or action),
            "startedAtNs": started_at_ns,
            "finishedAtNs": time.monotonic_ns(),
            "response": response_payload,
            "checkpoint": self._safe_fetch("digest"),
        }

    def _load_program(self, definition: ScenarioDefinition, step: dict[str, Any]) -> dict[str, Any]:
        program_path = self._resolve_workspace_path(str(step.get("program") or definition.program or ""))
        if not program_path:
            raise ScenarioExecutionError(
                f"Scenario '{definition.scenario_id}' has no program path for load_program"
            )
        deployment_path = None
        deployment_raw = str(step.get("deployment") or definition.deployment or "")
        if deployment_raw:
            deployment_path = self._resolve_workspace_path(deployment_raw)
        payload = {"path": program_path}
        if deployment_path:
            payload["deploymentPath"] = deployment_path
        if bool(step.get("force_discard_overrides", False)):
            payload["forceDiscardOverrides"] = True
        return self._post_json("/api/program/load", payload)

    def _publish_signal(self, step: dict[str, Any]) -> dict[str, Any]:
        payload = {
            "adapter": str(step.get("adapter") or ""),
            "signal": str(step.get("signal") or ""),
            "value": step.get("value"),
            "staleAfterMs": int(step.get("stale_after_ms", step.get("staleAfterMs", 0)) or 0),
            "mirrorKey": step.get("mirror_key", step.get("mirrorKey")),
            "clearOnStale": bool(step.get("clear_on_stale", step.get("clearOnStale", True))),
            "metadata": dict(step.get("metadata") or {}),
        }
        if payload["staleAfterMs"] <= 0:
            payload.pop("staleAfterMs")
        if payload["mirrorKey"] is None:
            payload.pop("mirrorKey")
        return self._post_json("/api/debug/signals/publish", payload)

    def _evaluate_assertions(self, assertions: list[dict[str, Any]]) -> list[dict[str, Any]]:
        results: list[dict[str, Any]] = []
        for assertion in assertions:
            matched, actual = self._evaluate_condition(assertion)
            if not matched:
                raise ScenarioAssertionError(self._format_condition_failure(assertion, actual))
            results.append({"condition": assertion, "matched": True, "actual": actual})
        return results

    def _evaluate_forbidden(self, forbidden: list[dict[str, Any]]) -> list[dict[str, Any]]:
        results: list[dict[str, Any]] = []
        for condition in forbidden:
            matched, actual = self._evaluate_condition(condition)
            if matched:
                raise ScenarioAssertionError(
                    f"Forbidden condition matched: {self._condition_summary(condition)} (actual={actual!r})"
                )
            results.append({"condition": condition, "matched": False, "actual": actual})
        return results

    def _wait_for_condition(
        self,
        condition: dict[str, Any],
        *,
        scenario_deadline: float,
    ) -> dict[str, Any]:
        timeout_s = float(condition.get("timeout_s", condition.get("timeoutS", 5.0)))
        deadline = min(time.monotonic() + timeout_s, scenario_deadline)
        poll_interval_s = self._poll_interval_s(condition)
        last_actual: Any = None
        while time.monotonic() < deadline:
            matched, actual = self._evaluate_condition(condition)
            last_actual = actual
            if matched:
                return {
                    "condition": condition,
                    "matched": True,
                    "actual": actual,
                }
            time.sleep(poll_interval_s)
        raise ScenarioAssertionError(self._format_condition_failure(condition, last_actual))

    def _evaluate_condition(self, condition: dict[str, Any]) -> tuple[bool, Any]:
        endpoint = str(condition.get("endpoint") or "").strip()
        if not endpoint:
            raise ScenarioExecutionError(f"Condition is missing required 'endpoint': {condition}")
        payload = self._fetch_endpoint(endpoint, query=dict(condition.get("query") or {}))

        event_type = condition.get("event_type", condition.get("eventType"))
        if event_type is not None:
            event_key = "eventType" if endpoint == "medkit_trace" else "type"
            records = payload.get("records") if endpoint == "medkit_trace" else payload.get("events")
            if not isinstance(records, list):
                raise ScenarioExecutionError(
                    f"Endpoint '{endpoint}' does not provide an event list for event_type matching"
                )
            matches = [record for record in records if record.get(event_key) == event_type]
            if bool(condition.get("absent", False)):
                return (not matches, len(matches))
            match_count = len(matches)
            if any(
                key in condition
                for key in ("equals", "not_equals", "greater_equal", "less_equal", "contains", "truthy")
            ):
                return (self._match_expected(match_count, condition), match_count)
            return (bool(matches), match_count)

        path = str(condition.get("path") or "").strip()
        actual = payload if not path else self._resolve_payload_path(payload, path)
        return self._match_expected(actual, condition), actual

    def _fetch_endpoint(self, endpoint: str, *, query: dict[str, Any] | None = None) -> dict[str, Any]:
        route = {
            "digest": "/api/execution/digest",
            "history": "/api/execution/history",
            "blackboard": "/api/execution/blackboard",
            "scene_diagnostics": "/api/scene/diagnostics",
            "scene_trace": "/api/debug/scene-trace",
            "scene_snapshot": "/api/scene/snapshot",
            "debug_export": "/api/debug/export",
            "medkit_trace": "/api/debug/export/ros2-medkit",
            "cycle_status": "/api/cycle/status",
            "program": "/api/program",
        }.get(endpoint)
        if route is None:
            raise ScenarioExecutionError(f"Unsupported condition endpoint '{endpoint}'")
        if endpoint == "scene_snapshot":
            return self._get_binary_summary(route, params=query or None)
        return self._get_json(route, params=query or None)

    def _collect_final_artifacts(self) -> dict[str, Any]:
        return {
            "digest": self._safe_fetch("digest"),
            "history": self._safe_fetch("history"),
            "blackboard": self._safe_fetch("blackboard"),
            "sceneDiagnostics": self._safe_fetch("scene_diagnostics"),
            "sceneSnapshot": self._safe_fetch("scene_snapshot"),
            "sceneTrace": self._safe_fetch("scene_trace"),
            "debugExport": self._safe_fetch("debug_export"),
        }

    def _build_run_metadata(
        self,
        *,
        definition: ScenarioDefinition | None = None,
        matrix_definition: ScenarioMatrixDefinition | None = None,
    ) -> dict[str, Any]:
        metadata = {
            "workspaceRoot": str(self._workspace_root),
            "hostname": socket.gethostname(),
            "platform": platform.platform(),
            "pythonVersion": platform.python_version(),
            "gitCommit": self._git_commit,
            "baseUrl": str(getattr(self._client, "base_url", "")),
            "timestampNs": time.monotonic_ns(),
        }
        if definition is not None:
            metadata["scenarioPath"] = (
                str(definition.source_path) if definition.source_path is not None else None
            )
            metadata["scenarioSha256"] = _sha256_path(definition.source_path)
        if matrix_definition is not None:
            metadata["matrixPath"] = (
                str(matrix_definition.source_path) if matrix_definition.source_path is not None else None
            )
            metadata["matrixSha256"] = _sha256_path(matrix_definition.source_path)
        return metadata

    def _safe_fetch(self, endpoint: str) -> dict[str, Any] | None:
        try:
            return self._fetch_endpoint(endpoint)
        except Exception:
            return None

    def _post_json(self, path: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
        response = self._client.post(path, json=payload)
        if response.status_code >= 400:
            raise ScenarioExecutionError(
                f"POST {path} failed with {response.status_code}: {response.text}"
            )
        return response.json()

    def _get_json(self, path: str, *, params: dict[str, Any] | None = None) -> dict[str, Any]:
        response = self._client.get(path, params=params)
        if response.status_code >= 400:
            raise ScenarioExecutionError(
                f"GET {path} failed with {response.status_code}: {response.text}"
            )
        return response.json()

    def _get_binary_summary(self, path: str, *, params: dict[str, Any] | None = None) -> dict[str, Any]:
        response = self._client.get(path, params=params)
        if response.status_code >= 400:
            raise ScenarioExecutionError(
                f"GET {path} failed with {response.status_code}: {response.text}"
            )
        return {
            "statusCode": response.status_code,
            "contentType": response.headers.get("content-type"),
            "contentLength": len(response.content),
            "etag": response.headers.get("etag"),
        }

    def _resolve_workspace_path(self, raw_path: str) -> str | None:
        candidate = raw_path.strip()
        if not candidate:
            return None
        path = Path(candidate)
        if path.is_absolute():
            return str(path)
        return str((self._workspace_root / path).resolve())

    def _poll_interval_s(self, scope: dict[str, Any]) -> float:
        raw = scope.get("poll_interval_ms", scope.get("pollIntervalMs"))
        if raw is None:
            raw = _DEFAULT_POLL_INTERVAL_S * 1000.0
        return max(float(raw) / 1000.0, 0.01)

    def _ensure_before_deadline(self, deadline: float, message: str) -> None:
        if time.monotonic() >= deadline:
            raise ScenarioExecutionError(message)

    def _resolve_payload_path(self, payload: Any, path: str) -> Any:
        current = payload
        for segment in path.split("."):
            if segment == "":
                continue
            for key_token, index_token in _PATH_TOKEN_RE.findall(segment):
                token = key_token or index_token
                if key_token:
                    if not isinstance(current, dict) or token not in current:
                        return _MISSING
                    current = current[token]
                else:
                    if not isinstance(current, list):
                        return _MISSING
                    index = int(token)
                    if index >= len(current):
                        return _MISSING
                    current = current[index]
        return current

    def _condition_summary(self, condition: dict[str, Any]) -> str:
        endpoint = condition.get("endpoint")
        event_type = condition.get("event_type", condition.get("eventType"))
        if event_type is not None:
            qualifier = "absent" if bool(condition.get("absent", False)) else "present"
            return f"{endpoint} event_type={event_type!r} {qualifier}"
        path = condition.get("path")
        if "equals" in condition:
            return f"{endpoint}.{path} == {condition['equals']!r}"
        if "not_equals" in condition:
            return f"{endpoint}.{path} != {condition['not_equals']!r}"
        if "contains" in condition:
            return f"{endpoint}.{path} contains {condition['contains']!r}"
        if bool(condition.get("truthy", False)):
            return f"{endpoint}.{path} is truthy"
        return str(condition)

    def _format_condition_failure(self, condition: dict[str, Any], actual: Any) -> str:
        return (
            "Condition not met: "
            f"{self._condition_summary(condition)} (actual={actual!r})"
        )

    def _write_artifacts(self, result: dict[str, Any], *, output_dir: str | Path | None) -> None:
        if output_dir is None:
            return
        target_dir = Path(output_dir).resolve()
        target_dir.mkdir(parents=True, exist_ok=True)
        scenario_prefix = target_dir / result["scenarioId"]
        (scenario_prefix.with_suffix(".result.json")).write_text(
            json.dumps(result, indent=2, sort_keys=True),
            encoding="utf-8",
        )
        final_artifacts = result.get("finalArtifacts") or {}
        for artifact_name, payload in final_artifacts.items():
            if payload is None:
                continue
            (target_dir / f"{result['scenarioId']}.{artifact_name}.json").write_text(
                json.dumps(payload, indent=2, sort_keys=True),
                encoding="utf-8",
            )

    def _write_matrix_artifacts(self, summary: dict[str, Any], *, output_dir: str | Path | None) -> None:
        if output_dir is None or not summary.get("matrixId"):
            return
        target_dir = Path(output_dir).resolve()
        target_dir.mkdir(parents=True, exist_ok=True)
        (target_dir / f"{summary['matrixId']}.summary.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True),
            encoding="utf-8",
        )
        archive_path = summary.get("archivePath")
        if archive_path:
            self._archive_artifacts(target_dir, Path(archive_path))

    def _archive_artifacts(self, source_dir: Path, archive_path: Path) -> None:
        resolved_source = source_dir.resolve()
        resolved_archive = archive_path.resolve()
        if resolved_archive.is_relative_to(resolved_source):
            raise ScenarioExecutionError(
                f"Archive path must not be inside the artifact directory: {resolved_archive}"
            )
        archive_path.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive_path, "w:gz") as tar:
            tar.add(source_dir, arcname=source_dir.name)

    def _match_expected(self, actual: Any, condition: dict[str, Any]) -> bool:
        if "equals" in condition:
            return actual == condition["equals"]
        if "not_equals" in condition:
            return actual != condition["not_equals"]
        if "greater_equal" in condition:
            return actual is not _MISSING and actual >= condition["greater_equal"]
        if "less_equal" in condition:
            return actual is not _MISSING and actual <= condition["less_equal"]
        if "contains" in condition:
            expected = condition["contains"]
            if isinstance(actual, str):
                return str(expected) in actual
            if isinstance(actual, list):
                return expected in actual
            if isinstance(actual, dict):
                return expected in actual.values() or expected in actual.keys()
            return False
        if bool(condition.get("truthy", False)):
            return bool(actual)
        raise ScenarioExecutionError(
            f"Condition must define one of equals/not_equals/greater_equal/less_equal/contains/truthy/event_type: {condition}"
        )


def _sha256_path(path: Path | None) -> str | None:
    if path is None or not path.exists():
        return None
    digest = hashlib.sha256()
    digest.update(path.read_bytes())
    return digest.hexdigest()


def _git_rev_parse(workspace_root: Path) -> str | None:
    for env_name in ("MANYFORGE_GIT_COMMIT", "GIT_COMMIT"):
        value = os.environ.get(env_name, "").strip()
        if re.fullmatch(r"[0-9a-fA-F]{7,40}", value):
            return value

    commands = [
        ["git", "-C", str(workspace_root), "rev-parse", "HEAD"],
        [
            "git",
            "-c",
            f"safe.directory={workspace_root}",
            "-C",
            str(workspace_root),
            "rev-parse",
            "HEAD",
        ],
    ]
    for command in commands:
        try:
            completed = subprocess.run(
                command,
                capture_output=True,
                check=True,
                text=True,
            )
        except Exception:
            continue
        value = completed.stdout.strip()
        if value:
            return value
    return None
