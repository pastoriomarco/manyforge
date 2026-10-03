"""Bounded worker for preparing one captured activation input."""

from __future__ import annotations

from copy import deepcopy
from functools import lru_cache
import json
import hashlib
import logging
import re
from pathlib import Path
import subprocess
import tempfile
import threading
from typing import Any, Mapping

import rfc8785

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.model_source_capture import SOURCE_CLOSURE_SCHEMA
from manyforge_behavior.model_source_preparation import (
    PREPARED_MODEL_CLOSURE_SCHEMA,
    StructuralModelPreparationError,
    prepare_captured_model_source,
)
from manyforge_behavior.node_environment import BUILTIN_CATALOG_PATH
from manyforge_behavior.program import Program, ProgramValidationError

from .native import (
    HeadlessApiError,
    MAX_ACTIVATION_PREPARATION_REQUEST_BYTES,
)


_MAXIMUM_COMPILER_OUTPUT_BYTES = 128 * 1024 * 1024
_MAXIMUM_COMPILER_ERROR_BYTES = 64 * 1024
_RECEIPT_FIELDS = {
    "assistant_mode",
    "captured_input_digest",
    "deployment_artifact_id",
    "deployment_revision",
    "graph_revision",
    "node_environment_digest",
    "prepared_activation_id",
    "program_id",
    "program_outcome",
    "program_revision",
    "retained_bundle_id",
    "scenario_id",
    "schema_version",
    "source_document_digest",
}
_EXPECTED_RESULT_FIELDS = _RECEIPT_FIELDS | {"activation_payload"}

_PROFILE_DESCRIPTION = Path(
    "manyforge_deployment/resources/maintained/"
    "maintained_deployment_sources.v1.json"
)


def _model_preparation_profiles(
    capability_root: Path,
) -> dict[str, tuple[str, str]]:
    """Read the minimum repository-owned profile facts needed by Python XML prep.

    Native preparation independently validates the complete profile and intent
    join.  This projection only supplies the model identity and accepted package
    prefix required to rewrite the already captured source bytes.
    """

    try:
        root = json.loads((capability_root / _PROFILE_DESCRIPTION).read_bytes())
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise HeadlessApiError(
            "activation_model_profile_invalid",
            "Activation model profiles could not be loaded",
        ) from error
    if not isinstance(root, dict):
        raise HeadlessApiError(
            "activation_model_profile_invalid",
            "Activation model profiles have an invalid shape",
        )
    families: dict[str, tuple[str, str]] = {}
    family_rows = root.get("model_families")
    if not isinstance(family_rows, list):
        raise HeadlessApiError(
            "activation_model_profile_invalid",
            "Activation model families are missing",
        )
    for row in family_rows:
        source = row.get("source") if isinstance(row, dict) else None
        family_id = row.get("model_family_id") if isinstance(row, dict) else None
        model_asset_id = row.get("model_asset_id") if isinstance(row, dict) else None
        prefix = (
            source.get("accepted_package_uri_prefix")
            if isinstance(source, dict)
            else None
        )
        if (
            not isinstance(family_id, str)
            or not family_id
            or not isinstance(model_asset_id, str)
            or not model_asset_id
            or not isinstance(prefix, str)
            or not prefix
            or family_id in families
        ):
            raise HeadlessApiError(
                "activation_model_profile_invalid",
                "Activation model families contain an invalid row",
            )
        families[family_id] = (model_asset_id, prefix)
    profiles: dict[str, tuple[str, str]] = {}
    profile_rows = root.get("runtime_restoration_profiles")
    if not isinstance(profile_rows, list):
        raise HeadlessApiError(
            "activation_model_profile_invalid",
            "Activation runtime profiles are missing",
        )
    for row in profile_rows:
        profile_id = (
            row.get("runtime_restoration_profile_id")
            if isinstance(row, dict)
            else None
        )
        family_id = row.get("model_family_id") if isinstance(row, dict) else None
        family = families.get(family_id) if isinstance(family_id, str) else None
        if (
            not isinstance(profile_id, str)
            or not profile_id
            or family is None
            or profile_id in profiles
        ):
            raise HeadlessApiError(
                "activation_model_profile_invalid",
                "Activation runtime profiles contain an invalid row",
            )
        profiles[profile_id] = family
    return profiles


@lru_cache(maxsize=1)
def _program_catalog() -> NodeCatalog:
    """Load the build-owned authoring catalog once per headless process."""

    return NodeCatalog(BUILTIN_CATALOG_PATH)


def _require_admitted_program(value: Mapping[str, Any]) -> None:
    """Admit the prepared Program once against the authoring Program contract.

    Native preparation proves the Program's content identity, envelope, roles,
    tree, key-typed parameters, value sources, expressions and execution
    groups against the retained catalog, but not the remaining node-catalog
    parameter, Scene, blackboard and contact-policy semantics the Program
    validator owns. The admitted Program is content-addressed, so this one
    check covers every later read of the Applied Program.
    """

    program = value["activation_payload"].get("program")
    if value.get("program_id") is None:
        if program is not None:
            raise HeadlessApiError(
                "activation_preparer_response_invalid",
                "Activation preparer returned a Program for no Program",
            )
        return
    if (
        not isinstance(program, dict)
        or program.get("program_id") != value.get("program_id")
        or program.get("program_revision") != value.get("program_revision")
    ):
        raise HeadlessApiError(
            "activation_preparer_response_invalid",
            "Activation preparer returned a divergent Program",
        )
    try:
        admitted = Program.from_dict(
            program, _program_catalog(), path="<activation-prepared-program>"
        ).to_dict()
    except (ProgramValidationError, TypeError, ValueError) as error:
        raise HeadlessApiError(
            "activation_preparation_failed",
            "Activation preparation refused an invalid Program",
            details={"preparer_detail": str(error)[:4096]},
        ) from error
    if admitted != program:
        raise HeadlessApiError(
            "activation_preparation_failed",
            "Activation preparation refused a Program that is not in its "
            "validated canonical form",
        )


def _prepare_model_closure(
    *,
    deployment: Mapping[str, Any],
    model_source_closure: Mapping[str, Any],
    profiles: Mapping[str, tuple[str, str]],
) -> dict[str, Any]:
    if (
        set(model_source_closure) != {"components", "schema_version"}
        or model_source_closure.get("schema_version") != SOURCE_CLOSURE_SCHEMA
        or not isinstance(model_source_closure.get("components"), list)
        or not isinstance(deployment.get("components"), list)
    ):
        raise HeadlessApiError(
            "activation_model_source_invalid",
            "Captured activation model sources have an invalid shape",
        )
    source_by_component: dict[str, Mapping[str, object]] = {}
    for source in model_source_closure["components"]:
        component_id = source.get("component_id") if isinstance(source, dict) else None
        if (
            not isinstance(component_id, str)
            or not component_id
            or component_id in source_by_component
        ):
            raise HeadlessApiError(
                "activation_model_source_invalid",
                "Captured activation model sources contain duplicate components",
            )
        source_by_component[component_id] = source
    prepared: list[dict[str, object]] = []
    seen_components: set[str] = set()
    try:
        for component in deployment["components"]:
            component_id = (
                component.get("component_id") if isinstance(component, dict) else None
            )
            profile_id = (
                component.get("component_profile_id")
                if isinstance(component, dict)
                else None
            )
            profile = profiles.get(profile_id) if isinstance(profile_id, str) else None
            source = (
                source_by_component.get(component_id)
                if isinstance(component_id, str)
                else None
            )
            if (
                not isinstance(component_id, str)
                or not component_id
                or component_id in seen_components
                or profile is None
                or source is None
            ):
                raise HeadlessApiError(
                    "activation_model_profile_invalid",
                    "Deployment components do not match captured model profiles",
                )
            seen_components.add(component_id)
            model_asset_id, package_prefix = profile
            prepared.append(
                prepare_captured_model_source(
                    source,
                    model_asset_id=model_asset_id,
                    accepted_package_uri_prefix=package_prefix,
                )
            )
    except StructuralModelPreparationError as error:
        raise HeadlessApiError(
            "activation_model_preparation_failed",
            "Activation model source preparation failed",
            details={"preparer_detail": str(error)[:4096]},
        ) from error
    if seen_components != set(source_by_component):
        raise HeadlessApiError(
            "activation_model_source_invalid",
            "Captured activation model sources contain an unused component",
        )
    prepared.sort(key=lambda row: str(row["component_id"]))
    return {
        "schema_version": PREPARED_MODEL_CLOSURE_SCHEMA,
        "components": prepared,
    }


class ActivationPreparer:
    """Serialize one exact preparation effect without owning activation."""

    def __init__(
        self,
        *,
        executable: Path,
        capability_root: Path,
        activation_root: Path,
        runtime_cell_id: str,
        timeout_seconds: float | None,
        publish_canonical_empty: bool = False,
    ) -> None:
        if not isinstance(runtime_cell_id, str) or re.fullmatch(
            r"[a-z][a-z0-9_.-]{0,127}", runtime_cell_id
        ) is None:
            raise ValueError("runtime_cell_id must be a bounded portable identifier")
        self._runtime_cell_id = runtime_cell_id
        self._executable = executable
        self._capability_root = capability_root
        self._activation_root = activation_root
        self._profiles = _model_preparation_profiles(capability_root)
        self._timeout_seconds = timeout_seconds
        self._publish_canonical_empty = publish_canonical_empty
        self._lock = threading.Lock()
        self._prepared: dict[str, dict[str, Any]] = {}

    def prepared_value(self, prepared_activation_id: str) -> dict[str, Any] | None:
        """Return the capacity-one immutable activation receipt."""

        with self._lock:
            value = self._prepared.get(prepared_activation_id)
            return None if value is None else deepcopy(value)

    def release(self, prepared_activation_id: str) -> None:
        """Release an accepted, refused, or cancelled preparation explicitly."""

        with self._lock:
            self._prepared.pop(prepared_activation_id, None)

    def prepare(
        self,
        *,
        source_document_digest: str,
        scenario_id: str,
        captured_input_digest: str,
        deployment: Mapping[str, Any],
        model_source_closure: Mapping[str, Any],
        program: Mapping[str, Any] | None,
        program_role_assignment: Mapping[str, Any],
    ) -> dict[str, Any]:
        if not self._lock.acquire(blocking=False):
            raise HeadlessApiError(
                "activation_preparer_busy",
                "another activation input is already being prepared",
            )
        input_path: Path | None = None
        try:
            prepared_model_closure = _prepare_model_closure(
                deployment=deployment,
                model_source_closure=model_source_closure,
                profiles=self._profiles,
            )
            request = {
                "schema_version": "manyforge.activation_preparation_request.v3",
                "runtime_cell_id": self._runtime_cell_id,
                "source_document_digest": source_document_digest,
                "scenario_id": scenario_id,
                "captured_input_digest": captured_input_digest,
                "deployment": dict(deployment),
                "model_source_closure": dict(model_source_closure),
                "prepared_model_closure": prepared_model_closure,
                "program": dict(program) if program is not None else None,
                "program_role_assignment": dict(program_role_assignment),
            }
            encoded = rfc8785.dumps(request)
            if len(encoded) > MAX_ACTIVATION_PREPARATION_REQUEST_BYTES:
                raise HeadlessApiError(
                    "activation_preparation_request_oversized",
                    "Activation preparation request exceeded its encoded bound",
                )
            with tempfile.NamedTemporaryFile(
                mode="wb",
                prefix="activation-preparation-",
                suffix=".json",
                delete=False,
            ) as input_file:
                input_file.write(encoded)
                input_file.flush()
                input_path = Path(input_file.name)
            command = [
                str(self._executable),
                "--input",
                str(input_path),
                "--capability-root",
                str(self._capability_root),
                "--activation-root",
                str(self._activation_root),
            ]
            if self._publish_canonical_empty:
                command.append("--publish-canonical-empty")
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
                        "activation_preparation_timeout",
                        "Activation preparation timed out",
                    ) from error
                stdout_size = stdout.tell()
                stderr_size = stderr.tell()
                if (
                    stdout_size > _MAXIMUM_COMPILER_OUTPUT_BYTES
                    or stderr_size > _MAXIMUM_COMPILER_ERROR_BYTES
                ):
                    raise HeadlessApiError(
                        "activation_preparer_output_oversized",
                        "Activation preparer output exceeded its bound",
                    )
                stdout.seek(0)
                stderr.seek(0)
                output = stdout.read()
                detail = stderr.read().decode("utf-8", errors="replace").strip()
            if completed.returncode != 0:
                logging.getLogger(__name__).error(
                    "Activation preparation failed: runtime=%s scenario=%s input=%s detail=%s",
                    self._runtime_cell_id,
                    scenario_id,
                    captured_input_digest,
                    detail[:4096],
                )
                raise HeadlessApiError(
                    "activation_preparation_failed",
                    "Activation preparation failed",
                    details={"preparer_detail": detail[:4096]},
                )
            try:
                value = json.loads(output.decode("utf-8", errors="strict"))
            except (UnicodeError, json.JSONDecodeError) as error:
                raise HeadlessApiError(
                    "activation_preparer_response_invalid",
                    "Activation preparer returned invalid JSON",
                ) from error
            if (
                not isinstance(value, dict)
                or set(value) != _EXPECTED_RESULT_FIELDS
                or value.get("schema_version")
                != "manyforge.prepared_activation_input.v5"
                or value.get("source_document_digest") != source_document_digest
                or value.get("captured_input_digest") != captured_input_digest
            ):
                raise HeadlessApiError(
                    "activation_preparer_response_invalid",
                    "Activation preparer returned a divergent result",
                )
            identity = dict(value)
            prepared_activation_id = identity.pop("prepared_activation_id")
            expected_identity = "sha256:" + hashlib.sha256(
                rfc8785.dumps(identity)
            ).hexdigest()
            if prepared_activation_id != expected_identity:
                raise HeadlessApiError(
                    "activation_preparer_response_invalid",
                    "Activation preparer returned a false content identity",
                )
            if not isinstance(value.get("activation_payload"), dict):
                raise HeadlessApiError(
                    "activation_preparer_response_invalid",
                    "Activation preparer returned an invalid direct payload",
                )
            _require_admitted_program(value)
            receipt = {field: value[field] for field in _RECEIPT_FIELDS}
            self._prepared = {prepared_activation_id: receipt}
            return deepcopy(receipt)
        finally:
            if input_path is not None:
                input_path.unlink(missing_ok=True)
            self._lock.release()
