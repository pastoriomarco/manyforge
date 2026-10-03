#!/usr/bin/env python3
"""Render the bounded Program-Binding transport validators from one descriptor."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
DESCRIPTOR = ROOT / "manyforge_deployment/resources/program_binding_transport.v1.json"
FIXTURE_RELATIVE = Path(
    "manyforge_deployment/resources/program_binding_lite6_native.fixture.json"
)
OUTPUT_RELATIVES = (
    Path("manyforge_headless_api/program_binding_transport.py"),
    Path("manyforge_composer/frontend/src/types/programBindingTransport.generated.ts"),
    Path("manyforge_composer/frontend/src/api/programBindingTransport.generated.ts"),
)
TEMPLATES = {
    OUTPUT_RELATIVES[0]: Path(__file__).with_name("program_binding_transport.py.tmpl"),
    OUTPUT_RELATIVES[1]: Path(__file__).with_name("program_binding_transport_types.ts.tmpl"),
    OUTPUT_RELATIVES[2]: Path(__file__).with_name("program_binding_transport.ts.tmpl"),
}

EXPECTED_LIMITS = {
    "compiled_text_bytes": 4096,
    "identity_bytes": 128,
    "nested_rows": 4096,
    "node_policy_rows": 100000,
    "provenance_rows": 16384,
    "role_rows": 4096,
    "max_retries": 255,
}
EXPECTED_SCALARS = {
    "digest": {"kind": "string", "pattern": r"^sha256:[0-9a-f]{64}$"},
    "identity": {
        "kind": "string",
        "maximum_utf8_bytes": 128,
        "pattern": r"^[A-Za-z0-9][A-Za-z0-9._-]*$",
    },
    "text": {
        "kind": "string",
        "minimum_utf8_bytes": 1,
        "maximum_utf8_bytes": 4096,
    },
}
EXPECTED_ENUMS = {
    "operator_stop_policy": ["inherit", "hold", "continue"],
}
EXPECTED_RECORDS = {
    "binding": ["api_version", "program_binding_id", "program_id", "program_revision", "retained_bundle_id", "graph_revision", "node_environment", "component_role_bindings", "target_role_bindings", "node_policies", "contact_grasp_bindings", "provenance"],
    "node_environment": ["catalog_id", "content_version", "catalog_digest", "environment_digest"],
    "component": ["component_role_id", "controllable_component_id", "component_type_id", "component_type_revision", "model_role_bindings"],
    "model": ["model_role_id", "model_instance_id", "model_revision"],
    "target": ["target_role_id", "component_role_id", "controllable_component_id", "command_target_id", "frame_role_bindings", "resource_role_bindings", "collision_body_role_bindings", "capability_bindings"],
    "frame": ["frame_role_id", "model_instance_id", "model_revision", "compiled_frame_name"],
    "resource": ["resource_role_id", "resource_ids", "compiled_resource_names"],
    "collision": ["collision_body_role_id", "model_instance_id", "model_revision", "compiled_collision_body_names"],
    "capability": ["capability_id", "capability_definition_revision", "control_binding", "capability_provider_binding", "command_schema"],
    "node_policy": ["instance_id", "recovery_policy", "operator_stop_policy"],
    "recovery_policy": ["allow_intervention", "max_retries"],
    "control": ["control_binding_id", "control_binding_revision"],
    "provider": ["capability_provider_binding_id", "capability_provider_binding_revision"],
    "command_schema": ["command_schema_id", "command_schema_revision"],
    "contact": ["contact_policy_id", "contact_policy_revision", "runtime_object_id", "allowed_body_bindings", "max_penetration_m", "terminal_window_start_fraction"],
    "contact_body": ["component_role_id", "target_role_id", "controllable_component_id", "command_target_id", "model_role_id", "collision_body_role_id", "model_instance_id", "model_revision", "collision_body_ids"],
    "provenance": ["binding_compiler_contract_digest", "role_assignment_digest", "source_records", "resolved_output_records"],
    "provenance_record": ["record_kind", "record_id", "revision"],
}
EXPECTED_KINDS = [
    "model_asset", "runtime_profile", "component_type",
    "control_interface_contract", "controller_topology", "provider_manifest",
    "command_schema", "capability_definition", "contact_policy",
    "stop_evidence_policy",
]
EXPECTED_FORMULAS = {
    "collision_body_id": "component/{controllable_component_id}/model/{model_instance_id}/body/{collision_body_role_id}/collision/{index}",
}
EXPECTED_INVARIANTS = [
    "closed_records", "bounded_scalars", "bounded_tables",
    "canonical_sorted_unique",
    "node_policy_enums_and_cross_fields", "node_policy_sparse_rows",
    "resource_vector_correspondence",
    "resource_vector_independent_uniqueness", "collision_name_uniqueness",
    "capability_has_resolution", "target_component_correspondence",
    "finite_contact_ranges", "contact_target_collision_model_correspondence",
    "generated_collision_body_ids", "provenance_kind_and_tuple_order",
]

TYPE_NAMES = {
    "binding": "ProgramBindingDocument",
    "node_environment": "ProgramBindingNodeEnvironment",
    "component": "ProgramComponentRoleBinding",
    "model": "ProgramModelRoleBinding",
    "target": "ProgramTargetRoleBinding",
    "frame": "ProgramFrameRoleBinding",
    "resource": "ProgramResourceRoleBinding",
    "collision": "ProgramCollisionBodyRoleBinding",
    "capability": "ProgramCapabilityBinding",
    "node_policy": "ProgramNodePolicy",
    "recovery_policy": "ProgramNodeRecoveryPolicy",
    "control": "ProgramControlBindingRef",
    "provider": "ProgramCapabilityProviderBindingRef",
    "command_schema": "ProgramCommandSchemaRef",
    "contact": "ProgramContactGraspBinding",
    "contact_body": "ProgramContactGraspBodyBinding",
    "provenance": "ProgramBindingProvenance",
    "provenance_record": "ProgramBindingProvenanceRecord",
}
FIELD_TYPES = {
    ("binding", "api_version"): "{api_version!r}",
    ("binding", "node_environment"): "ProgramBindingNodeEnvironment",
    ("binding", "component_role_bindings"): "ProgramComponentRoleBinding[]",
    ("binding", "target_role_bindings"): "ProgramTargetRoleBinding[]",
    ("binding", "node_policies"): "ProgramNodePolicy[]",
    ("binding", "contact_grasp_bindings"): "ProgramContactGraspBinding[]",
    ("binding", "provenance"): "ProgramBindingProvenance",
    ("component", "model_role_bindings"): "ProgramModelRoleBinding[]",
    ("target", "frame_role_bindings"): "ProgramFrameRoleBinding[]",
    ("target", "resource_role_bindings"): "ProgramResourceRoleBinding[]",
    ("target", "collision_body_role_bindings"): "ProgramCollisionBodyRoleBinding[]",
    ("target", "capability_bindings"): "ProgramCapabilityBinding[]",
    ("resource", "resource_ids"): "string[]",
    ("resource", "compiled_resource_names"): "string[]",
    ("collision", "compiled_collision_body_names"): "string[]",
    ("capability", "control_binding"): "ProgramControlBindingRef | null",
    ("capability", "capability_provider_binding"): "ProgramCapabilityProviderBindingRef | null",
    ("capability", "command_schema"): "ProgramCommandSchemaRef | null",
    ("node_policy", "recovery_policy"): "ProgramNodeRecoveryPolicy",
    ("node_policy", "operator_stop_policy"): "ProgramOperatorStopPolicy",
    ("recovery_policy", "allow_intervention"): "boolean",
    ("recovery_policy", "max_retries"): "number",
    ("contact", "allowed_body_bindings"): "ProgramContactGraspBodyBinding[]",
    ("contact", "max_penetration_m"): "number",
    ("contact", "terminal_window_start_fraction"): "number",
    ("contact_body", "model_role_id"): "string | null",
    ("contact_body", "collision_body_ids"): "string[]",
    ("provenance", "source_records"): "ProgramBindingProvenanceRecord[]",
    ("provenance", "resolved_output_records"): "ProgramBindingProvenanceRecord[]",
    ("provenance_record", "record_kind"): "CatalogRecordKind",
}


def load_and_validate(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid descriptor JSON: {error}") from error
    expected_root = {
        "api_version", "binding_api_version", "limits", "scalars", "enums", "records",
        "catalog_record_kinds", "formulas", "invariants",
    }
    if not isinstance(value, dict) or set(value) != expected_root:
        raise ValueError("descriptor root is not the closed v1 record")
    if value["api_version"] != "manyforge.program_binding_transport_descriptor.v1":
        raise ValueError("descriptor API version is invalid")
    if value["binding_api_version"] != "manyforge.program_binding.v2":
        raise ValueError("Binding API version is invalid")
    exact = (
        ("limits", EXPECTED_LIMITS),
        ("scalars", EXPECTED_SCALARS),
        ("enums", EXPECTED_ENUMS),
        ("records", EXPECTED_RECORDS),
        ("catalog_record_kinds", EXPECTED_KINDS),
        ("formulas", EXPECTED_FORMULAS),
        ("invariants", EXPECTED_INVARIANTS),
    )
    for name, expected in exact:
        if value[name] != expected:
            raise ValueError(f"descriptor {name} differ from native v1 contract")
    return value


def _py_constants(descriptor: dict[str, Any]) -> str:
    scalars = descriptor["scalars"]
    limits = descriptor["limits"]
    lines = [
        f'_DIGEST = re.compile(r"{scalars["digest"]["pattern"][1:-1]}\\Z")',
        f'_IDENTITY = re.compile(r"{scalars["identity"]["pattern"][1:-1]}\\Z")',
        "_KINDS = frozenset({",
        *[
            "    " + ", ".join(f'"{kind}"' for kind in group) + ","
            for group in (
                descriptor["catalog_record_kinds"][0:3],
                descriptor["catalog_record_kinds"][3:6],
                descriptor["catalog_record_kinds"][6:9],
                descriptor["catalog_record_kinds"][9:],
            )
        ],
        "})",
        f'_MAX = {limits["nested_rows"]}',
        f'_ROLE_MAX = {limits["role_rows"]}',
        f'_NODE_POLICY_MAX = {limits["node_policy_rows"]}',
        f'_RETRY_MAX = {limits["max_retries"]}',
        f'_PROVENANCE_MAX = {limits["provenance_rows"]}',
        f'_TEXT_MAX = {limits["compiled_text_bytes"]}',
        f'_IDENTITY_MAX = {limits["identity_bytes"]}',
        f'_BINDING_API_VERSION = "{descriptor["binding_api_version"]}"',
        "_FIELDS = {",
    ]
    for name, fields in descriptor["records"].items():
        entries = ", ".join(f'"{field}"' for field in fields)
        lines.append(f'    "{name}": ({entries}),')
    lines.extend([
        "}",
        "_STOP_POLICIES = frozenset(" +
        repr(tuple(descriptor["enums"]["operator_stop_policy"])) + ")",
        '_PROGRAM_ID = re.compile(r"^[a-z][a-z0-9_.-]*\\Z")',
        f'_COLLISION_BODY_ID = "{descriptor["formulas"]["collision_body_id"]}"',
    ])
    return "\n".join(lines)


def _ts_constants(descriptor: dict[str, Any]) -> str:
    scalars = descriptor["scalars"]
    lines = [
        f'const DIGEST = /{scalars["digest"]["pattern"]}/;',
        f'const IDENTITY = /{scalars["identity"]["pattern"]}/;',
        "const KINDS = new Set<CatalogRecordKind>([",
        *[
            "  " + ", ".join(repr(kind) for kind in group) + ","
            for group in (
                descriptor["catalog_record_kinds"][0:3],
                descriptor["catalog_record_kinds"][3:6],
                descriptor["catalog_record_kinds"][6:9],
                descriptor["catalog_record_kinds"][9:],
            )
        ],
        "]);",
        "const FIELDS = {",
    ]
    for name, fields in descriptor["records"].items():
        lines.append(f"  {name}: [{', '.join(repr(field) for field in fields)}],")
    lines.extend([
        "} as const;",
        "const STOP_POLICIES = new Set(" +
        repr(descriptor["enums"]["operator_stop_policy"]) + ");",
        f"const COLLISION_BODY_ID = {descriptor['formulas']['collision_body_id']!r};",
        f'const MAX_ROWS = {descriptor["limits"]["nested_rows"]};',
        f'const ROLE_MAX_ROWS = {descriptor["limits"]["role_rows"]};',
        f'const NODE_POLICY_MAX_ROWS = {descriptor["limits"]["node_policy_rows"]};',
        f'const RETRY_MAX = {descriptor["limits"]["max_retries"]};',
        f'const PROVENANCE_MAX_ROWS = {descriptor["limits"]["provenance_rows"]};',
        f'const TEXT_MAX_BYTES = {descriptor["limits"]["compiled_text_bytes"]};',
        f'const IDENTITY_MAX_BYTES = {descriptor["limits"]["identity_bytes"]};',
        f"const BINDING_API_VERSION = {descriptor['binding_api_version']!r};",
    ])
    return "\n".join(lines)


def _type_interfaces(descriptor: dict[str, Any]) -> str:
    kinds = " | ".join(repr(kind) for kind in descriptor["catalog_record_kinds"])
    sections = [
        f"export type CatalogRecordKind = {kinds};",
        "export type ProgramOperatorStopPolicy = " +
        " | ".join(repr(value) for value in descriptor["enums"]["operator_stop_policy"]) + ";",
    ]
    api_version = descriptor["binding_api_version"]
    for record_name, fields in descriptor["records"].items():
        sections.append(f"export interface {TYPE_NAMES[record_name]} {{")
        for field in fields:
            field_type = FIELD_TYPES.get((record_name, field), "string")
            sections.append(f"  {field}: {field_type.format(api_version=api_version)};")
        sections.append("}")
    return "\n".join(sections)


def render(descriptor: dict[str, Any]) -> dict[Path, bytes]:
    replacements = {
        "@@PY_CONSTANTS@@": _py_constants(descriptor),
        "@@TS_CONSTANTS@@": _ts_constants(descriptor),
        "@@TYPE_INTERFACES@@": _type_interfaces(descriptor),
    }
    rendered: dict[Path, bytes] = {}
    for relative, template_path in TEMPLATES.items():
        text = template_path.read_text(encoding="utf-8")
        for marker, replacement in replacements.items():
            text = text.replace(marker, replacement)
        if "@@" in text:
            raise ValueError(f"{template_path}: unexpanded generator marker")
        rendered[relative] = text.encode("utf-8")
    return rendered


def _validate_fixture(source: bytes) -> None:
    try:
        value = json.loads(source)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError("fixture source is not canonical JSON") from error
    if (
        not isinstance(value, dict)
        or value.get("api_version") != "manyforge.program_binding.v2"
        or not value.get("component_role_bindings")
        or source
        != json.dumps(
            value, ensure_ascii=False, separators=(",", ":"), sort_keys=True
        ).encode()
    ):
        raise ValueError("fixture source is not a canonical nonempty Program Binding")
    # Fixture refresh is a maintainer-time operation. Reuse the generated
    # bounded validator rather than maintaining a second fixture validator.
    sys.path.insert(0, str(ROOT))
    from manyforge_headless_api.program_binding_transport import (  # noqa: PLC0415
        ProgramBindingTransportError,
        validate_program_binding,
    )

    try:
        validate_program_binding(value, label="fixture_source")
    except ProgramBindingTransportError as error:
        raise ValueError(f"fixture source is invalid: {error}") from error


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--descriptor", type=Path, default=DESCRIPTOR)
    parser.add_argument("--output-root", type=Path, default=ROOT)
    parser.add_argument("--fixture-source", type=Path)
    args = parser.parse_args()
    descriptor = load_and_validate(args.descriptor)
    for relative, expected in render(descriptor).items():
        output = args.output_root / relative
        if args.check:
            if not output.is_file() or output.read_bytes() != expected:
                raise ValueError(f"{output}: generated output is stale")
        else:
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(expected)
    if args.fixture_source is not None:
        source = args.fixture_source.read_bytes()
        _validate_fixture(source)
        fixture = args.output_root / FIXTURE_RELATIVE
        if args.check:
            if not fixture.is_file() or fixture.read_bytes() != source:
                raise ValueError("checked-in Lite6 Program-Binding fixture is stale")
        else:
            fixture.parent.mkdir(parents=True, exist_ok=True)
            fixture.write_bytes(source)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
