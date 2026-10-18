"""Bounded worker for preparing one captured activation input."""

from __future__ import annotations

from copy import deepcopy
from functools import lru_cache
import json
import hashlib
import logging
import os
import re
from pathlib import Path
import stat
import subprocess
import tempfile
import threading
from typing import Any, Callable, Mapping

from pydantic import ValidationError

from .models import ManagerApplyPreparedActivation, PreparedSealedCanonicalEmptyOrigin

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


# The preparer prints only the compact receipt; the complete prepared value is
# read from the stored product it names.
_MAXIMUM_COMPILER_OUTPUT_BYTES = 1024 * 1024
_MAXIMUM_COMPILER_ERROR_BYTES = 64 * 1024
# Must match kMaxPreparedActivationBytes in
# manyforge_deployment/prepared_activation_repository.hpp.
_MAXIMUM_STORED_PREPARED_ACTIVATION_BYTES = 128 * 1024 * 1024
_PREPARED_ACTIVATION_ID = re.compile(r"sha256:([0-9a-f]{64})")
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
    "origin",
    "schema_version",
}
_EXPECTED_RESULT_FIELDS = _RECEIPT_FIELDS | {"activation_payload"}

_PROFILE_DESCRIPTION = Path(
    "manyforge_deployment/resources/maintained/"
    "maintained_deployment_sources.v1.json"
)


_MODEL_FREE_IO_PROFILE = {
    "runtime_restoration_profile_id": "io_device_v0",
    "io_provider_runtime": {},
    "source_fields": {},
    "stop_profiles": {},
    "acceptance_obligations": [],
}


def _model_preparation_profiles(
    capability_root: Path,
) -> dict[str, tuple[str, str] | None]:
    """Read the minimum repository-owned profile facts needed by Python XML prep.

    Native preparation independently validates the complete profile and intent
    join.  This projection only supplies the model identity and accepted package
    prefix required to rewrite the already captured source bytes.  The approved
    model-free I/O profile (runtime kind ``io_provider_runtime``) maps to
    ``None``: it has no model to prepare.  Only its exact approved row is
    accepted; every other row must name a known model family.
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
    profiles: dict[str, tuple[str, str] | None] = {}
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
        model_free = isinstance(row, dict) and "io_provider_runtime" in row
        family_id = row.get("model_family_id") if isinstance(row, dict) else None
        family = families.get(family_id) if isinstance(family_id, str) else None
        if (
            not isinstance(profile_id, str)
            or not profile_id
            or (row != _MODEL_FREE_IO_PROFILE if model_free else family is None)
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


def _read_stored_prepared_activation(
    activation_root: Path, prepared_activation_id: str
) -> bytes:
    """Read the exact immutable product the native preparer published.

    The repository layout is ``prepared-activations/<hex>.json`` below the
    trusted activation root (``FilesystemPreparedActivationRepository``).
    """

    match = _PREPARED_ACTIVATION_ID.fullmatch(prepared_activation_id)
    if match is None:
        raise HeadlessApiError(
            "activation_preparer_response_invalid",
            "Activation preparer returned an invalid prepared identity",
        )
    path = activation_root / "prepared-activations" / f"{match.group(1)}.json"
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    except OSError as error:
        raise HeadlessApiError(
            "activation_preparer_response_invalid",
            "Activation preparer named no stored prepared activation",
        ) from error
    try:
        with os.fdopen(descriptor, "rb") as stream:
            status = os.fstat(stream.fileno())
            if (
                not stat.S_ISREG(status.st_mode)
                or status.st_size > _MAXIMUM_STORED_PREPARED_ACTIVATION_BYTES
            ):
                raise HeadlessApiError(
                    "activation_preparer_response_invalid",
                    "Stored prepared activation violates its file bound",
                )
            stored = stream.read(_MAXIMUM_STORED_PREPARED_ACTIVATION_BYTES + 1)
    except OSError as error:
        raise HeadlessApiError(
            "activation_preparer_response_invalid",
            "Stored prepared activation could not be read",
        ) from error
    if len(stored) != status.st_size:
        raise HeadlessApiError(
            "activation_preparer_response_invalid",
            "Stored prepared activation changed during its exact read",
        )
    return stored


def _require_stored_product(
    receipt: Mapping[str, Any], stored: bytes
) -> dict[str, Any]:
    """Bind the compact receipt to the stored product and verify its identity.

    The stored bytes must be exactly the RFC 8785 form of their value, as the
    Manager's repository reader requires; the value must carry exactly the
    receipt plus its payload, and its RFC 8785 content identity must be the
    receipt's prepared_activation_id.
    """

    try:
        value = json.loads(
            stored.decode("utf-8", errors="strict"),
        )
        canonical = rfc8785.dumps(value)
    except (UnicodeError, ValueError, rfc8785.CanonicalizationError) as error:
        raise HeadlessApiError(
            "activation_preparer_response_invalid",
            "Stored prepared activation is not valid canonical JSON",
        ) from error
    if canonical != stored:
        raise HeadlessApiError(
            "activation_preparer_response_invalid",
            "Stored prepared activation is not in its canonical form",
        )
    if (
        not isinstance(value, dict)
        or set(value) != _EXPECTED_RESULT_FIELDS
        or any(value[field] != receipt[field] for field in _RECEIPT_FIELDS)
    ):
        raise HeadlessApiError(
            "activation_preparer_response_invalid",
            "Stored prepared activation differs from the preparer receipt",
        )
    if receipt["origin"]["kind"] == "sealed_canonical_empty":
        try:
            ManagerApplyPreparedActivation.model_validate(dict(receipt))
        except ValidationError as error:
            raise HeadlessApiError(
                "activation_preparer_response_invalid",
                "Activation preparer returned an invalid sealed origin or receipt",
            ) from error
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
            "Stored prepared activation has an invalid payload",
        )
    if value["origin"]["kind"] == "sealed_canonical_empty":
        payload = value["activation_payload"]
        if (payload.get("program") is not None
                or "program_binding" not in payload or payload["program_binding"] is not None
                or payload.get("program_role_assignment") != {"component_roles": [], "target_roles": []}):
            raise HeadlessApiError(
                "activation_preparer_response_invalid",
                "Sealed empty preparation returned fabricated Program facts",
            )
    return value


def _prepare_model_closure(
    *,
    deployment: Mapping[str, Any],
    model_source_closure: Mapping[str, Any],
    profiles: Mapping[str, tuple[str, str] | None],
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
    consumed_sources: set[str] = set()
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
            known_profile = isinstance(profile_id, str) and profile_id in profiles
            profile = profiles.get(profile_id) if known_profile else None
            source = (
                source_by_component.get(component_id)
                if isinstance(component_id, str)
                else None
            )
            if (
                not isinstance(component_id, str)
                or not component_id
                or component_id in seen_components
                or not known_profile
            ):
                raise HeadlessApiError(
                    "activation_model_profile_invalid",
                    "Deployment components do not match captured model profiles",
                )
            seen_components.add(component_id)
            if profile is None:
                # The registered model-free I/O profile: only an io_provider
                # component with no model source and no captured row has no XML.
                configuration = component.get("configuration")
                if (
                    not isinstance(configuration, dict)
                    or configuration.get("runtime_kind") != "io_provider"
                    or "model_source" in component
                    or source is not None
                ):
                    raise HeadlessApiError(
                        "activation_model_profile_invalid",
                        "Deployment components do not match captured model profiles",
                    )
                continue
            if source is None:
                raise HeadlessApiError(
                    "activation_model_profile_invalid",
                    "Deployment components do not match captured model profiles",
                )
            consumed_sources.add(component_id)
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
    if consumed_sources != set(source_by_component):
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
        canonical_empty_package_root: Path | None = None,
        canonical_empty_admission_filename: Path | None = None,
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
        self._canonical_empty_package_root = canonical_empty_package_root
        self._canonical_empty_admission_filename = canonical_empty_admission_filename
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
        origin = {"kind": "scenario", "scenario_id": scenario_id,
                  "source_document_digest": source_document_digest}

        def request_value() -> dict[str, Any]:
            prepared_model_closure = _prepare_model_closure(
                deployment=deployment, model_source_closure=model_source_closure,
                profiles=self._profiles,
            )
            return {
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

        return self._prepare(request_value, origin, captured_input_digest)

    def prepare_canonical_empty(self, selection: Mapping[str, Any]) -> dict[str, Any]:
        """Prepare the actual sealed selection through the same bounded worker."""
        if (self._canonical_empty_package_root is None
                or self._canonical_empty_admission_filename is None):
            raise HeadlessApiError(
                "canonical_empty_preparation_unavailable",
                "Canonical-empty preparation requires the configured Manager admission",
            )
        origin = PreparedSealedCanonicalEmptyOrigin.model_validate({
            "kind": "sealed_canonical_empty", "cell_id": selection["cell_id"],
            "deployment_artifact_id": selection["deployment_artifact_id"],
            "deployment_revision": selection["deployment_revision"],
            "retained_bundle_id": selection["candidate_retained_bundle_id"],
            "graph_revision": selection["candidate_graph_revision"],
            "admission_digest": selection["admission_digest"],
        }).model_dump(mode="json")
        if origin["cell_id"] != self._runtime_cell_id:
            raise HeadlessApiError("stale_generation", "Sealed admission names a different runtime cell")
        captured = "sha256:" + hashlib.sha256(rfc8785.dumps({
            "schema_version": "manyforge.sealed_canonical_empty_capture.v1",
            "origin": origin,
        })).hexdigest()
        return self._prepare(lambda: {
            "schema_version": "manyforge.sealed_canonical_empty_preparation_request.v1",
            "runtime_cell_id": self._runtime_cell_id, "origin": origin,
            "captured_input_digest": captured,
        }, origin, captured, sealed=True)

    def _prepare(
        self, request_value: Callable[[], dict[str, Any]], origin: dict[str, Any],
        captured_input_digest: str, *, sealed: bool = False,
    ) -> dict[str, Any]:
        if not self._lock.acquire(blocking=False):
            raise HeadlessApiError(
                "activation_preparer_busy",
                "another activation input is already being prepared",
            )
        input_path: Path | None = None
        try:
            request = request_value()
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
            if sealed:
                command.extend([
                    "--canonical-empty-package-root", str(self._canonical_empty_package_root),
                    "--canonical-empty-admission-filename", str(self._canonical_empty_admission_filename),
                ])
            elif self._publish_canonical_empty:
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
                    "Activation preparation failed: runtime=%s origin_kind=%s origin_id=%s input=%s detail=%s",
                    self._runtime_cell_id,
                    origin["kind"],
                    origin.get("scenario_id", origin.get("cell_id")),
                    captured_input_digest,
                    detail[:4096],
                )
                raise HeadlessApiError(
                    "activation_preparation_failed",
                    "Activation preparation failed",
                    details={"preparer_detail": detail[:4096]},
                )
            try:
                receipt = json.loads(output.decode("utf-8", errors="strict"))
            except (UnicodeError, ValueError) as error:
                raise HeadlessApiError(
                    "activation_preparer_response_invalid",
                    "Activation preparer returned invalid JSON",
                ) from error
            if (
                not isinstance(receipt, dict)
                or set(receipt) != _RECEIPT_FIELDS
                or receipt.get("schema_version")
                != "manyforge.prepared_activation_input.v6"
                or receipt.get("origin") != origin
                or receipt.get("captured_input_digest") != captured_input_digest
                or not isinstance(receipt.get("prepared_activation_id"), str)
            ):
                raise HeadlessApiError(
                    "activation_preparer_response_invalid",
                    "Activation preparer returned a divergent result",
                )
            value = _require_stored_product(
                receipt,
                _read_stored_prepared_activation(
                    self._activation_root, receipt["prepared_activation_id"]
                ),
            )
            _require_admitted_program(value)
            self._prepared = {receipt["prepared_activation_id"]: receipt}
            return deepcopy(receipt)
        finally:
            if input_path is not None:
                input_path.unlink(missing_ok=True)
            self._lock.release()
