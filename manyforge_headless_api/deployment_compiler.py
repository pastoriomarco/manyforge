"""Bounded worker for compiling one authored Deployment candidate."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import tempfile
import threading
from typing import Any, Mapping

import rfc8785

from .native import HeadlessApiError


_MAXIMUM_COMPILER_OUTPUT_BYTES = 64 * 1024
_EXPECTED_RESULT_FIELDS = {
    "deployment_artifact_id",
    "deployment_revision",
    "graph_revision",
    "program_binding_id",
    "program_id",
    "program_revision",
    "retained_bundle_id",
    "scenario_catalog_revision",
    "scenario_id",
    "schema_version",
}


class DeploymentCandidateCompiler:
    """Serialize one compiler effect without owning active selection."""

    def __init__(
        self,
        *,
        executable: Path,
        workspace_root: Path,
        installed_package_root: Path,
        candidate_package_root: Path,
        prepared_roots: tuple[tuple[str, Path], ...],
        timeout_seconds: float,
    ) -> None:
        self._executable = executable
        self._workspace_root = workspace_root
        self._installed_package_root = installed_package_root
        self._candidate_package_root = candidate_package_root
        self._prepared_roots = prepared_roots
        self._timeout_seconds = timeout_seconds
        self._lock = threading.Lock()

    def compile(
        self,
        *,
        scenario_id: str,
        expected_scenario_catalog_revision: str,
        deployment: Mapping[str, Any],
        program: Mapping[str, Any],
    ) -> dict[str, Any]:
        if not self._lock.acquire(blocking=False):
            raise HeadlessApiError(
                "deployment_candidate_compiler_busy",
                "another Deployment candidate is already compiling",
            )
        draft_path: Path | None = None
        program_path: Path | None = None
        try:
            encoded = rfc8785.dumps(dict(deployment))
            encoded_program = rfc8785.dumps(dict(program))
            with tempfile.NamedTemporaryFile(
                mode="wb",
                prefix="deployment-candidate-",
                suffix=".json",
                dir=self._candidate_package_root,
                delete=False,
            ) as draft:
                draft.write(encoded)
                draft.flush()
                draft_path = Path(draft.name)
            with tempfile.NamedTemporaryFile(
                mode="wb",
                prefix="program-candidate-",
                suffix=".json",
                dir=self._candidate_package_root,
                delete=False,
            ) as program_file:
                program_file.write(encoded_program)
                program_file.flush()
                program_path = Path(program_file.name)
            command = [
                str(self._executable),
                "--compile-candidate",
                "--workspace-root",
                str(self._workspace_root),
                "--base-package-root",
                str(self._installed_package_root),
                "--candidate-package-root",
                str(self._candidate_package_root),
                "--candidate-deployment",
                str(draft_path),
                "--candidate-program",
                str(program_path),
                "--scenario-id",
                scenario_id,
                "--expected-scenario-catalog-revision",
                expected_scenario_catalog_revision,
            ]
            for family_id, prepared_root in self._prepared_roots:
                command.extend(("--prepared", f"{family_id}={prepared_root}"))
            with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
                try:
                    completed = subprocess.run(
                        command,
                        stdin=subprocess.DEVNULL,
                        stdout=stdout,
                        stderr=stderr,
                        check=False,
                        timeout=self._timeout_seconds,
                        close_fds=True,
                    )
                except subprocess.TimeoutExpired as error:
                    raise HeadlessApiError(
                        "deployment_candidate_compiler_timeout",
                        "Deployment candidate compilation timed out",
                    ) from error
                stdout_size = stdout.tell()
                stderr_size = stderr.tell()
                if (
                    stdout_size > _MAXIMUM_COMPILER_OUTPUT_BYTES
                    or stderr_size > _MAXIMUM_COMPILER_OUTPUT_BYTES
                ):
                    raise HeadlessApiError(
                        "deployment_candidate_compiler_output_oversized",
                        "Deployment candidate compiler output exceeded its bound",
                    )
                stdout.seek(0)
                stderr.seek(0)
                output = stdout.read()
                detail = stderr.read().decode("utf-8", errors="replace").strip()
            if completed.returncode != 0:
                raise HeadlessApiError(
                    "deployment_candidate_compilation_failed",
                    "Deployment candidate compilation failed",
                    details={"compiler_detail": detail[:4096]},
                )
            try:
                value = json.loads(output.decode("utf-8", errors="strict"))
            except (UnicodeError, json.JSONDecodeError) as error:
                raise HeadlessApiError(
                    "deployment_candidate_compiler_response_invalid",
                    "Deployment candidate compiler returned invalid JSON",
                ) from error
            if (
                not isinstance(value, dict)
                or set(value) != _EXPECTED_RESULT_FIELDS
                or value.get("schema_version")
                != "manyforge.deployment_candidate_compilation.v1"
                or value.get("scenario_id") != scenario_id
            ):
                raise HeadlessApiError(
                    "deployment_candidate_compiler_response_invalid",
                    "Deployment candidate compiler returned a divergent result",
                )
            return value
        finally:
            if draft_path is not None:
                draft_path.unlink(missing_ok=True)
            if program_path is not None:
                program_path.unlink(missing_ok=True)
            self._lock.release()
