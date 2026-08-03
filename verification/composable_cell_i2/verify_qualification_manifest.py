#!/usr/bin/env python3
"""Validate and execute the frozen Composable Cell I2 qualification manifest."""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import posixpath
import re
import shlex
import stat
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections import Counter
from datetime import UTC, datetime
from pathlib import Path, PurePosixPath
from typing import Any, NoReturn

try:
    from .native_build_provenance import (
        NativeBuildProvenanceError,
        compile_dependency_cross_profile_sha256,
        generated_output_cross_profile_sha256,
        replay_configured_target_graph,
        replay_git_source_generation,
        validate_build_provenance_contract,
        validate_binding_build_provenance,
        validate_native_build_provenance,
        validate_production_build_provenance,
    )
except ImportError:
    from native_build_provenance import (  # type: ignore[no-redef]
        NativeBuildProvenanceError,
        compile_dependency_cross_profile_sha256,
        generated_output_cross_profile_sha256,
        replay_configured_target_graph,
        replay_git_source_generation,
        validate_build_provenance_contract,
        validate_binding_build_provenance,
        validate_native_build_provenance,
        validate_production_build_provenance,
    )


SCHEMA = "manyforge.composable_cell_i2.qualification_manifest.v1"
RESULT_SCHEMA = "manyforge.composable_cell_i2.qualification_result.v1"
REVIEW_SCHEMA = "manyforge.composable_cell_i2.adversarial_review.v1"
CLOSURE_SCHEMA = "manyforge.composable_cell_i2.qualification_closure.v1"
SOURCE_IDENTITY_SCHEMA = "manyforge.source_identity.v1"
FIXTURE_IR_WITNESS_SCHEMA = "manyforge.composable_cell_i2.fixture_ir_witness.v1"
OUTPUT_ROOT = Path(".manyforge-var/qualification/composable-cell-i2")
GRAPHVIZ_OPTIONS = b"""set(GRAPHVIZ_GRAPH_NAME \"manyforge_i2\")
set(GRAPHVIZ_GRAPH_HEADER \"node [ fontsize = \\\"12\\\" ];\")
set(GRAPHVIZ_NODE_PREFIX \"i2node\")
set(GRAPHVIZ_EXECUTABLES TRUE)
set(GRAPHVIZ_STATIC_LIBS TRUE)
set(GRAPHVIZ_SHARED_LIBS TRUE)
set(GRAPHVIZ_MODULE_LIBS TRUE)
set(GRAPHVIZ_INTERFACE_LIBS TRUE)
set(GRAPHVIZ_OBJECT_LIBS TRUE)
set(GRAPHVIZ_UNKNOWN_LIBS TRUE)
set(GRAPHVIZ_EXTERNAL_LIBS TRUE)
set(GRAPHVIZ_CUSTOM_TARGETS TRUE)
set(GRAPHVIZ_IGNORE_TARGETS \"\")
set(GRAPHVIZ_GENERATE_PER_TARGET FALSE)
set(GRAPHVIZ_GENERATE_DEPENDERS FALSE)
"""
PINNED_CONTAINER_REPLAY_REQUEST_SCHEMA = (
    "manyforge.composable_cell_i2.pinned_container_replay_request.v1"
)
PINNED_CONTAINER_REPLAY_RESPONSE_SCHEMA = (
    "manyforge.composable_cell_i2.pinned_container_replay_response.v1"
)
PINNED_CONTAINER_REPLAY_RECEIPT_SCHEMA = (
    "manyforge.composable_cell_i2.pinned_container_replay_receipt.v1"
)
PINNED_CONTAINER_CLAIM_KINDS = (
    "build_tool",
    "command_input",
    "compile_dependency",
    "elf_dependency",
    "inspection_tool",
    "link_command_input",
    "transitive_link_input",
)
PINNED_CONTAINER_FULL_CHAIN_KINDS = frozenset(
    {
        "command_input",
        "elf_dependency",
        "link_command_input",
        "transitive_link_input",
    }
)
PINNED_CONTAINER_LIBRARY_ROOTS = (
    "/lib",
    "/lib64",
    "/usr/lib",
    "/usr/lib64",
    "/usr/local/lib",
    "/usr/local/lib64",
)
PINNED_CONTAINER_INSPECTION_TOOLS = (
    "/usr/bin/ar",
    "/usr/bin/ldd",
    "/usr/bin/readelf",
)
PINNED_CONTAINER_BUILD_TOOLS = (
    "/usr/bin/ar",
    "/usr/bin/c++",
    "/usr/bin/cmake",
    "/usr/bin/git",
    "/usr/bin/ninja",
    "/usr/bin/ranlib",
    "/usr/local/bin/python3",
)
PROJECT_BUILD_TOOL_FIELDS = (
    ("cmake", "cmake", "version_first_line", None),
    ("git", "git", "version_first_line", None),
    ("python", "python", "version_first_line", None),
    ("ninja", "ninja", "version", "1.11.1"),
    ("compiler", "cxx_compiler", "version_first_line", None),
    ("archiver", "archiver", None, None),
    ("ranlib", "ranlib", None, None),
)
PINNED_CONTAINER_COMPLETE_MINIMUM_UNIQUE_CLAIMS = {
    "build_tool": len(PINNED_CONTAINER_BUILD_TOOLS),
    "command_input": 0,
    "compile_dependency": 1,
    "elf_dependency": 1,
    "inspection_tool": len(PINNED_CONTAINER_INSPECTION_TOOLS),
    "link_command_input": 1,
    "transitive_link_input": 1,
}
PINNED_CONTAINER_REPLAY_MAX_CLAIMS = 100_000
PINNED_CONTAINER_REPLAY_MAX_REQUEST_BYTES = 32 * 1024 * 1024
PINNED_CONTAINER_REPLAY_TIMEOUT_SECONDS = 120
PINNED_CONTAINER_REPLAY_PROGRAM = r'''import hashlib
import json
import os
import posixpath
import stat
import sys

REQUEST_SCHEMA = "manyforge.composable_cell_i2.pinned_container_replay_request.v1"
RESPONSE_SCHEMA = "manyforge.composable_cell_i2.pinned_container_replay_response.v1"
KINDS = {
    "build_tool",
    "command_input",
    "compile_dependency",
    "elf_dependency",
    "inspection_tool",
    "link_command_input",
    "transitive_link_input",
}
FULL_CHAIN_KINDS = {
    "command_input",
    "elf_dependency",
    "link_command_input",
    "transitive_link_input",
}
LINK_ROOTS = (
    "/lib",
    "/lib64",
    "/usr/lib",
    "/usr/lib64",
    "/usr/local/lib",
    "/usr/local/lib64",
)
INSPECTION_TOOLS = {"/usr/bin/ar", "/usr/bin/ldd", "/usr/bin/readelf"}
BUILD_TOOLS = {
    "/usr/bin/ar",
    "/usr/bin/c++",
    "/usr/bin/cmake",
    "/usr/bin/git",
    "/usr/bin/ninja",
    "/usr/bin/ranlib",
    "/usr/local/bin/python3",
}
MAX_CLAIMS = 100000
MAX_REQUEST_BYTES = 32 * 1024 * 1024


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def within(path, root):
    return path == root or path.startswith(root + "/")


def canonical_absolute(path):
    return (
        isinstance(path, str)
        and path.startswith("/")
        and not path.startswith("//")
        and "\\" not in path
        and posixpath.normpath(path) == path
    )


def regular_metadata(path, *, require_single_link):
    metadata = os.stat(path)
    if not stat.S_ISREG(metadata.st_mode):
        raise ValueError("claim does not resolve to a regular file")
    if require_single_link and metadata.st_nlink != 1:
        raise ValueError("claim resolves to an unqualified hardlink")
    return metadata


def hash_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def full_symlink_chain(path):
    current = path
    seen = set()
    chain = []
    for _ in range(32):
        metadata = os.lstat(current)
        chain.append(current)
        if not stat.S_ISLNK(metadata.st_mode):
            resolved = os.path.realpath(current, strict=True)
            final_metadata = regular_metadata(resolved, require_single_link=True)
            return resolved, chain, final_metadata
        if current in seen:
            raise ValueError("claim contains a symlink cycle")
        seen.add(current)
        target = os.readlink(current)
        current = posixpath.normpath(
            target
            if target.startswith("/")
            else posixpath.join(posixpath.dirname(current), target)
        )
        if not canonical_absolute(current):
            raise ValueError("claim symlink target is not canonical and absolute")
    raise ValueError("claim exceeds the symlink-depth bound")


def endpoint(path, *, require_single_link):
    os.lstat(path)
    resolved = os.path.realpath(path, strict=True)
    metadata = regular_metadata(resolved, require_single_link=require_single_link)
    chain = []
    for value in (path, resolved):
        if value not in chain:
            chain.append(value)
    return resolved, chain, metadata


raw = sys.stdin.buffer.read(MAX_REQUEST_BYTES + 1)
if len(raw) > MAX_REQUEST_BYTES:
    raise ValueError("replay request exceeds its byte bound")
request = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object)
if not isinstance(request, dict) or set(request) != {"schema_version", "claims"}:
    raise ValueError("replay request envelope drifted")
if request["schema_version"] != REQUEST_SCHEMA or not isinstance(request["claims"], list):
    raise ValueError("replay request schema drifted")
claims = request["claims"]
if len(claims) > MAX_CLAIMS:
    raise ValueError("replay request exceeds its claim bound")
expected_order = []
for claim in claims:
    if not isinstance(claim, dict) or set(claim) != {"claim_kind", "path"}:
        raise ValueError("replay claim shape drifted")
    kind = claim["claim_kind"]
    path = claim["path"]
    if kind not in KINDS or not canonical_absolute(path):
        raise ValueError("replay claim identity is malformed")
    expected_order.append((kind, path))
if expected_order != sorted(set(expected_order)):
    raise ValueError("replay claims are not exact, unique, and sorted")

files = []
for kind, path in expected_order:
    content_format = None
    if kind in FULL_CHAIN_KINDS:
        if not any(within(path, root) for root in LINK_ROOTS):
            raise ValueError("full-chain claim escaped system library roots")
        resolved, chain, metadata = full_symlink_chain(path)
        if not any(within(resolved, root) for root in LINK_ROOTS) or any(
            not any(within(item, root) for root in LINK_ROOTS) for item in chain
        ):
            raise ValueError("full-chain claim escaped system library roots")
    elif kind == "compile_dependency":
        if not (within(path, "/usr") or within(path, "/lib")):
            raise ValueError("compiler dependency escaped pinned system roots")
        resolved, chain, metadata = endpoint(path, require_single_link=False)
        if not (within(resolved, "/usr") or within(resolved, "/lib")):
            raise ValueError("compiler dependency escaped pinned system roots")
    elif kind == "inspection_tool":
        if path not in INSPECTION_TOOLS:
            raise ValueError("inspection-tool claim escaped its exact catalog")
        resolved, chain, metadata = endpoint(path, require_single_link=True)
    else:
        if path not in BUILD_TOOLS:
            raise ValueError("build-tool claim escaped its exact catalog")
        resolved, chain, metadata = endpoint(path, require_single_link=True)
        if not os.access(resolved, os.X_OK):
            raise ValueError("build-tool claim is not executable")
    if kind in {"elf_dependency", "link_command_input", "transitive_link_input"}:
        with open(resolved, "rb") as source:
            magic = source.read(8)
        if kind != "elf_dependency" and path.endswith(".a"):
            if magic != b"!<arch>\n":
                raise ValueError("link archive input is thin or has invalid magic")
            content_format = "archive"
        else:
            if magic[:4] != b"\x7fELF":
                raise ValueError("non-archive link input is not ELF")
            content_format = "elf"
    files.append(
        {
            "claim_kind": kind,
            "content_format": content_format,
            "path": path,
            "resolved_path": resolved,
            "symlink_chain": chain,
            "sha256": hash_file(resolved),
            "size": metadata.st_size,
        }
    )
sys.stdout.write(
    json.dumps(
        {"schema_version": RESPONSE_SCHEMA, "files": files},
        sort_keys=True,
        separators=(",", ":"),
    )
)
'''
TOP_LEVEL_KEYS = {
    "schema_version",
    "status",
    "implementation_base",
    "historical_packet_lineage",
    "candidate_identity",
    "qualification_environment",
    "binding_contract",
    "test_inventory_contract",
    "pinned_inputs",
    "rotating_identities",
    "fixture_matrix",
    "qualification_lanes",
    "artifact_dependency_graph",
    "forbidden_scans",
    "deferred_requirements",
    "historical_non_satisfying_inputs",
    "evidence_contract",
}
QUALIFICATION_STATUS = "active_i2_qualification_contract"
HISTORICAL_PACKET_COMMIT = "c860246d377f63e1937922ab7611ec77aa18331e"
HISTORICAL_PACKET_TREE = "3af67641352edec310c6f2aea8b0784c5b269993"
HISTORICAL_NEW_PACKET_PATHS = {
    "scripts/ci/run-composable-cell-i2-core-asan.sh",
    "scripts/ci/run-composable-cell-i2-core-tsan.sh",
    "scripts/ci/run-composable-cell-i2-deployment-asan.sh",
    "scripts/ci/run-composable-cell-i2-native-evidence.py",
    "scripts/ci/run-composable-cell-i2-qualification.sh",
    "scripts/ci/run-composable-cell-i2-roboplan-scene-sanitizer.sh",
    "tests/test_composable_cell_i2_build_provenance.py",
    "tests/test_composable_cell_i2_qualification_manifest.py",
    "verification/composable_cell_i2/README.md",
    "verification/composable_cell_i2/adversarial_review_template.v1.json",
    "verification/composable_cell_i2/baseline_test_inventory.v1.json",
    "verification/composable_cell_i2/binding_contract_tests.py",
    "verification/composable_cell_i2/build_provenance_contract.v1.json",
    "verification/composable_cell_i2/native_build_provenance.py",
    "verification/composable_cell_i2/qualification_manifest.v1.json",
    "verification/composable_cell_i2/native_target_contract.v1.json",
    "verification/composable_cell_i2/verify_qualification_manifest.py",
}
HISTORICAL_SUPPORT_PATHS = {
    "scripts/ci/tsan.supp",
    "scripts/qualification/source_identity.py",
}
REQUIRED_LANES = {
    "preflight",
    "core_release",
    "core_production_release",
    "deployment_release",
    "deployment_production_release",
    "c02_tail_focused_release",
    "roboplan_release",
    "roboplan_consumer_focused",
    "bindings_selector",
    "bindings_release",
    "core_asan_ubsan",
    "core_tsan",
    "deployment_asan_ubsan",
    "roboplan_asan_ubsan",
    "roboplan_tsan",
    "bindings_asan_ubsan",
    "bindings_tsan",
    "bindings_import",
    "bindings_parity",
    "bindings_lifetime",
    "bindings_exception",
    "bindings_gil_concurrency",
    "bindings_shutdown",
    "bindings_generated_api",
    "deterministic_build_a",
    "deterministic_build_b",
    "deterministic_rebuild",
    "forbidden_surface_scan",
    "manifest_contract_tests",
    "final_recheck",
}
LANE_READ_ONLY_INPUTS = {
    "core_production_release": ("core_release",),
    "deployment_production_release": ("deployment_release",),
    "bindings_import": ("bindings_release",),
    "bindings_parity": ("bindings_release",),
    "bindings_lifetime": ("bindings_release",),
    "bindings_exception": ("bindings_release",),
    "bindings_gil_concurrency": ("bindings_release",),
    "bindings_shutdown": ("bindings_release",),
    "bindings_generated_api": ("bindings_release",),
    "deterministic_rebuild": ("deterministic_build_a", "deterministic_build_b"),
}
REQUIRED_SCAN_IDS = {
    "stale_public_structure_header",
    "public_or_python_structural_stage",
    "deleted_roboplan_module",
    "trusted_graph_alias",
    "old_parsed_source_api",
    "old_experimental_import",
    "trusted_stage_python_constructor",
    "test_only_final_trust_bypass",
    "ros_or_backend_header_in_core_binding",
    "direct_graph_to_retention_bypass",
    "compatibility_fallback_or_alias",
}
FORBIDDEN_SCAN_OUTPUT = "forbidden-surface-scan.json"
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
IDENTIFIER = re.compile(r"^[a-z][a-z0-9_]*$")
RUN_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
CAPTURE_TOOL = Path("scripts/qualification/source_identity.py")
TRACKED_FILE_POLICIES = {
    "immutable_in_i2",
    "must_change_and_requalify",
    "rotate_and_requalify",
    "retain_behavior_and_record_final_digest",
}
REQUIRED_BINDING_CASES = {
    "ros_free_import_and_sole_module_inventory",
    "cpp_python_field_enum_default_ordering_and_error_parity",
    "final_compiler_projection_nonconstructible_and_read_only",
    "all_trusted_predecessor_projections_nonconstructible_and_read_only",
    "detached_predecessor_and_final_value_lifetime",
    "blocking_compile_releases_gil_with_progress_witness",
    "concurrent_pure_compiles_are_deterministic",
    "exception_translation_after_released_gil",
    "repeated_fresh_interpreter_import_use_and_clean_shutdown",
    "generated_stub_inventory_and_documentation_parity",
    "complete_forbidden_surface_absence",
}
REQUIRED_BINDING_WITNESS_MEASUREMENTS = {
    "ros_free_import_and_sole_module_inventory": [
        "imported_module_count",
        "forbidden_module_count",
        "ros_module_count",
        "extension_suffix_matches",
    ],
    "cpp_python_field_enum_default_ordering_and_error_parity": [
        "cpp_vector_sha256",
        "python_vector_sha256",
        "vectors_equal",
    ],
    "final_compiler_projection_nonconstructible_and_read_only": [
        "construct_attempts",
        "construct_rejections",
        "write_attempts",
        "write_rejections",
    ],
    "all_trusted_predecessor_projections_nonconstructible_and_read_only": [
        "projection_count",
        "construct_attempts",
        "construct_rejections",
        "write_attempts",
        "write_rejections",
    ],
    "detached_predecessor_and_final_value_lifetime": [
        "detached_projection_count",
        "post_owner_read_iterations",
        "failed_reads",
    ],
    "blocking_compile_releases_gil_with_progress_witness": [
        "compiler_calls",
        "worker_progress_events",
        "distinct_worker_thread",
    ],
    "concurrent_pure_compiles_are_deterministic": [
        "concurrent_calls",
        "unique_result_digests",
        "result_digest_sha256",
    ],
    "exception_translation_after_released_gil": [
        "translated_exception_count",
        "released_gil_failure_count",
        "exact_payload_match",
    ],
    "repeated_fresh_interpreter_import_use_and_clean_shutdown": [
        "fresh_interpreter_runs",
        "clean_exit_count",
        "nonzero_exit_count",
    ],
    "generated_stub_inventory_and_documentation_parity": [
        "generated_output_count",
        "api_export_count",
        "parity_mismatches",
    ],
    "complete_forbidden_surface_absence": [
        "forbidden_export_count",
        "forbidden_module_count",
        "forbidden_scan_match_count",
    ],
}
REQUIRED_BINDING_WITNESS_EVIDENCE_ROLES = {
    case_id: ["direct_case_evidence"] for case_id in REQUIRED_BINDING_CASES
}
BINDING_INTEGER_MEASUREMENTS = {
    "imported_module_count",
    "forbidden_module_count",
    "ros_module_count",
    "construct_attempts",
    "construct_rejections",
    "write_attempts",
    "write_rejections",
    "projection_count",
    "detached_projection_count",
    "post_owner_read_iterations",
    "failed_reads",
    "compiler_calls",
    "worker_progress_events",
    "concurrent_calls",
    "unique_result_digests",
    "translated_exception_count",
    "released_gil_failure_count",
    "fresh_interpreter_runs",
    "clean_exit_count",
    "nonzero_exit_count",
    "generated_output_count",
    "api_export_count",
    "parity_mismatches",
    "forbidden_export_count",
    "forbidden_scan_match_count",
}
REQUIRED_REGISTRATION_UNITS = {
    "manyforge_core/python/bindings.cpp",
    "manyforge_core/python/bindings_catalog.cpp",
    "manyforge_core/python/bindings_cell_graph.cpp",
    "manyforge_core/python/bindings_component_type.cpp",
    "manyforge_core/python/bindings_deployment.cpp",
    "manyforge_core/python/bindings_model_compiler.cpp",
    "manyforge_core/python/bindings_strong_identity.cpp",
    "manyforge_core/python/binding_registrations.hpp",
}
REQUIRED_EXISTING_BINDING_TESTS = {
    "manyforge_core/python/tests/test_bindings_smoke.py",
    "manyforge_core/python/tests/test_collision_bindings.py",
    "manyforge_core/python/tests/test_component_type_bindings.py",
    "manyforge_core/python/tests/test_composable_cell_bindings.py",
    "manyforge_core/python/tests/test_controller_topology_bindings.py",
    "manyforge_core/python/tests/test_model_asset_bindings.py",
    "manyforge_core/python/tests/test_model_compiler_bindings.py",
    "manyforge_core/python/tests/test_provider_manifest_bindings.py",
    "manyforge_core/python/tests/test_runtime_profile_bindings.py",
}
REQUIRED_BINDING_INTEGRATION_UPDATES = {
    "scripts/test-all.sh",
    ".github/workflows/python-tests.yml",
    "README.md",
}
REQUIRED_BINDING_SCRIPTS = {
    "scripts/ci/run-core-bindings.sh",
    "scripts/ci/run-core-bindings-sanitizer.sh",
    "scripts/ci/run-core-bindings-contract.sh",
    "scripts/ci/run-composable-cell-i2-determinism.sh",
}
REQUIRED_GENERATED_OUTPUTS = {
    "extension": "/qualification-output/generated/manyforge_core_py.cpython-312-x86_64-linux-gnu.so",
    "typed_stub": "/qualification-output/generated/manyforge_core_py.pyi",
    "api_inventory": "/qualification-output/generated/manyforge_core_py.api.json",
    "api_documentation": "/qualification-output/generated/manyforge_core_py-api.md",
}
GENERATED_OUTPUT_LANES = {
    "extension": "bindings_release",
    "typed_stub": "bindings_generated_api",
    "api_inventory": "bindings_generated_api",
    "api_documentation": "bindings_generated_api",
}
REQUIRED_NONCONSTRUCTIBLE_EXPORTS = {
    "GraphRevisionVerifiedCellGraph",
    "VerifiedModelSourceArtifact",
    "SourceVerifiedModelAsset",
    "CompositionQualifiedCellIr",
    "ApplicabilityQualifiedCellIr",
    "CompilerQualifiedCellIr",
}
FORBIDDEN_BINDING_EXPORTS = {
    "StructurallyQualifiedCellGraph",
    "TrustedCellGraph",
    "TrustedCellGraphCandidate",
    "ParsedModelSourceIr",
    "ParsedUrdfJoint",
    "ParsedUrdfJointType",
    "ParsedSrdfChain",
    "ParsedSrdfDisabledCollision",
    "ParsedSrdfEndEffector",
    "ParsedSrdfGroup",
    "ParsedSrdfGroupState",
    "ParsedSrdfGroupStateJoint",
}
REQUIRED_CONSUMER_TARGETS = {
    "generate_canonical_empty_product_package",
    "generate_lite6_retained_consumer_fixture",
    "generate_lite6_retained_consumer_product",
    "test_retained_product_consumer",
}
REQUIRED_CONSUMER_TESTS = {
    "RetainedProductRoboPlanConsumer.Mf2CrossBranchTraversalImportsDeeperMasterBeforeFollower",
    "RetainedProductRoboPlanConsumer.CanonicalEmptyExactBytesYieldDeterministicPlanningCollisionScene",
    "RetainedProductRoboPlanConsumer.DurableLite6ReopensStagesAndBuildsDeterministicNonEmptyScene",
    "retained_compiled_visual_fk_composer_consumer",
    "retained_compiled_model_independent_standard_parser",
}
REQUIRED_TRACKED_FILE_POLICIES = {
    "third_party/roboplan/README.md": "immutable_in_i2",
    "third_party/roboplan/patches.sha256": "immutable_in_i2",
    "manyforge_deployment/test/golden/canonical_empty_admission.release.json": "rotate_and_requalify",
    "manyforge_deployment/test/generate_lite6_retained_consumer_fixture.cpp": "retain_behavior_and_record_final_digest",
    "manyforge_deployment/test/qualify_retained_model_standard_parsers.py": "retain_behavior_and_record_final_digest",
    "manyforge_composer/tests/qualify_retained_compiled_visual_fk.py": "retain_behavior_and_record_final_digest",
    "manyforge_deployment/tools/prepare_structural_model.py": "immutable_in_i2",
    "manyforge_core/python/CMakeLists.txt": "must_change_and_requalify",
    "manyforge_roboplan/test/test_retained_product_consumer.cpp": "retain_behavior_and_record_final_digest",
    "scripts/qualification/source_identity.py": "immutable_in_i2",
}
REQUIRED_FIXTURE_IDS = {
    "c02a_l_cumulative_regression",
    "c02m_empty_passive_aggregate_attached_internal_and_mixed_vendor",
    "c02m_two_same_type_robot_and_tool_and_independent_attachments",
    "c02m_mobile_rail_gantry_floating_prismatic_and_mimic_coordinates",
    "c02m_mf2_rank_digest_collision_rewrite_and_shuffle_determinism",
    "c02m_sibling_cross_branch_safe_chain_descendant_and_cycle_mimic_cases",
    "c02m_parser_validity_and_pinocchio_child_order_assumption",
    "c02n_contract_route_activation_provider_stop_and_external_overlay_matrix",
    "c02n_65536_and_1000000_exact_bounds_plus_overflow",
    "c02n_site_overlay_presence_digest_values_and_secrets_absence",
    "c02o_single_final_operation_empty_nonempty_equality_and_nonforgeability",
    "python_ros_free_import_parity_errors_immutability_and_strong_ids",
    "python_lifetime_released_gil_concurrency_exception_and_shutdown",
    "stub_api_inventory_and_documentation_parity",
    "legacy_and_bypass_surface_deletion",
}
REQUIRED_ROTATING_BASELINE = {
    "node_environment_digest": "sha256:c1a6f5fcdcabc18c30ade4761a64ff7fd10dfbeb473514ccde3a69e3bd11dd6a",
    "compiler_contract_digest": "sha256:2197422750ff536028772ff44c555c4cc0725859fd6763236fe5ef14ba641fb5",
    "release_compiler_build_digest": "sha256:1a0004374353968bc4b0b1fb50229ab95c48790ddb4b58c35f5f5f39862c3d72",
    "canonical_empty_retained_bundle_id": "sha256:58bbf96c6990ca84bae1d1d5986323984704f0842334acd898d6cddb41f61858",
    "canonical_empty_admission_digest": "sha256:d8add4248a9089eb3c2b43d58df4ef46c7c0503c1ed8ff4eb415a252d5db9d69",
}
REQUIRED_REGRESSION_PRODUCTS = {
    "canonical_empty_product_package/canonical-empty-admission.json",
    "lite6_retained_consumer_fixture/manifest.json",
    "lite6_retained_consumer_fixture/retained-model.urdf",
    "lite6_retained_consumer_fixture/retained-model.srdf",
    "lite6_retained_consumer_fixture/retained-package/**/*",
    "deployment-build/generated/embedded_build_evidence.hpp",
    "deployment-build/generated/build_qualified_admission_profile_generated.hpp",
    "deployment-build/generated/closed_schema/**/*",
    "fixture-raw/**/*.json",
    "fixture-ir/*.json",
}
REQUIRED_IDENTITY_ARTIFACT = {
    "path": "canonical_empty_product_package/canonical-empty-admission.json",
    "field_map": {
        "node_environment_digest": "environment_digest",
        "compiler_contract_digest": "compiler_contract_digest",
        "release_compiler_build_digest": "compiler_build_digest",
        "canonical_empty_retained_bundle_id": "retained_bundle_id",
        "canonical_empty_admission_digest": "admission_digest",
    },
}
REQUIRED_FIXTURE_IR_OUTPUTS = {
    fixture_id: f"fixture-ir/{fixture_id}.json"
    for fixture_id in REQUIRED_FIXTURE_IDS
    if fixture_id.startswith(("c02m_", "c02n_", "c02o_"))
}
REQUIRED_DEFERRED = {
    ("ProgramBinding and Program execution bindings", "I6/N"),
    ("Apply request, replay and durable Manager observations", "I4/M0"),
    ("Lifecycle request and observation bindings", "I7/L0"),
    ("Runtime status, progress and provider-replacement lifetime", "owning I5-I11 runtime milestone"),
    ("Stop request, participant evidence and certificate bindings", "I8/P5"),
    ("RoboPlan 0.6.0 adoption or patch reduction", "E1 after I5/H0"),
    ("Alternative binding-framework evaluation", "separately approved post-I12 work"),
}
REQUIRED_HISTORICAL_COMMITS = {
    "C0.2m mf1 historical checkpoint": "9e41b30378959465640ae2e2239e83807000d864",
    "C0.2n historical checkpoint": "e0ede0aa64f5340ddad5ad33387d6d349ae2fc87",
    "C0.2o historical C++ checkpoint": "153195a692c50e606f50e40bd755da6081dd24c5",
    "E0 historical binding experiment": "637894c460172b47faf64c7ece2e865bb8d69593",
}
REQUIRED_HISTORICAL_IMAGE = "sha256:08863b21e0908f5400443eebc5c32a6412b356f34e6468799cd1e5b0490918ff"
BASELINE_TEST_INVENTORY_SCHEMA = "manyforge.composable_cell_i2.baseline_test_inventory.v1"
BASELINE_TEST_INVENTORY_PATH = Path(
    "verification/composable_cell_i2/baseline_test_inventory.v1.json"
)
BASELINE_TEST_INVENTORY_SHA256 = "a59745622c9047ef00c0533e5065067254787699ef0f53e509be000b7b366a9b"
NATIVE_TARGET_CONTRACT_PATH = Path(
    "verification/composable_cell_i2/native_target_contract.v1.json"
)
NATIVE_TARGET_CONTRACT_SHA256 = (
    "a5161424a62319cea8baa61ca15ceed318ca272b6c0654968f355593d5bffb9d"
)
BUILD_PROVENANCE_CONTRACT_PATH = Path(
    "verification/composable_cell_i2/build_provenance_contract.v1.json"
)
BUILD_PROVENANCE_CONTRACT_SHA256 = (
    "aa087f5cc90f90ce41a3e992c68101483269d1c0cd3865d63f4a0538cf174ad1"
)
NATIVE_TARGET_GROUP_DIGESTS = {
    "core_release_baseline": "0f84d1494ab118aa0a6862a4845cb49d03ab54ea7275f9e71f452563a8c307f6",
    "deployment_release_baseline": "a6f662da3281b925ff8f6bf75113f52a916b4e99ccd3b672233081ec306905ad",
    "roboplan_release_baseline": "a984d33198f8288429c4510dcc4f5c8b8fa7edad0136b7081f7a9f3c7e02f77b",
    "c02_tail_required": "55d408adc1de5dac6afdd95278f744b49cc6e9c5db93481ae274583be39a75ec",
    "roboplan_consumer_required": "bec4ba3c034fc4b3d6f641247411dc121da8d2e065fda1e52d52ae8ec18cc25e",
    "roboplan_scene_sanitizer_required": "56ca1f91d50afca581d2eb51b5fa8d79ffc6a1ef4e9d1916ddfbdc28f46dc8af",
}
NATIVE_ORDINARY_BY_GROUP = {
    "core_release_baseline": set(),
    "deployment_release_baseline": {
        "BuildManifestAuthorityRejectsMultiConfigGenerator",
        "CanonicalEmptyGeneratorCli",
        "BuildQualifiedAdmissionProfileIsDeterministic",
    },
    "c02_tail_required": set(),
    "roboplan_release_baseline": {
        "retained_compiled_visual_fk_composer_consumer",
        "retained_compiled_model_independent_standard_parser",
    },
    "roboplan_consumer_required": {
        "retained_compiled_visual_fk_composer_consumer",
        "retained_compiled_model_independent_standard_parser",
    },
    "roboplan_scene_sanitizer_required": set(),
}
NATIVE_GTEST_TIMEOUTS = {
    "core_release": 60.0,
    "deployment_release": 120.0,
    "c02_tail_focused_release": 180.0,
    "roboplan_release": 60.0,
    "roboplan_consumer_focused": 60.0,
    "core_asan_ubsan": 300.0,
    "core_tsan": 300.0,
    "deployment_asan_ubsan": 300.0,
    "roboplan_asan_ubsan": 60.0,
    "roboplan_tsan": 60.0,
}
ROBOPLAN_PINNED_LD_LIBRARY_PATH = (
    "/usr/local/lib:"
    "/usr/local/lib/python3.12/site-packages/cmeel.prefix/lib:"
    "/usr/local/lib"
)
PYTHON_BINDING_FUNCTION_COUNT = 142
PYTHON_BINDING_MINIMUM_CASE_COUNT = 177
PYTHON_BINDING_FUNCTION_DIGEST = (
    "6065a8a501c35dc6efb8367b37d0e669b17c5d09daac4d3d3adae63170340f99"
)
REQUIRED_TEST_GROUPS = {
    "core_release_baseline": (
        1101,
        "913239f745ab21fd301d57a2eada76ce5051672525ea142c47da17ddf0e9a15f",
    ),
    "deployment_release_baseline": (
        457,
        "f80a23ce8c0a260a53b8162241ca2ebab9cd374e50161da84f918767fc3a9c60",
    ),
    "roboplan_release_baseline": (
        56,
        "27308c0b04fd9730ea76dd83f1b6db8798c256a341377ebbe09d8a939da363ca",
    ),
    "c02_tail_required": (
        147,
        "5ddc5f532094e71969be1a36d3a8673e5b48a65877711381ce4ee1da24e007d1",
    ),
    "roboplan_consumer_required": (
        5,
        "5a18dc44dff19ee95daa8725d5c3eeeb50da6223b26d0048879bbe63d748d1bc",
    ),
    "roboplan_scene_sanitizer_required": (
        5,
        "70b8c38cdc5374f8d6739ac2461a280f26fb8137d9ef9e6e3192de39c5f18299",
    ),
}
REQUIRED_WITNESS_GROUPS = {
    "c02m_composition_shapes_required": (
        6,
        "bf7149c8b9cde46fb1b3948a5931559e68df9f13fc5d4fd3efe95befaecf655f",
    ),
    "c02m_coordinate_projection_required": (
        4,
        "dd2c50d538a4e5d769c75d2b2b44096d8b8994a70333c66593cde95b2165222a",
    ),
    "c02m_mf2_determinism_required": (
        6,
        "157c5622c3b8af275fee6dd30d2fe218e5984f16037fa49de2dabe34825f69fb",
    ),
    "c02m_mimic_dependency_required": (
        14,
        "8f205bca995927e010674acabedb1b4f16f3186d1c6819247b68a96f2bc67e52",
    ),
    "c02m_repeated_identity_required": (
        4,
        "e2045611ce3cfa7cfa717695630963d8f1e0e5c27e8f1f7eac53cdb07623ec3d",
    ),
    "c02n_applicability_matrix_required": (
        45,
        "d429e2ece8c23f128455354bdcdd138acb43405061421e9c1f871e2c6dc623cd",
    ),
    "c02n_bounds_required": (
        10,
        "7fd9c1dbfafd626b4939d5b4cddb6dba7ef4267363b6dc8b91ba14a759b2b939",
    ),
    "c02n_site_overlay_required": (
        17,
        "eb1e33428748e5a7810138686e19ab3f3174768777a45649e27f089c038d3874",
    ),
    "c02o_final_compiler_required": (
        8,
        "68a86c581a3c8e69c74399f76f28e00aef7e94bd40b8ebc6a7a464a04dce46c2",
    ),
}
NATIVE_EXECUTION_LANES = {
    "core_release",
    "deployment_release",
    "c02_tail_focused_release",
    "roboplan_release",
    "roboplan_consumer_focused",
    "core_asan_ubsan",
    "core_tsan",
    "deployment_asan_ubsan",
    "roboplan_asan_ubsan",
    "roboplan_tsan",
}
PRODUCTION_EXECUTION_LANES = {
    "core_production_release",
    "deployment_production_release",
}


def _production_build_environment(lane_id: str) -> dict[str, str]:
    temporary_root = Path("/qualification-output/tmp") / lane_id
    return {
        "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "TZ": "UTC",
        "HOME": temporary_root.as_posix(),
        "TMPDIR": temporary_root.as_posix(),
        "XDG_CACHE_HOME": (temporary_root / "xdg-cache").as_posix(),
        "PYTHONNOUSERSITE": "1",
        "CMAKE_BUILD_PARALLEL_LEVEL": "2",
    }
NATIVE_LANE_SCRIPTS = {
    "core_asan_ubsan": (
        "scripts/ci/run-composable-cell-i2-core-asan.sh",
        [],
        "BUILD_DIR",
        "build/core-asan",
    ),
    "core_tsan": (
        "scripts/ci/run-composable-cell-i2-core-tsan.sh",
        [],
        "BUILD_DIR",
        "build/core-tsan",
    ),
    "deployment_asan_ubsan": (
        "scripts/ci/run-composable-cell-i2-deployment-asan.sh",
        [],
        "DEPLOYMENT_BUILD_DIR",
        "build/deployment-asan",
    ),
    "roboplan_asan_ubsan": (
        "scripts/ci/run-composable-cell-i2-roboplan-scene-sanitizer.sh",
        ["address"],
        "BUILD_ROOT",
        "build/roboplan-asan",
    ),
    "roboplan_tsan": (
        "scripts/ci/run-composable-cell-i2-roboplan-scene-sanitizer.sh",
        ["thread"],
        "BUILD_ROOT",
        "build/roboplan-tsan",
    ),
}
NATIVE_SANITIZER_ENVIRONMENT = {
    "none": {},
    "address+undefined": {
        "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=1:exitcode=66",
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1:exitcode=66",
    },
    "thread": {
        "TSAN_OPTIONS": (
            "halt_on_error=1 second_deadlock_stack=1 history_size=7 "
            "exitcode=66 suppressions=/workspace/scripts/ci/tsan.supp"
        ),
    },
}
TSAN_EXECUTION_LANES = {"core_tsan", "roboplan_tsan", "bindings_tsan"}
TSAN_EXECUTOR_PREFIX = ["/usr/bin/setarch", "x86_64", "-R"]


def _native_process_base_environment(
    lane_id: str, sanitizer: str
) -> dict[str, str]:
    temporary_root = Path("/qualification-output/tmp") / lane_id
    return {
        "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "TZ": "UTC",
        "PYTHONNOUSERSITE": "1",
        "TMPDIR": temporary_root.as_posix(),
        "HOME": temporary_root.as_posix(),
        "XDG_CACHE_HOME": (temporary_root / "xdg-cache").as_posix(),
        **NATIVE_SANITIZER_ENVIRONMENT[sanitizer],
    }


BINDING_RESULT_LANES = {
    "bindings_selector",
    "bindings_release",
    "bindings_asan_ubsan",
    "bindings_tsan",
    "bindings_import",
    "bindings_parity",
    "bindings_lifetime",
    "bindings_exception",
    "bindings_gil_concurrency",
    "bindings_shutdown",
    "bindings_generated_api",
}
BINDING_HELPER_PATH = Path(
    "verification/composable_cell_i2/binding_contract_tests.py"
)
BINDING_NODE_PREFIX = f"{BINDING_HELPER_PATH.as_posix()}::test_"
BINDING_EXTENSION_NAME = (
    "manyforge_core_py.cpython-312-x86_64-linux-gnu.so"
)
BINDING_LANE_STAGE_ROOTS = {
    "bindings_selector": "/qualification-output/bindings-selector/staged",
    "bindings_release": "/qualification-output/bindings-release/staged",
    "bindings_asan_ubsan": "/qualification-output/bindings-asan/staged",
    "bindings_tsan": "/qualification-output/bindings-tsan/staged",
    **{
        lane_id: (
            "/qualification-input/bindings_release/bindings-release/staged"
        )
        for lane_id in (
            "bindings_import",
            "bindings_parity",
            "bindings_lifetime",
            "bindings_exception",
            "bindings_gil_concurrency",
            "bindings_shutdown",
            "bindings_generated_api",
        )
    },
}
BINDING_LANE_GENERATED_ROOTS = {
    "bindings_selector": "/qualification-output/bindings-selector/generated",
    "bindings_release": "/qualification-output/generated",
    "bindings_generated_api": "/qualification-output/generated",
}
BINDING_LANE_PREPARATION = {
    "bindings_selector": [
        "/usr/bin/env",
        "MANYFORGE_BINDINGS_PREPARE_ONLY=1",
        "MANYFORGE_BUILD_DIR=/qualification-output/build/bindings-selector",
        (
            "MANYFORGE_BINDINGS_STAGE_ROOT="
            "/qualification-output/bindings-selector/staged"
        ),
        (
            "MANYFORGE_BINDINGS_GENERATED_ROOT="
            "/qualification-output/bindings-selector/generated"
        ),
        "/workspace/scripts/test-all.sh",
        "bindings",
    ],
    "bindings_release": [
        "/workspace/scripts/ci/run-core-bindings.sh",
        "release",
        "--build-root",
        "/qualification-output/build/bindings-release",
        "--stage-root",
        "/qualification-output/bindings-release/staged",
        "--generated-output-root",
        "/qualification-output/generated",
        "--prepare-only",
    ],
    "bindings_asan_ubsan": [
        "/workspace/scripts/ci/run-core-bindings-sanitizer.sh",
        "address",
        "--build-root",
        "/qualification-output/build/bindings-asan",
        "--stage-root",
        "/qualification-output/bindings-asan/staged",
        "--prepare-only",
    ],
    "bindings_tsan": [
        "/workspace/scripts/ci/run-core-bindings-sanitizer.sh",
        "thread",
        "--build-root",
        "/qualification-output/build/bindings-tsan",
        "--stage-root",
        "/qualification-output/bindings-tsan/staged",
        "--prepare-only",
    ],
    "bindings_generated_api": [
        "/workspace/scripts/ci/run-core-bindings-contract.sh",
        "generated-api",
        "--extension",
        (
            "/qualification-input/bindings_release/bindings-release/staged/"
            + BINDING_EXTENSION_NAME
        ),
        "--generated-output-root",
        "/qualification-output/generated",
        "--prepare-only",
    ],
}
DETERMINISM_BUILD_LANES = {
    "deterministic_build_a": ("a", "determinism/build-a"),
    "deterministic_build_b": ("b", "determinism/build-b"),
}
DETERMINISM_ROOT_LANES = {
    root: lane_id for lane_id, (_, root) in DETERMINISM_BUILD_LANES.items()
}
DETERMINISM_PREPARATION = {
    lane_id: [
        "/workspace/scripts/ci/run-composable-cell-i2-determinism.sh",
        "--build-id",
        build_id,
        "--artifact-root",
        f"/qualification-output/{root}",
        "--prepare-only",
    ]
    for lane_id, (build_id, root) in DETERMINISM_BUILD_LANES.items()
}
GOLDEN_RELEASE_TEST = (
    "CanonicalEmptyAdmissionPackageTest.ReleasePackageMatchesTheCheckedQualificationGolden"
)
IMPLEMENTATION_BASES = {
    "manyforge": {
        "schema_version": SOURCE_IDENTITY_SCHEMA,
        "head_commit": "9252769feb7e57a054bb2e7b6067325758d0d09a",
        "head_tree": "6b582fc5a1bb6c37ec8bba8f01bd5bf2020e04c9",
        "dirty": False,
        "delta_sha256": "231be27a2a9546fef3543a3b9db4ebc12e6e654872be7babff59dd4147ab17c6",
        "fingerprint_sha256": "6af345f64d3e549f23c4bffc466b647ee1992d9e00960fb9188d8b4c3763acbc",
    },
    "manyforge_specs": {
        "schema_version": SOURCE_IDENTITY_SCHEMA,
        "head_commit": "38a11788503bae33cf536fa3e54b2837f05eefd4",
        "head_tree": "2212f526c052b4b0d08aa37e7565e9d3987227a7",
        "dirty": False,
        "delta_sha256": "231be27a2a9546fef3543a3b9db4ebc12e6e654872be7babff59dd4147ab17c6",
        "fingerprint_sha256": "ab59fedb7fb97698dc45436a7f399e3f077b9d5aeda6939d64f4d2197e38751f",
    },
}
FROZEN_SECTION_SHA256 = {
    "qualification_lanes": "66f88b604fc9140ad99494561fe45c11faaa1af8589d479162a8f2ed19921f40",
    "forbidden_scans": "8ece13c3bc7a54513a149260e8459ce7b8d563dec709bafafa723846b04633d3",
    "fixture_matrix": "daa18fd82cb0f878772bac43e7d017ae4c427b74489a1657bc8399124457edd7",
}
REQUIRED_EVIDENCE_FIELDS = {
    "manifest_sha256",
    "verifier_sha256",
    "historical_packet_lineage",
    "candidate_manyforge_source_identity",
    "candidate_specs_source_identity",
    "candidate_source_snapshot",
    "source_identity_file_evidence",
    "candidate_tracked_input_digests",
    "resolved_image_inspection",
    "abi_and_dependency_probe",
    "pinned_container_replay",
    "exact_lane_argv",
    "lane_exit_statuses",
    "lane_artifact_seals",
    "test_inventory_counts_and_digests",
    "binding_junit_summary",
    "log_sha256",
    "generated_output_sha256_and_size",
    "fixture_ir_manifest_golden_and_product_digests",
    "identity_rotation_comparison",
    "forbidden_scan_results",
    "final_unchanged_source_identity",
    "independent_adversarial_review",
}
REQUIRED_REVIEW_CHECKS = [
    "candidate_diff_and_historical_packet_lineage",
    "manifest_verifier_and_exact_lane_commands",
    "all_native_test_artifacts_and_raw_logs",
    "native_production_and_binding_build_provenance_closure",
    "exact_image_pinned_container_file_replay",
    "source_identity_and_tracked_input_rotation",
    "c02m_mf2_composition_and_determinism",
    "c02n_applicability_bounds_and_secret_absence",
    "c02o_exclusive_trust_binding_and_deletion_boundary",
    "false_green_unsafe_path_and_relabeling_attempts",
    "i2_nonclaims_and_later_owner_exclusions",
]


class ManifestError(RuntimeError):
    """The I2 qualification contract is malformed or cannot be verified."""


def fail(message: str) -> NoReturn:
    raise ManifestError(message)


def _object_without_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            fail(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        decoded = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_object_without_duplicates,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"manifest is not readable canonical JSON: {error}")
    if not isinstance(decoded, dict):
        fail("manifest root must be an object")
    return decoded


def _require_object(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        fail(f"{label} must be an object")
    return value


def _require_list(value: Any, label: str) -> list[Any]:
    if not isinstance(value, list):
        fail(f"{label} must be an array")
    return value


def _require_text(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        fail(f"{label} must be non-empty text")
    return value


def _exact_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    if set(value) != expected:
        missing = sorted(expected - set(value))
        extra = sorted(set(value) - expected)
        fail(f"{label} keys differ: missing={missing}, extra={extra}")


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        fail(f"cannot hash {path}: {error}")
    return digest.hexdigest()


def _canonical_json_sha256(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def _safe_relative_path(value: str, label: str) -> Path:
    path = Path(value)
    if path.is_absolute() or not path.parts or ".." in path.parts:
        fail(f"{label} must be a repository-relative path: {value}")
    return path


def _require_contained_regular_file(
    path: Path,
    root: Path,
    label: str,
    *,
    require_single_link: bool = False,
) -> Path:
    try:
        root_resolved = root.resolve(strict=True)
        unresolved = path if path.is_absolute() else root_resolved / path
        relative = unresolved.relative_to(root_resolved)
    except OSError as error:
        fail(f"{label} is not an existing regular file: {error}")
    except ValueError:
        fail(f"{label} escapes its declared root: {path}")
    current = root_resolved
    try:
        for index, part in enumerate(relative.parts):
            current = current / part
            metadata = current.lstat()
            if stat.S_ISLNK(metadata.st_mode):
                fail(f"{label} contains a symlinked path component: {current}")
            if index < len(relative.parts) - 1 and not stat.S_ISDIR(
                metadata.st_mode
            ):
                fail(f"{label} contains a non-directory path component: {current}")
    except OSError as error:
        fail(f"{label} is not an existing regular file: {error}")
    if not relative.parts or not stat.S_ISREG(metadata.st_mode):
        fail(f"{label} must be a non-symlink regular file: {path}")
    if require_single_link and metadata.st_nlink != 1:
        fail(f"{label} must have exactly one hard link: {path}")
    return current


def _require_contained_plain_directory(
    path: Path, root: Path, label: str
) -> Path:
    try:
        root_resolved = root.resolve(strict=True)
        unresolved = path if path.is_absolute() else root_resolved / path
        relative = unresolved.relative_to(root_resolved)
    except OSError as error:
        fail(f"{label} is not an existing directory: {error}")
    except ValueError:
        fail(f"{label} escapes its declared root: {path}")
    current = root_resolved
    try:
        for part in relative.parts:
            current = current / part
            metadata = current.lstat()
            if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(
                metadata.st_mode
            ):
                fail(f"{label} contains a symlink or non-directory: {current}")
    except OSError as error:
        fail(f"{label} is not an existing directory: {error}")
    if not relative.parts:
        fail(f"{label} may not be the lane root")
    return current


def _run_checked(
    argv: list[str], *, cwd: Path, label: str, text: bool = True
) -> subprocess.CompletedProcess[str]:
    try:
        result = subprocess.run(
            argv,
            cwd=cwd,
            check=False,
            capture_output=True,
            text=text,
        )
    except OSError as error:
        fail(f"{label} could not start: {error}")
    if result.returncode != 0:
        fail(
            f"{label} failed ({result.returncode}): "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )
    return result


def _git_tree(repository: Path, commit: str) -> str:
    return _run_checked(
        [
            "git",
            "--no-optional-locks",
            "-C",
            str(repository),
            "rev-parse",
            f"{commit}^{{tree}}",
        ],
        cwd=repository,
        label=f"Git tree lookup for {repository}",
    ).stdout.strip()


def _git_blob(repository: Path, commit: str, relative: str) -> bytes:
    result = subprocess.run(
        [
            "git",
            "--no-optional-locks",
            "-C",
            str(repository),
            "show",
            f"{commit}:{relative}",
        ],
        cwd=repository,
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        fail(
            f"baseline file is absent at {commit}:{relative}: "
            f"{result.stderr.decode(errors='replace').strip()}"
        )
    return result.stdout


def _git_head(repository: Path) -> str:
    return _run_checked(
        ["git", "--no-optional-locks", "-C", str(repository), "rev-parse", "HEAD"],
        cwd=repository,
        label=f"Git HEAD lookup for {repository}",
    ).stdout.strip()


def _require_commit_descends_from(
    repository: Path,
    *,
    candidate: str,
    base: str,
    label: str,
) -> None:
    if not HEX40.fullmatch(candidate) or not HEX40.fullmatch(base):
        fail(f"{label} ancestry identity is malformed")
    completed = subprocess.run(
        [
            "git",
            "--no-optional-locks",
            "-C",
            str(repository),
            "merge-base",
            "--is-ancestor",
            base,
            candidate,
        ],
        cwd=repository,
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode == 1:
        fail(f"{label} candidate does not descend from its implementation base")
    if completed.returncode != 0:
        fail(f"cannot verify {label} candidate ancestry: {completed.stderr.strip()}")


def _git_tree_entry(repository: Path, commit: str, relative: str) -> tuple[str, str]:
    output = _run_checked(
        [
            "git",
            "--no-optional-locks",
            "-C",
            str(repository),
            "ls-tree",
            commit,
            "--",
            relative,
        ],
        cwd=repository,
        label=f"Git tree-entry lookup for {relative}",
    ).stdout.strip()
    match = re.fullmatch(r"(100644|100755) blob ([0-9a-f]{40})\t(.+)", output)
    if match is None or match.group(3) != relative:
        fail(f"packet path is not one regular Git blob at {commit}: {relative}")
    return match.group(1), match.group(2)


def _git_tree_entry_optional(
    repository: Path, commit: str, relative: str
) -> tuple[str, str] | None:
    result = subprocess.run(
        [
            "git",
            "--no-optional-locks",
            "-C",
            str(repository),
            "ls-tree",
            commit,
            "--",
            relative,
        ],
        cwd=repository,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        fail(
            f"Git tree-entry lookup failed for {relative} at {commit}: "
            f"{result.stderr.strip()}"
        )
    output = result.stdout.strip()
    if not output:
        return None
    match = re.fullmatch(r"(100644|100755) blob ([0-9a-f]{40})\t(.+)", output)
    if match is None or match.group(3) != relative:
        fail(f"packet path is not one regular Git blob at {commit}: {relative}")
    return match.group(1), match.group(2)


def _validate_source_identity(
    value: Any, *, repository: Path | None, label: str
) -> dict[str, Any]:
    identity = _require_object(value, label)
    expected = {
        "schema_version",
        "head_commit",
        "head_tree",
        "dirty",
        "delta_sha256",
        "fingerprint_sha256",
    }
    _exact_keys(identity, expected, label)
    if identity["schema_version"] != SOURCE_IDENTITY_SCHEMA:
        fail(f"{label}.schema_version is not {SOURCE_IDENTITY_SCHEMA}")
    commit = _require_text(identity["head_commit"], f"{label}.head_commit")
    tree = _require_text(identity["head_tree"], f"{label}.head_tree")
    if not HEX40.fullmatch(commit) or not HEX40.fullmatch(tree):
        fail(f"{label} commit/tree must be lowercase 40-hex Git identities")
    if identity["dirty"] is not False:
        fail(f"{label} implementation base must be clean")
    for field in ("delta_sha256", "fingerprint_sha256"):
        if not HEX64.fullmatch(_require_text(identity[field], f"{label}.{field}")):
            fail(f"{label}.{field} must be lowercase SHA-256")
    if repository is not None and _git_tree(repository, commit) != tree:
        fail(f"{label} commit does not resolve to its declared tree")
    return identity


def _validate_base(manifest: dict[str, Any], repository_root: Path) -> None:
    base = _require_object(manifest["implementation_base"], "implementation_base")
    _exact_keys(base, {"manyforge", "manyforge_specs"}, "implementation_base")
    expected_paths = {"manyforge": ".", "manyforge_specs": "../manyforge_specs"}
    for name in ("manyforge", "manyforge_specs"):
        entry = _require_object(base[name], f"implementation_base.{name}")
        _exact_keys(
            entry,
            {"repository_path", "source_identity"},
            f"implementation_base.{name}",
        )
        declared_path = _require_text(
            entry["repository_path"],
            f"implementation_base.{name}.repository_path",
        )
        if declared_path != expected_paths[name]:
            fail(f"implementation_base.{name} repository path drifted")
        resolved = (repository_root / declared_path).resolve()
        repository: Path | None = resolved
        if not (resolved / ".git").exists() and name == "manyforge_specs":
            # The canonical dev container mounts only ManyForge at /workspace.
            # Host dispatch and final evidence still resolve the sibling Specs
            # repository before any lane runs.
            repository = None
        elif not (resolved / ".git").exists():
            fail(f"implementation_base.{name} repository is absent: {resolved}")
        identity = _validate_source_identity(
            entry["source_identity"], repository=repository, label=f"base.{name}"
        )
        if identity != IMPLEMENTATION_BASES[name]:
            fail(f"implementation base identity drifted for {name}")


def _validate_historical_packet_lineage(
    manifest: dict[str, Any],
    repository_root: Path,
) -> dict[str, Any]:
    contract = _require_object(
        manifest["historical_packet_lineage"], "historical_packet_lineage"
    )
    _exact_keys(
        contract,
        {
            "disposition",
            "initial_packet_commit",
            "initial_packet_tree",
            "initial_packet_parent_commit",
            "initial_new_paths",
            "initial_support_paths",
        },
        "historical_packet_lineage",
    )
    base_commit = IMPLEMENTATION_BASES["manyforge"]["head_commit"]
    if (
        contract["disposition"] != "historical_non_qualifying"
        or contract["initial_packet_commit"] != HISTORICAL_PACKET_COMMIT
        or contract["initial_packet_tree"] != HISTORICAL_PACKET_TREE
        or contract["initial_packet_parent_commit"] != base_commit
    ):
        fail("historical packet lineage identity drifted")
    new_paths = _require_list(
        contract["initial_new_paths"],
        "historical_packet_lineage.initial_new_paths",
    )
    support_paths = _require_list(
        contract["initial_support_paths"],
        "historical_packet_lineage.initial_support_paths",
    )
    if (
        new_paths != sorted(HISTORICAL_NEW_PACKET_PATHS)
        or support_paths != sorted(HISTORICAL_SUPPORT_PATHS)
        or set(new_paths).intersection(support_paths)
    ):
        fail("historical packet lineage path inventory drifted")
    paths = [*new_paths, *support_paths]
    base_entries: dict[str, tuple[str, str] | None] = {
        relative: _git_tree_entry_optional(repository_root, base_commit, relative)
        for relative in paths
    }
    if any(base_entries[path] is not None for path in new_paths):
        fail("a new qualification packet path already exists at the implementation base")
    if any(base_entries[path] is None for path in support_paths):
        fail("a historical support path is absent at the implementation base")

    initial_commit = HISTORICAL_PACKET_COMMIT
    if _git_tree(repository_root, initial_commit) != HISTORICAL_PACKET_TREE:
        fail("historical packet tree drifted")
    parents = _run_checked(
        [
            "git",
            "--no-optional-locks",
            "-C",
            str(repository_root),
            "rev-list",
            "--parents",
            "-n",
            "1",
            initial_commit,
        ],
        cwd=repository_root,
        label="packet first-add parent lookup",
    ).stdout.split()
    if parents != [initial_commit, base_commit]:
        fail("historical packet commit does not directly follow its implementation base")
    changed_paths = set(
        _run_checked(
            [
                "git",
                "--no-optional-locks",
                "-C",
                str(repository_root),
                "diff-tree",
                "--no-commit-id",
                "--name-only",
                "-r",
                initial_commit,
            ],
            cwd=repository_root,
            label="packet first-add changed-path lookup",
        ).stdout.splitlines()
    )
    if changed_paths != set(new_paths):
        fail("packet first-add commit path inventory drifted")
    if any(
        _git_tree_entry(repository_root, initial_commit, path)
        != base_entries[path]
        for path in support_paths
    ):
        fail("a support path changed in the historical packet commit")
    files: list[dict[str, Any]] = []
    for relative in paths:
        mode, blob = _git_tree_entry(repository_root, initial_commit, relative)
        content = _git_blob(repository_root, initial_commit, relative)
        files.append(
            {
                "path": relative,
                "origin": "initial_new" if relative in new_paths else "support",
                "sha256": hashlib.sha256(content).hexdigest(),
                "size": len(content),
                "initial_git_mode": mode,
                "initial_git_blob": blob,
                "base_git_mode": (
                    base_entries[relative][0]
                    if base_entries[relative] is not None
                    else None
                ),
                "base_git_blob": (
                    base_entries[relative][1]
                    if base_entries[relative] is not None
                    else None
                ),
            }
        )
    return {
        "disposition": "historical_non_qualifying",
        "initial_packet_commit": initial_commit,
        "initial_packet_tree": HISTORICAL_PACKET_TREE,
        "initial_packet_parent_commit": base_commit,
        "files": files,
    }


def _validate_candidate_identity(value: Any, repository_root: Path) -> None:
    candidate = _require_object(value, "candidate_identity")
    expected = {
        "capture_tool",
        "require_clean_committed_manyforge_tree",
        "require_clean_committed_specs_tree",
        "capture_before_first_lane",
        "recheck_after_last_lane",
        "require_unchanged_fingerprint",
        "note",
    }
    _exact_keys(candidate, expected, "candidate_identity")
    capture_tool = _safe_relative_path(
        _require_text(candidate["capture_tool"], "candidate_identity.capture_tool"),
        "candidate_identity.capture_tool",
    )
    if capture_tool != CAPTURE_TOOL:
        fail(f"candidate capture tool must be exactly {CAPTURE_TOOL.as_posix()}")
    _require_contained_regular_file(
        repository_root / capture_tool,
        repository_root,
        "candidate capture tool",
    )
    for field in (
        "require_clean_committed_manyforge_tree",
        "require_clean_committed_specs_tree",
        "capture_before_first_lane",
        "recheck_after_last_lane",
        "require_unchanged_fingerprint",
    ):
        if candidate[field] is not True:
            fail(f"candidate_identity.{field} must be true")
    _require_text(candidate["note"], "candidate_identity.note")


def _validate_environment(environment: dict[str, Any]) -> None:
    _exact_keys(
        environment,
        {
            "image",
            "docker_prefix",
            "machine",
            "address_bits",
            "glibc",
            "cmake",
            "ninja",
            "cxx_compiler",
            "production_build",
            "sanitizer_builds",
            "python",
            "pybind11",
        },
        "qualification_environment",
    )
    image = _require_object(environment.get("image"), "qualification_environment.image")
    _exact_keys(
        image,
        {"id", "os", "architecture", "platform"},
        "qualification_environment.image",
    )
    image_id = _require_text(image["id"], "image.id")
    if not image_id.startswith("sha256:") or not HEX64.fullmatch(image_id[7:]):
        fail("image.id must be a resolved sha256 image ID")
    if (image["os"], image["architecture"], image["platform"]) != (
        "linux",
        "amd64",
        "linux/amd64",
    ):
        fail("I2 qualification image must be exactly linux/amd64")
    prefix = _require_list(
        environment.get("docker_prefix"), "qualification_environment.docker_prefix"
    )
    expected_prefix = [
        "docker",
        "run",
        "--rm",
        "--init",
        "--user",
        "{host_uid}:{host_gid}",
        "--platform",
        "linux/amd64",
        "--network",
        "none",
        "--security-opt",
        "seccomp=unconfined",
        "--mount",
        "type=bind,src={repository_root},dst=/workspace,readonly",
        "--mount",
        "type=bind,src={output_root},dst=/qualification-output",
        "--workdir",
        "/workspace",
        "{image_id}",
    ]
    if prefix != expected_prefix:
        fail("qualification_environment.docker_prefix differs from the frozen argv")
    expected_scalars = {
        "machine": "x86_64",
        "address_bits": 64,
        "glibc": "2.36",
        "cmake": "3.25.1",
        "ninja": "1.11.1",
        "cxx_compiler": "g++ (Debian 12.2.0-14+deb12u1) 12.2.0",
    }
    for field, expected in expected_scalars.items():
        if environment[field] != expected:
            fail(f"qualification_environment.{field} drifted from {expected!r}")
    production = _require_object(
        environment["production_build"], "qualification_environment.production_build"
    )
    _exact_keys(
        production,
        {"generator", "configuration", "cxx_standard", "single_configuration"},
        "qualification_environment.production_build",
    )
    if production != {
        "generator": "Ninja",
        "configuration": "Release",
        "cxx_standard": 20,
        "single_configuration": True,
    }:
        fail("production build profile differs from the frozen I2 profile")
    if environment["sanitizer_builds"] != ["address+undefined", "thread"]:
        fail("sanitizer build inventory differs from the frozen I2 profile")
    python = _require_object(environment.get("python"), "environment.python")
    _exact_keys(
        python,
        {
            "implementation",
            "version",
            "cache_tag",
            "soabi",
            "extension_suffix",
            "multiarch",
            "py_debug",
            "gil_disabled",
        },
        "qualification_environment.python",
    )
    if python != {
        "implementation": "CPython",
        "version": "3.12.13",
        "cache_tag": "cpython-312",
        "soabi": "cpython-312-x86_64-linux-gnu",
        "extension_suffix": ".cpython-312-x86_64-linux-gnu.so",
        "multiarch": "x86_64-linux-gnu",
        "py_debug": False,
        "gil_disabled": False,
    }:
        fail("CPython ABI profile differs from the frozen ordinary-GIL ABI")
    pybind11 = _require_object(environment.get("pybind11"), "environment.pybind11")
    _exact_keys(
        pybind11,
        {"version", "cmake_dir"},
        "qualification_environment.pybind11",
    )
    if pybind11 != {
        "version": "3.0.3",
        "cmake_dir": "/usr/local/lib/python3.12/site-packages/pybind11/share/cmake/pybind11",
    }:
        fail("pybind11 profile differs from the frozen I2 profile")


def _validate_bindings(binding: dict[str, Any], repository_root: Path) -> None:
    _exact_keys(
        binding,
        {
            "production_modules",
            "forbidden_production_modules",
            "framework",
            "registration_units",
            "build_contract",
            "closure_integration",
            "existing_test_files",
            "generated_outputs",
            "generated_api_contract",
            "required_final_cases",
            "case_witness_contracts",
            "skip_policy",
            "minimum_collected_tests",
        },
        "binding_contract",
    )
    if binding.get("production_modules") != ["manyforge_core_py"]:
        fail("manyforge_core_py must be the sole production binding module")
    forbidden = _require_list(
        binding.get("forbidden_production_modules"),
        "binding_contract.forbidden_production_modules",
    )
    if forbidden != ["manyforge_roboplan_py", "e0_nanobind", "e0_pybind"]:
        fail("forbidden production binding-module inventory drifted")
    if binding.get("framework") != "pybind11":
        fail("I2 must retain pybind11")
    for field in ("registration_units", "existing_test_files"):
        paths = _require_list(binding.get(field), f"binding_contract.{field}")
        if len(paths) != len(set(paths)) or not paths:
            fail(f"binding_contract.{field} must be non-empty and unique")
        for index, value in enumerate(paths):
            relative = _safe_relative_path(
                _require_text(value, f"{field}[{index}]"), f"{field}[{index}]"
            )
            if not (repository_root / relative).is_file():
                fail(f"binding input is absent: {relative}")
    if set(binding["registration_units"]) != REQUIRED_REGISTRATION_UNITS:
        fail("binding registration-unit inventory differs from the frozen set")
    if set(binding["existing_test_files"]) != REQUIRED_EXISTING_BINDING_TESTS:
        fail("existing binding-test inventory differs from the frozen set")
    build_contract = _safe_relative_path(
        _require_text(binding.get("build_contract"), "binding_contract.build_contract"),
        "binding_contract.build_contract",
    )
    if not (repository_root / build_contract).is_file():
        fail(f"binding build contract is absent: {build_contract}")
    integration = _require_object(
        binding.get("closure_integration"), "binding_contract.closure_integration"
    )
    _exact_keys(
        integration,
        {"must_update", "must_add"},
        "binding_contract.closure_integration",
    )
    for field in ("must_update", "must_add"):
        values = _require_list(integration[field], f"closure_integration.{field}")
        if not values or len(values) != len(set(values)):
            fail(f"closure_integration.{field} must be non-empty and unique")
        for value in values:
            relative = _safe_relative_path(value, f"closure_integration.{field}")
            if field == "must_update" and not (repository_root / relative).is_file():
                fail(f"required binding integration input is absent: {relative}")
    if set(integration["must_update"]) != REQUIRED_BINDING_INTEGRATION_UPDATES:
        fail("binding integration-update inventory differs from the frozen set")
    if set(integration["must_add"]) != REQUIRED_BINDING_SCRIPTS:
        fail("binding script inventory differs from the frozen set")
    outputs = _require_list(binding.get("generated_outputs"), "generated_outputs")
    generated: dict[str, str] = {}
    for index, raw in enumerate(outputs):
        output = _require_object(raw, f"generated_outputs[{index}]")
        _exact_keys(output, {"role", "path"}, f"generated_outputs[{index}]")
        role = _require_text(output["role"], f"generated_outputs[{index}].role")
        path = _require_text(output["path"], f"generated_outputs[{index}].path")
        if role in generated:
            fail("generated output roles must be unique and confined to generated output")
        generated[role] = path
    if generated != REQUIRED_GENERATED_OUTPUTS:
        fail("generated output inventory is incomplete")
    api_contract = _require_object(
        binding["generated_api_contract"], "binding_contract.generated_api_contract"
    )
    _exact_keys(
        api_contract,
        {
            "schema_version",
            "required_read_only_nonconstructible_exports",
            "forbidden_exports",
        },
        "binding_contract.generated_api_contract",
    )
    if api_contract["schema_version"] != (
        "manyforge.composable_cell_i2.binding_api_inventory.v1"
    ):
        fail("generated binding API schema drifted")
    required_exports = _require_list(
        api_contract["required_read_only_nonconstructible_exports"],
        "required nonconstructible binding exports",
    )
    forbidden_exports = _require_list(
        api_contract["forbidden_exports"], "forbidden binding exports"
    )
    if (
        len(required_exports) != len(set(required_exports))
        or set(required_exports) != REQUIRED_NONCONSTRUCTIBLE_EXPORTS
        or len(forbidden_exports) != len(set(forbidden_exports))
        or set(forbidden_exports) != FORBIDDEN_BINDING_EXPORTS
    ):
        fail("generated binding API export contract drifted")
    final_cases = _require_list(
        binding.get("required_final_cases"), "binding_contract.required_final_cases"
    )
    if len(final_cases) != len(set(final_cases)) or set(final_cases) != REQUIRED_BINDING_CASES:
        fail("binding final-case inventory differs from the frozen set")
    witness_rows = _require_list(
        binding["case_witness_contracts"], "binding case witness contracts"
    )
    witness_contracts: dict[str, list[str]] = {}
    for index, raw in enumerate(witness_rows):
        row = _require_object(raw, f"binding case witness contract {index}")
        _exact_keys(
            row,
            {"case_id", "required_measurements", "required_evidence_roles"},
            f"binding case witness contract {index}",
        )
        case_id = _require_text(
            row["case_id"], f"binding case witness contract {index} case ID"
        )
        measurements = _require_list(
            row["required_measurements"],
            f"binding case witness contract {case_id} measurements",
        )
        evidence_roles = _require_list(
            row["required_evidence_roles"],
            f"binding case witness contract {case_id} evidence roles",
        )
        if (
            case_id in witness_contracts
            or not measurements
            or len(measurements) != len(set(measurements))
            or not all(isinstance(value, str) and IDENTIFIER.fullmatch(value) for value in measurements)
            or not evidence_roles
            or len(evidence_roles) != len(set(evidence_roles))
            or not all(
                isinstance(value, str) and IDENTIFIER.fullmatch(value)
                for value in evidence_roles
            )
        ):
            fail(f"binding case witness contract is malformed: {case_id}")
        witness_contracts[case_id] = measurements
    if witness_contracts != REQUIRED_BINDING_WITNESS_MEASUREMENTS:
        fail("binding case witness measurement contract drifted")
    declared_roles = {
        row["case_id"]: row["required_evidence_roles"] for row in witness_rows
    }
    if declared_roles != REQUIRED_BINDING_WITNESS_EVIDENCE_ROLES:
        fail("binding case witness evidence-role contract drifted")
    if binding.get("skip_policy") != "forbid":
        fail("binding skip policy must be forbid")
    if not isinstance(binding.get("minimum_collected_tests"), int) or binding[
        "minimum_collected_tests"
    ] < 1:
        fail("binding tests must require nonzero collection")


def _validate_pinned_inputs(
    pinned: dict[str, Any], manifest: dict[str, Any], repository_root: Path
) -> None:
    _exact_keys(
        pinned,
        {"roboplan", "consumer_stack", "tracked_files", "consumer_targets", "consumer_tests"},
        "pinned_inputs",
    )
    roboplan = _require_object(pinned.get("roboplan"), "pinned_inputs.roboplan")
    _exact_keys(
        roboplan,
        {
            "role",
            "version",
            "checkout_path",
            "commit",
            "tree",
            "patched_index_tree",
            "require_clean",
            "must_not_change_in_i2",
            "pin_manifest_sha256",
            "patch_manifest_sha256",
            "patches",
        },
        "pinned_inputs.roboplan",
    )
    if (
        roboplan.get("role") != "file_only_regression_consumer"
        or roboplan.get("version") != "0.5.1"
        or roboplan.get("require_clean") is not True
        or roboplan.get("must_not_change_in_i2") is not True
    ):
        fail("RoboPlan must stay the immutable 0.5.1 regression consumer")
    commit = _require_text(roboplan.get("commit"), "roboplan.commit")
    tree = _require_text(roboplan.get("tree"), "roboplan.tree")
    patched_index_tree = _require_text(
        roboplan.get("patched_index_tree"), "roboplan.patched_index_tree"
    )
    checkout_relative = _safe_relative_path(
        _require_text(roboplan.get("checkout_path"), "roboplan.checkout_path"),
        "roboplan.checkout_path",
    )
    if (
        checkout_relative != Path(".docker/roboplan")
        or commit != "e06d1a2f1b749c38742aee9e401bbe97fa967f7b"
        or tree != "d918767326a19d44886c0a4560cabe5a4ebe044d"
        or patched_index_tree != "687405b677d50965c9ce5701016cb67c4579c563"
    ):
        fail("RoboPlan checkout/commit/tree pin drifted")
    checkout = repository_root / checkout_relative
    if _git_tree(checkout, commit) != tree:
        fail("RoboPlan commit/tree pin does not resolve")
    checkout_status = _run_checked(
        [
            "git",
            "--no-optional-locks",
            "-C",
            str(checkout),
            "status",
            "--porcelain=v1",
            "--untracked-files=all",
            "--ignore-submodules=none",
        ],
        cwd=repository_root,
        label="RoboPlan clean-checkout validation",
    ).stdout
    if checkout_status:
        fail("RoboPlan regression checkout is dirty")
    pin_digest = _require_text(
        roboplan["pin_manifest_sha256"], "roboplan.pin_manifest_sha256"
    )
    patch_manifest_digest = _require_text(
        roboplan["patch_manifest_sha256"], "roboplan.patch_manifest_sha256"
    )
    if (
        not HEX64.fullmatch(pin_digest)
        or _sha256_file(repository_root / "third_party/roboplan/README.md") != pin_digest
    ):
        fail("RoboPlan pin manifest drifted")
    if (
        not HEX64.fullmatch(patch_manifest_digest)
        or _sha256_file(repository_root / "third_party/roboplan/patches.sha256")
        != patch_manifest_digest
    ):
        fail("RoboPlan patch manifest drifted")
    patches = _require_list(roboplan.get("patches"), "roboplan.patches")
    if len(patches) != 7:
        fail("RoboPlan patch series must contain exactly seven patches")
    expected_patch_rows: list[dict[str, str]] = []
    try:
        for line in (repository_root / "third_party/roboplan/patches.sha256").read_text(
            encoding="utf-8"
        ).splitlines():
            digest, relative_value = line.split(maxsplit=1)
            expected_patch_rows.append(
                {
                    "path": f"third_party/roboplan/{relative_value}",
                    "sha256": digest,
                }
            )
    except (OSError, UnicodeError, ValueError) as error:
        fail(f"RoboPlan patch manifest is malformed: {error}")
    if patches != expected_patch_rows:
        fail("RoboPlan patch row inventory differs from the pinned patch manifest")
    for index, raw in enumerate(patches):
        row = _require_object(raw, f"roboplan.patches[{index}]")
        _exact_keys(row, {"path", "sha256"}, f"roboplan.patches[{index}]")
        relative = _safe_relative_path(row["path"], f"roboplan.patches[{index}].path")
        digest = _require_text(row["sha256"], f"roboplan.patches[{index}].sha256")
        patch_path = _require_contained_regular_file(
            repository_root / relative,
            repository_root,
            f"RoboPlan patch input {relative}",
        )
        if not HEX64.fullmatch(digest) or _sha256_file(patch_path) != digest:
            fail(f"RoboPlan patch input drifted: {relative}")
    base_commit = manifest["implementation_base"]["manyforge"]["source_identity"][
        "head_commit"
    ]
    tracked = _require_list(pinned.get("tracked_files"), "pinned_inputs.tracked_files")
    tracked_policies: dict[str, str] = {}
    for index, raw in enumerate(tracked):
        row = _require_object(raw, f"tracked_files[{index}]")
        _exact_keys(row, {"path", "sha256", "policy"}, f"tracked_files[{index}]")
        relative = _safe_relative_path(row["path"], f"tracked_files[{index}].path")
        digest = _require_text(row["sha256"], f"tracked_files[{index}].sha256")
        policy = _require_text(row["policy"], f"tracked_files[{index}].policy")
        if not HEX64.fullmatch(digest):
            fail(f"tracked_files[{index}].sha256 is malformed")
        if policy not in TRACKED_FILE_POLICIES:
            fail(f"tracked_files[{index}].policy is unknown: {policy}")
        if relative.as_posix() in tracked_policies:
            fail(f"tracked regression input is duplicated: {relative}")
        tracked_policies[relative.as_posix()] = policy
        baseline = _git_blob(repository_root, base_commit, relative.as_posix())
        if hashlib.sha256(baseline).hexdigest() != digest:
            fail(f"tracked baseline digest is wrong: {relative}")
        candidate = _require_contained_regular_file(
            repository_root / relative,
            repository_root,
            f"tracked regression input {relative}",
        )
        if policy == "immutable_in_i2" and _sha256_file(candidate) != digest:
            fail(f"immutable I2 input drifted: {relative}")
    if tracked_policies != REQUIRED_TRACKED_FILE_POLICIES:
        fail("tracked regression-input path/policy inventory differs from the frozen set")
    consumer_targets = _require_list(
        pinned.get("consumer_targets"), "pinned_inputs.consumer_targets"
    )
    if (
        len(consumer_targets) != len(set(consumer_targets))
        or set(consumer_targets) != REQUIRED_CONSUMER_TARGETS
    ):
        fail("retained consumer target inventory differs from the frozen set")
    consumers = _require_list(pinned.get("consumer_tests"), "consumer_tests")
    if len(consumers) != len(set(consumers)) or set(consumers) != REQUIRED_CONSUMER_TESTS:
        fail("retained consumer test inventory differs from the frozen set")

    stack = _require_object(pinned.get("consumer_stack"), "pinned_inputs.consumer_stack")
    _exact_keys(
        stack,
        {"pinocchio", "coal", "check_urdf_path", "check_urdf_sha256"},
        "pinned_inputs.consumer_stack",
    )
    if stack != {
        "pinocchio": "4.1.0",
        "coal": "3.0.3",
        "check_urdf_path": "/usr/local/bin/check_urdf",
        "check_urdf_sha256": "83352c180a270474e19ac0d960871ee5104263bd03ec1402d4905a4024d2a109",
    }:
        fail("RoboPlan/Pinocchio consumer stack drifted from the frozen profile")


def _candidate_tracked_file_digests(
    manifest: dict[str, Any], repository_root: Path, *, require_final_rotation: bool
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for raw in manifest["pinned_inputs"]["tracked_files"]:
        relative = _safe_relative_path(raw["path"], "tracked candidate path")
        candidate = _require_contained_regular_file(
            repository_root / relative,
            repository_root,
            f"tracked candidate input {relative}",
        )
        current = _sha256_file(candidate)
        baseline = raw["sha256"]
        policy = raw["policy"]
        changed = current != baseline
        if policy == "immutable_in_i2" and changed:
            fail(f"immutable I2 input drifted: {relative}")
        if (
            require_final_rotation
            and policy in {"must_change_and_requalify", "rotate_and_requalify"}
            and not changed
        ):
            fail(f"required I2 input rotation did not occur: {relative}")
        rows.append(
            {
                "path": relative.as_posix(),
                "policy": policy,
                "baseline_sha256": baseline,
                "candidate_sha256": current,
                "size": candidate.stat().st_size,
                "changed": changed,
            }
        )
    return rows


def _validate_lanes(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    raw_lanes = _require_list(manifest["qualification_lanes"], "qualification_lanes")
    lanes: dict[str, dict[str, Any]] = {}
    for index, raw in enumerate(raw_lanes, start=1):
        lane = _require_object(raw, f"qualification_lanes[{index - 1}]")
        _exact_keys(
            lane,
            {
                "ordinal",
                "id",
                "claim",
                "container_command",
                "skip_policy",
                "expected_outputs",
            },
            f"qualification_lanes[{index - 1}]",
        )
        lane_id = _require_text(lane["id"], f"lane[{index}].id")
        if not IDENTIFIER.fullmatch(lane_id) or lane_id in lanes:
            fail(f"lane ID is invalid or duplicated: {lane_id}")
        if lane["ordinal"] != index:
            fail(f"lane {lane_id} has non-contiguous ordinal")
        _require_text(lane["claim"], f"lane {lane_id} claim")
        command = _require_list(lane["container_command"], f"lane {lane_id} command")
        if not command or not all(isinstance(value, str) and value for value in command):
            fail(f"lane {lane_id} command must be non-empty argv")
        if lane_id in TSAN_EXECUTION_LANES:
            if command[:3] != TSAN_EXECUTOR_PREFIX:
                fail(f"TSan lane does not use the exact no-ASLR executor: {lane_id}")
        elif command[0] not in {"bash", "python3"}:
            fail(f"lane {lane_id} command has undeclared executable")
        if any("{repository_root}" in value or "{output_root}" in value or "{image_id}" in value for value in command):
            fail(f"lane {lane_id} may not introduce renderer placeholders")
        if lane["skip_policy"] != "forbid":
            fail(f"lane {lane_id} permits a skip")
        outputs = _require_list(lane["expected_outputs"], f"lane {lane_id} outputs")
        if len(outputs) != len(set(outputs)):
            fail(f"lane {lane_id} contains duplicate expected outputs")
        for output in outputs:
            _safe_relative_path(_require_text(output, f"lane {lane_id} output"), f"lane {lane_id} output")
        lanes[lane_id] = lane
    if set(lanes) != REQUIRED_LANES:
        fail(
            "qualification lane set differs from v1 contract: "
            f"missing={sorted(REQUIRED_LANES - set(lanes))}, "
            f"extra={sorted(set(lanes) - REQUIRED_LANES)}"
        )
    for lane_id in NATIVE_EXECUTION_LANES:
        expected = [
            *(TSAN_EXECUTOR_PREFIX if lane_id in TSAN_EXECUTION_LANES else []),
            "python3",
            "verification/composable_cell_i2/verify_qualification_manifest.py",
            "--manifest",
            "verification/composable_cell_i2/qualification_manifest.v1.json",
            "--execute-native-lane",
            lane_id,
            "--output-root",
            "/qualification-output",
        ]
        if lanes[lane_id]["container_command"] != expected:
            fail(f"native lane does not use the exact frozen executor: {lane_id}")
    for lane_id in PRODUCTION_EXECUTION_LANES:
        expected = [
            "python3",
            "verification/composable_cell_i2/verify_qualification_manifest.py",
            "--manifest",
            "verification/composable_cell_i2/qualification_manifest.v1.json",
            "--execute-production-lane",
            lane_id,
            "--output-root",
            "/qualification-output",
        ]
        if lanes[lane_id]["container_command"] != expected:
            fail(f"production lane does not use the exact frozen executor: {lane_id}")
    for lane_id in BINDING_RESULT_LANES:
        expected = [
            *(TSAN_EXECUTOR_PREFIX if lane_id in TSAN_EXECUTION_LANES else []),
            "python3",
            "verification/composable_cell_i2/verify_qualification_manifest.py",
            "--manifest",
            "verification/composable_cell_i2/qualification_manifest.v1.json",
            "--execute-binding-lane",
            lane_id,
            "--output-root",
            "/qualification-output",
        ]
        if lanes[lane_id]["container_command"] != expected:
            fail(f"binding lane does not use the exact frozen executor: {lane_id}")
    for lane_id in DETERMINISM_BUILD_LANES:
        expected = [
            "python3",
            "verification/composable_cell_i2/verify_qualification_manifest.py",
            "--manifest",
            "verification/composable_cell_i2/qualification_manifest.v1.json",
            "--execute-determinism-build",
            lane_id,
            "--output-root",
            "/qualification-output",
        ]
        if lanes[lane_id]["container_command"] != expected:
            fail(f"determinism build does not use the frozen executor: {lane_id}")
    expected_compare = [
        "python3",
        "verification/composable_cell_i2/verify_qualification_manifest.py",
        "--manifest",
        "verification/composable_cell_i2/qualification_manifest.v1.json",
        "--execute-determinism-compare",
        "--output-root",
        "/qualification-output",
    ]
    if lanes["deterministic_rebuild"]["container_command"] != expected_compare:
        fail("determinism comparison does not use the frozen executor")
    expected_scan = [
        "python3",
        "verification/composable_cell_i2/verify_qualification_manifest.py",
        "--manifest",
        "verification/composable_cell_i2/qualification_manifest.v1.json",
        "--scan",
        "--output-root",
        "/qualification-output",
    ]
    scan_lane = lanes["forbidden_surface_scan"]
    if scan_lane["container_command"] != expected_scan:
        fail("forbidden-surface scan does not use the frozen artifact executor")
    if scan_lane["expected_outputs"] != [FORBIDDEN_SCAN_OUTPUT]:
        fail("forbidden-surface scan does not declare its exact artifact")
    rendered_commands = "\n".join(
        value
        for argv in [
            *BINDING_LANE_PREPARATION.values(),
            *DETERMINISM_PREPARATION.values(),
        ]
        for value in argv
    )
    for required_script in REQUIRED_BINDING_SCRIPTS:
        if required_script not in rendered_commands:
            fail(f"required future I2 script has no frozen lane: {required_script}")
    return lanes


def _validate_artifact_dependency_graph(
    value: Any, lanes: dict[str, dict[str, Any]]
) -> None:
    graph = _require_object(value, "artifact_dependency_graph")
    _exact_keys(
        graph,
        {
            "lane_output_namespace",
            "producer_inputs",
            "producer_input_mount",
            "host_seal",
            "closure_recheck",
        },
        "artifact_dependency_graph",
    )
    if graph != {
        "lane_output_namespace": "one dedicated writable root per lane",
        "producer_inputs": {
            lane_id: list(producers)
            for lane_id, producers in sorted(LANE_READ_ONLY_INPUTS.items())
        },
        "producer_input_mount": (
            "each declared producer root is mounted read-only below "
            "/qualification-input/<producer-lane>"
        ),
        "host_seal": (
            "host captures the exact path/type/mode/size/content inventory "
            "immediately after each successful lane"
        ),
        "closure_recheck": (
            "every lane inventory must remain byte-for-byte unchanged through closure"
        ),
    }:
        fail("artifact producer/consumer isolation contract drifted")
    ordinals = {lane_id: lane["ordinal"] for lane_id, lane in lanes.items()}
    for consumer, producers in LANE_READ_ONLY_INPUTS.items():
        if consumer not in lanes or any(producer not in lanes for producer in producers):
            fail("artifact dependency graph names an unknown lane")
        if any(ordinals[producer] >= ordinals[consumer] for producer in producers):
            fail("artifact dependency graph is not producer-before-consumer ordered")


def _validate_fixtures(
    manifest: dict[str, Any],
    lanes: dict[str, dict[str, Any]],
    repository_root: Path,
) -> None:
    groups, _ = _load_baseline_test_inventory(
        manifest["test_inventory_contract"], manifest, repository_root
    )
    native_contracts = {
        row["lane_id"]: row
        for row in manifest["test_inventory_contract"]["native_execution_lanes"]
    }
    binding_contracts = {
        row["lane_id"]: row
        for row in manifest["test_inventory_contract"]["binding_result_lanes"]
    }
    allowed_determinism_artifacts = set(REQUIRED_FIXTURE_IR_OUTPUTS.values()) | {
        REQUIRED_IDENTITY_ARTIFACT["path"]
    }
    lane_ordinals = {lane_id: lane["ordinal"] for lane_id, lane in lanes.items()}
    fixtures = _require_list(manifest["fixture_matrix"], "fixture_matrix")
    seen: set[str] = set()
    for index, raw in enumerate(fixtures):
        fixture = _require_object(raw, f"fixture_matrix[{index}]")
        _exact_keys(
            fixture,
            {
                "id",
                "evidence_lanes",
                "native_test_witnesses",
                "binding_case_witnesses",
                "python_binding_floor_lanes",
                "scan_ids",
                "determinism_artifacts",
                "generated_output_roles",
            },
            f"fixture_matrix[{index}]",
        )
        fixture_id = _require_text(fixture["id"], f"fixture_matrix[{index}].id")
        if not IDENTIFIER.fullmatch(fixture_id) or fixture_id in seen:
            fail(f"fixture requirement is invalid or duplicated: {fixture_id}")
        seen.add(fixture_id)
        raw_evidence_lanes = _require_list(
            fixture["evidence_lanes"], f"fixture {fixture_id} evidence_lanes"
        )
        if (
            not raw_evidence_lanes
            or not all(isinstance(lane, str) and lane in lanes for lane in raw_evidence_lanes)
            or len(raw_evidence_lanes) != len(set(raw_evidence_lanes))
        ):
            fail(f"fixture {fixture_id} references an unknown or duplicate lane")

        derived_lanes: set[str] = set()
        native_rows = _require_list(
            fixture["native_test_witnesses"],
            f"fixture {fixture_id} native test witnesses",
        )
        native_keys: set[tuple[str, str]] = set()
        for row_index, raw_native in enumerate(native_rows):
            native = _require_object(
                raw_native, f"fixture {fixture_id} native witness {row_index}"
            )
            _exact_keys(
                native,
                {"lane_id", "group_id"},
                f"fixture {fixture_id} native witness {row_index}",
            )
            lane_id = _require_text(
                native["lane_id"], f"fixture {fixture_id} native lane"
            )
            group_id = _require_text(
                native["group_id"], f"fixture {fixture_id} native group"
            )
            key = (lane_id, group_id)
            lane_contract = native_contracts.get(lane_id)
            if key in native_keys or lane_contract is None or group_id not in groups:
                fail(f"fixture {fixture_id} has an unknown or duplicate native witness")
            native_keys.add(key)
            registered_group = groups[lane_contract["required_group"]]
            if not groups[group_id].issubset(registered_group):
                fail(
                    f"fixture {fixture_id} native group is outside lane {lane_id}"
                )
            derived_lanes.add(lane_id)

        binding_rows = _require_list(
            fixture["binding_case_witnesses"],
            f"fixture {fixture_id} binding case witnesses",
        )
        binding_lanes: set[str] = set()
        for row_index, raw_binding in enumerate(binding_rows):
            binding = _require_object(
                raw_binding, f"fixture {fixture_id} binding witness {row_index}"
            )
            _exact_keys(
                binding,
                {"lane_id", "case_ids"},
                f"fixture {fixture_id} binding witness {row_index}",
            )
            lane_id = _require_text(
                binding["lane_id"], f"fixture {fixture_id} binding lane"
            )
            lane_contract = binding_contracts.get(lane_id)
            case_ids = _unique_text_set(
                binding["case_ids"], f"fixture {fixture_id} binding case IDs"
            )
            if (
                lane_id in binding_lanes
                or lane_contract is None
                or not case_ids
                or not case_ids.issubset(set(lane_contract["required_case_ids"]))
            ):
                fail(f"fixture {fixture_id} has an invalid binding witness")
            binding_lanes.add(lane_id)
            derived_lanes.add(lane_id)

        floor_lanes = _unique_text_set(
            fixture["python_binding_floor_lanes"],
            f"fixture {fixture_id} Python binding floor lanes",
        )
        for lane_id in floor_lanes:
            lane_contract = binding_contracts.get(lane_id)
            if lane_contract is None or lane_contract["require_python_baseline"] is not True:
                fail(f"fixture {fixture_id} references a lane without the Python floor")
            derived_lanes.add(lane_id)

        scan_ids = _unique_text_set(
            fixture["scan_ids"], f"fixture {fixture_id} scan IDs"
        )
        if not scan_ids.issubset(REQUIRED_SCAN_IDS):
            fail(f"fixture {fixture_id} references an unknown forbidden scan")
        if scan_ids:
            derived_lanes.add("forbidden_surface_scan")

        determinism_artifacts = _unique_text_set(
            fixture["determinism_artifacts"],
            f"fixture {fixture_id} determinism artifacts",
        )
        for value in determinism_artifacts:
            relative = _safe_relative_path(
                value, f"fixture {fixture_id} determinism artifact"
            )
            if relative.as_posix() not in allowed_determinism_artifacts:
                fail(f"fixture {fixture_id} references an unknown determinism artifact")
        if determinism_artifacts:
            derived_lanes.add("deterministic_rebuild")

        generated_roles = _unique_text_set(
            fixture["generated_output_roles"],
            f"fixture {fixture_id} generated output roles",
        )
        if not generated_roles.issubset(GENERATED_OUTPUT_LANES):
            fail(f"fixture {fixture_id} references an unknown generated output role")
        derived_lanes.update(GENERATED_OUTPUT_LANES[role] for role in generated_roles)

        expected_lanes = sorted(derived_lanes, key=lane_ordinals.__getitem__)
        if raw_evidence_lanes != expected_lanes:
            fail(f"fixture {fixture_id} evidence lanes do not match its exact witnesses")
        if not any(
            (
                native_rows,
                binding_rows,
                floor_lanes,
                scan_ids,
                determinism_artifacts,
                generated_roles,
            )
        ):
            fail(f"fixture {fixture_id} contains no executable witness")
    if seen != REQUIRED_FIXTURE_IDS:
        fail("fixture requirement inventory differs from the frozen I2 matrix")


def _validate_rotating_identities(value: Any) -> None:
    rotating = _require_object(value, "rotating_identities")
    _exact_keys(
        rotating,
        {
            "baseline",
            "rule",
            "candidate_expectations",
            "determinism_artifact_roots",
            "identity_artifact",
            "fixture_ir_outputs",
            "regression_products",
        },
        "rotating_identities",
    )
    baseline = _require_object(rotating["baseline"], "rotating_identities.baseline")
    if baseline != REQUIRED_ROTATING_BASELINE:
        fail("rotating identity baseline differs from the frozen pre-I2 values")
    _require_text(rotating["rule"], "rotating_identities.rule")
    if rotating["candidate_expectations"] != {
        "node_environment_digest": "unchanged",
        "compiler_contract_digest": "changed",
        "release_compiler_build_digest": "changed",
        "canonical_empty_retained_bundle_id": "changed",
        "canonical_empty_admission_digest": "changed",
    }:
        fail("candidate identity-rotation expectations drifted")
    if rotating["determinism_artifact_roots"] != [
        "determinism/build-a",
        "determinism/build-b",
    ]:
        fail("determinism artifact roots drifted")
    if rotating["identity_artifact"] != REQUIRED_IDENTITY_ARTIFACT:
        fail("determinism identity-artifact mapping drifted")
    if rotating["fixture_ir_outputs"] != REQUIRED_FIXTURE_IR_OUTPUTS:
        fail("determinism fixture-IR output mapping drifted")
    products = _require_list(
        rotating["regression_products"], "rotating_identities.regression_products"
    )
    if len(products) != len(set(products)) or set(products) != REQUIRED_REGRESSION_PRODUCTS:
        fail("regression-product inventory differs from the frozen set")


def _load_baseline_test_inventory(
    contract: dict[str, Any], manifest: dict[str, Any], repository_root: Path
) -> tuple[dict[str, set[str]], dict[str, int]]:
    declared_path = _safe_relative_path(
        _require_text(
            contract["baseline_inventory_path"],
            "test_inventory_contract.baseline_inventory_path",
        ),
        "test_inventory_contract.baseline_inventory_path",
    )
    if declared_path != BASELINE_TEST_INVENTORY_PATH:
        fail("baseline test inventory path drifted")
    inventory_path = _require_contained_regular_file(
        repository_root / declared_path,
        repository_root,
        "baseline test inventory",
    )
    declared_digest = _require_text(
        contract["baseline_inventory_sha256"],
        "test_inventory_contract.baseline_inventory_sha256",
    )
    if (
        declared_digest != BASELINE_TEST_INVENTORY_SHA256
        or _sha256_file(inventory_path) != declared_digest
    ):
        fail("baseline test inventory digest drifted")
    inventory = load_manifest(inventory_path)
    _exact_keys(
        inventory,
        {
            "schema_version",
            "implementation_base",
            "digest_framing",
            "python_binding_function_floor",
            "witness_groups",
            "groups",
        },
        "baseline test inventory",
    )
    if inventory["schema_version"] != BASELINE_TEST_INVENTORY_SCHEMA:
        fail("baseline test inventory schema drifted")
    expected_base = {
        "head_commit": manifest["implementation_base"]["manyforge"]["source_identity"][
            "head_commit"
        ],
        "head_tree": manifest["implementation_base"]["manyforge"]["source_identity"][
            "head_tree"
        ],
    }
    if inventory["implementation_base"] != expected_base:
        fail("baseline test inventory is not bound to the implementation base")
    if inventory["digest_framing"] != {
        "ordering": "ascending unsigned UTF-8 byte order",
        "uniqueness": "exact CTest name bytes; duplicates are forbidden",
        "payload": "each sorted name followed by one LF byte, including the final name",
        "algorithm": "SHA-256 encoded as 64 lowercase hexadecimal digits",
    }:
        fail("baseline test-name digest framing drifted")
    python_floor = _require_object(
        inventory["python_binding_function_floor"],
        "Python binding function floor",
    )
    _exact_keys(
        python_floor,
        {
            "count",
            "minimum_total_collected_cases",
            "entries_sha256",
            "digest_payload",
            "entries",
        },
        "Python binding function floor",
    )
    if (
        python_floor["count"] != PYTHON_BINDING_FUNCTION_COUNT
        or python_floor["minimum_total_collected_cases"]
        != PYTHON_BINDING_MINIMUM_CASE_COUNT
        or python_floor["entries_sha256"] != PYTHON_BINDING_FUNCTION_DIGEST
        or python_floor["digest_payload"]
        != "each sorted node_prefix, one HT byte, decimal minimum count, and one LF byte"
    ):
        fail("Python binding function floor metadata drifted")
    python_entries = _require_list(
        python_floor["entries"], "Python binding function floor entries"
    )
    python_functions: dict[str, int] = {}
    for index, raw in enumerate(python_entries):
        row = _require_object(raw, f"Python binding floor entry {index}")
        _exact_keys(
            row,
            {"node_prefix", "minimum_collected_cases"},
            f"Python binding floor entry {index}",
        )
        prefix = _require_text(
            row["node_prefix"], f"Python binding floor entry {index} node prefix"
        )
        minimum = row["minimum_collected_cases"]
        if (
            prefix in python_functions
            or not prefix.startswith("manyforge_core/python/tests/test_")
            or "::test_" not in prefix
            or not isinstance(minimum, int)
            or isinstance(minimum, bool)
            or minimum < 1
        ):
            fail(f"Python binding floor entry is malformed: {prefix}")
        python_functions[prefix] = minimum
    if list(python_functions) != sorted(python_functions):
        fail("Python binding function floor is not bytewise sorted")
    payload = "".join(
        f"{prefix}\t{minimum}\n"
        for prefix, minimum in python_functions.items()
    ).encode()
    if (
        len(python_functions) != PYTHON_BINDING_FUNCTION_COUNT
        or sum(python_functions.values()) != PYTHON_BINDING_MINIMUM_CASE_COUNT
        or hashlib.sha256(payload).hexdigest() != PYTHON_BINDING_FUNCTION_DIGEST
    ):
        fail("Python binding function floor count or digest drifted")
    decoded_groups: dict[str, set[str]] = {}
    for table_key, expected_groups in (
        ("groups", REQUIRED_TEST_GROUPS),
        ("witness_groups", REQUIRED_WITNESS_GROUPS),
    ):
        table = _require_object(
            inventory[table_key], f"baseline test inventory {table_key}"
        )
        if set(table) != set(expected_groups):
            fail(f"baseline test inventory {table_key} set drifted")
        for group_id, (expected_count, expected_digest) in expected_groups.items():
            group = _require_object(table[group_id], f"test group {group_id}")
            _exact_keys(
                group,
                {"names", "count", "names_sha256"},
                f"test group {group_id}",
            )
            names = _require_list(group["names"], f"test group {group_id} names")
            if not names or not all(
                isinstance(name, str) and name and name.isascii() for name in names
            ):
                fail(f"test group {group_id} names must be non-empty ASCII strings")
            if names != sorted(names, key=lambda value: value.encode("utf-8")):
                fail(f"test group {group_id} names are not bytewise sorted")
            if len(names) != len(set(names)):
                fail(f"test group {group_id} contains duplicate names")
            payload = "".join(f"{name}\n" for name in names).encode()
            digest = hashlib.sha256(payload).hexdigest()
            if (
                group["count"] != expected_count
                or len(names) != expected_count
                or group["names_sha256"] != expected_digest
                or digest != expected_digest
            ):
                fail(f"test group {group_id} count or digest drifted")
            decoded_groups[group_id] = set(names)
    if not REQUIRED_CONSUMER_TESTS.issubset(decoded_groups["roboplan_consumer_required"]):
        fail("retained consumer tests are absent from the frozen test baseline")
    if not decoded_groups["c02_tail_required"].issubset(
        decoded_groups["deployment_release_baseline"]
    ):
        fail("focused C0.2 tail is not a subset of the Deployment baseline")
    if GOLDEN_RELEASE_TEST not in decoded_groups["deployment_release_baseline"]:
        fail("Release-golden cross-lane proof test is absent from the baseline")
    c02_tail = decoded_groups["c02_tail_required"]
    for group_id in REQUIRED_WITNESS_GROUPS:
        if not decoded_groups[group_id].issubset(c02_tail):
            fail(f"fixture witness group escapes the frozen C0.2 tail: {group_id}")
    return decoded_groups, python_functions


def _load_native_target_contract(
    contract: dict[str, Any], manifest: dict[str, Any], repository_root: Path
) -> tuple[
    dict[str, dict[str, tuple[int, str]]],
    dict[str, tuple[str, str, str]],
]:
    declared_path = _safe_relative_path(
        _require_text(
            contract["native_target_contract_path"],
            "test_inventory_contract.native_target_contract_path",
        ),
        "test_inventory_contract.native_target_contract_path",
    )
    declared_digest = _require_text(
        contract["native_target_contract_sha256"],
        "test_inventory_contract.native_target_contract_sha256",
    )
    if (
        declared_path != NATIVE_TARGET_CONTRACT_PATH
        or declared_digest != NATIVE_TARGET_CONTRACT_SHA256
    ):
        fail("native target contract path/digest declaration drifted")
    path = _require_contained_regular_file(
        repository_root / declared_path,
        repository_root,
        "native target contract",
        require_single_link=True,
    )
    if _sha256_file(path) != declared_digest:
        fail("native target contract bytes drifted")
    value = load_manifest(path)
    _exact_keys(
        value,
        {
            "schema_version",
            "implementation_base",
            "gtest_source_rule",
            "target_groups",
            "target_group_digests",
            "ordinary_test_sources",
        },
        "native target contract",
    )
    if (
        value["schema_version"]
        != "manyforge.composable_cell_i2.native_target_contract.v1"
        or value["implementation_base"]
        != {
            "head_commit": manifest["implementation_base"]["manyforge"][
                "source_identity"
            ]["head_commit"],
            "head_tree": manifest["implementation_base"]["manyforge"][
                "source_identity"
            ]["head_tree"],
        }
        or value["gtest_source_rule"]
        != (
            "each frozen target has exactly one candidate-snapshot source at "
            "<package>/test/<target>.cpp"
        )
        or value["target_group_digests"] != NATIVE_TARGET_GROUP_DIGESTS
    ):
        fail("native target contract identity drifted")
    raw_groups = _require_object(value["target_groups"], "native target groups")
    if set(raw_groups) != set(NATIVE_TARGET_GROUP_DIGESTS):
        fail("native target group set drifted")
    package_by_group = {
        "core_release_baseline": "manyforge_core",
        "deployment_release_baseline": "manyforge_deployment",
        "c02_tail_required": "manyforge_deployment",
        "roboplan_release_baseline": "manyforge_roboplan",
        "roboplan_consumer_required": "manyforge_roboplan",
        "roboplan_scene_sanitizer_required": "manyforge_roboplan",
    }
    expected_counts = {
        "core_release_baseline": 1101,
        "deployment_release_baseline": 454,
        "c02_tail_required": 147,
        "roboplan_release_baseline": 54,
        "roboplan_consumer_required": 3,
        "roboplan_scene_sanitizer_required": 5,
    }
    groups: dict[str, dict[str, tuple[int, str]]] = {}
    for group_id, raw_rows in raw_groups.items():
        rows = _require_list(raw_rows, f"native target group {group_id}")
        decoded: dict[str, tuple[int, str]] = {}
        canonical: dict[str, dict[str, Any]] = {}
        for index, raw in enumerate(rows):
            row = _require_list(raw, f"native target group {group_id}[{index}]")
            if len(row) != 3:
                fail(f"native target row width drifted: {group_id}[{index}]")
            target, count, digest = row
            if (
                not isinstance(target, str)
                or not re.fullmatch(r"test_[a-z0-9_]+", target)
                or target in decoded
                or not _positive_int(count)
                or not isinstance(digest, str)
                or not HEX64.fullmatch(digest)
            ):
                fail(f"native target row is malformed: {group_id}[{index}]")
            decoded[target] = (count, digest)
            canonical[target] = {"count": count, "names_sha256": digest}
            source_relative = (
                f"{package_by_group[group_id]}/test/{target}.cpp"
            )
            _git_tree_entry(
                repository_root,
                manifest["implementation_base"]["manyforge"]["source_identity"][
                    "head_commit"
                ],
                source_relative,
            )
        if list(decoded) != sorted(decoded):
            fail(f"native target group is not target-sorted: {group_id}")
        if (
            sum(count for count, _ in decoded.values()) != expected_counts[group_id]
            or _canonical_json_sha256(canonical)
            != NATIVE_TARGET_GROUP_DIGESTS[group_id]
        ):
            fail(f"native target group count/digest drifted: {group_id}")
        groups[group_id] = decoded
    ordinary_raw = _require_object(
        value["ordinary_test_sources"], "ordinary native test sources"
    )
    expected_ordinary = {
        "BuildManifestAuthorityRejectsMultiConfigGenerator": (
            "/workspace/manyforge_deployment/test/reject_multi_config.cmake",
            "2518188271c056e43c2022cf662bcfa8df8521e1f35d95999cc54a1a325a2866",
        ),
        "CanonicalEmptyGeneratorCli": (
            "/workspace/manyforge_deployment/test/canonical_empty_generator_cli.cmake",
            "11b39e29743852e09c53d06832a2a249c440780dae19242e6532441d5911f839",
        ),
        "BuildQualifiedAdmissionProfileIsDeterministic": (
            "/workspace/manyforge_deployment/test/build_qualified_admission_profile_determinism.cmake",
            "056d2cb22c2c0b565517f89573420a446d28a0921e2c347fb9d718e6bec10951",
        ),
        "retained_compiled_visual_fk_composer_consumer": (
            "/workspace/manyforge_composer/tests/qualify_retained_compiled_visual_fk.py",
            "8d494f14a171432a7162260edfda0495067cb07de6283a357299a46ba5578569",
        ),
        "retained_compiled_model_independent_standard_parser": (
            "/workspace/manyforge_deployment/test/qualify_retained_model_standard_parsers.py",
            "dfa47407d13653e48bdbac5ae29e1ecc6decdf83820c09fa98990858da864162",
        ),
    }
    ordinary_baselines: dict[str, tuple[str, str]] = {}
    for name, raw in ordinary_raw.items():
        row = _require_list(raw, f"ordinary test source {name}")
        if len(row) != 2 or not all(isinstance(item, str) for item in row):
            fail(f"ordinary test source row is malformed: {name}")
        ordinary_baselines[name] = (row[0], row[1])
    if ordinary_baselines != expected_ordinary:
        fail("ordinary native test source contract drifted")
    ordinary: dict[str, tuple[str, str, str]] = {}
    for name, (container_path, digest) in ordinary_baselines.items():
        relative = Path(container_path).relative_to("/workspace")
        source = _require_contained_regular_file(
            repository_root / relative,
            repository_root,
            f"ordinary test source {name}",
            require_single_link=True,
        )
        baseline = _git_blob(
            repository_root,
            manifest["implementation_base"]["manyforge"]["source_identity"][
                "head_commit"
            ],
            relative.as_posix(),
        )
        if hashlib.sha256(baseline).hexdigest() != digest:
            fail(f"ordinary native test baseline digest drifted: {name}")
        ordinary[name] = (container_path, digest, _sha256_file(source))
    return groups, ordinary


def _load_build_provenance_contract(
    contract: dict[str, Any], manifest: dict[str, Any], repository_root: Path
) -> dict[str, Any]:
    """Load the one hash-bound authority for native/product/binding build shape."""

    declared_path = _safe_relative_path(
        _require_text(
            contract["build_provenance_contract_path"],
            "test_inventory_contract.build_provenance_contract_path",
        ),
        "test_inventory_contract.build_provenance_contract_path",
    )
    declared_digest = _require_text(
        contract["build_provenance_contract_sha256"],
        "test_inventory_contract.build_provenance_contract_sha256",
    )
    if (
        declared_path != BUILD_PROVENANCE_CONTRACT_PATH
        or declared_digest != BUILD_PROVENANCE_CONTRACT_SHA256
    ):
        fail("build-provenance contract path/digest declaration drifted")
    path = _require_contained_regular_file(
        repository_root / declared_path,
        repository_root,
        "build-provenance contract",
        require_single_link=True,
    )
    if _sha256_file(path) != declared_digest:
        fail("build-provenance contract bytes drifted")
    value = load_manifest(path)
    _exact_keys(
        value,
        {
            "schema_version",
            "implementation_base",
            "closed_shape_policy",
            "toolchain_contract",
            "production_exclusions",
            "configure_generated_input_contracts",
            "configure_executed_fixture_contracts",
            "configured_target_graph_contract",
            "cross_profile_parity_contract",
            "auxiliary_install_contract",
            "projects",
            "native_lane_target_partitions",
            "production_release_parity_lanes",
            "utility_target_contracts",
            "binding_build_contract",
            "binding_builder_constraints",
        },
        "build-provenance contract",
    )
    expected_base = {
        "commit": manifest["implementation_base"]["manyforge"]["source_identity"][
            "head_commit"
        ],
        "tree": manifest["implementation_base"]["manyforge"]["source_identity"][
            "head_tree"
        ],
    }
    if (
        value["schema_version"]
        != "manyforge.composable_cell_i2.build_provenance_contract.v1"
        or value["implementation_base"] != expected_base
        or value["closed_shape_policy"]
        != {
            "all_objects_reject_unknown_members": True,
            "all_arrays_have_exact_membership": True,
            "all_paths_are_normalized_repository_or_build_relative_posix_paths": True,
            "duplicate_json_members_are_forbidden": True,
            "target_and_source_arrays_are_lexicographically_ordered": True,
        }
    ):
        fail("build-provenance contract identity or closed-shape policy drifted")
    try:
        validate_build_provenance_contract(
            value, repository_root, reify_external_headers=False
        )
    except NativeBuildProvenanceError as error:
        fail(f"build-provenance contract deep semantic validation failed: {error}")
    toolchain = _require_object(
        value["toolchain_contract"], "build-provenance toolchain contract"
    )
    _exact_keys(
        toolchain,
        {
            "schema_version",
            "cmake",
            "git",
            "python",
            "cxx_compiler",
            "archiver",
            "ranlib",
            "ninja",
            "compile_modes",
        },
        "build-provenance toolchain contract",
    )
    if toolchain != {
        "schema_version": "manyforge.composable_cell_i2.toolchain_contract.v1",
        "cmake": {
            "lexical_path": "/usr/bin/cmake",
            "resolved_path": "/usr/bin/cmake",
            "sha256": "bad2e2bae7a1cc2c885d1aa06f19ae91be6684819aaeaf03f89410cf4854ecea",
            "version_first_line": "cmake version 3.25.1",
        },
        "git": {
            "lexical_path": "/usr/bin/git",
            "resolved_path": "/usr/bin/git",
            "sha256": "2540879925a6881e3877ff7e3330746ba3027b04edf16a3a12dccd1644c4f32d",
            "version_first_line": "git version 2.39.5",
        },
        "python": {
            "lexical_path": "/usr/local/bin/python3",
            "resolved_path": "/usr/local/bin/python3.12",
            "sha256": "4dbf3143240288fb2170257ffaa7bd030cdda5d2703d1f5f30b627042267e2e3",
            "version_first_line": "Python 3.12.13",
        },
        "cxx_compiler": {
            "lexical_path": "/usr/bin/c++",
            "resolved_path": "/usr/bin/x86_64-linux-gnu-g++-12",
            "sha256": "dd91977c184e327710578363ad93ebb175c3a457b6236b874fd3911b7c055c65",
            "version_first_line": "c++ (Debian 12.2.0-14+deb12u1) 12.2.0",
        },
        "archiver": {
            "lexical_path": "/usr/bin/ar",
            "resolved_path": "/usr/bin/x86_64-linux-gnu-ar",
            "sha256": "3acbee2794e3668a74bcb90f2eaf7d981211fb95288ab940f0e3ac380e8f6023",
        },
        "ranlib": {
            "lexical_path": "/usr/bin/ranlib",
            "resolved_path": "/usr/bin/x86_64-linux-gnu-ranlib",
            "sha256": "a502e11f3c9aac3554493cd4827fd488f9a8f3da8bcd4580b2c3172607bd51f6",
        },
        "ninja": {
            "lexical_path": "/usr/bin/ninja",
            "resolved_path": "/usr/bin/ninja",
            "sha256": "cb1f22b33e58fc69c47728e751b6ff6f5a1d78d60f46d734c02076dc90a78b19",
        },
        "compile_modes": {
            "cxx_standard_flag": "-std=c++20",
            "project_source_required_flags": {
                "manyforge_roboplan": {
                    "-fopenmp": [
                        "src/cartesian_planning_contract.cpp",
                        "src/joint_cap_contract.cpp",
                        "src/roboplan_cartesian_trajectory_dynamics.cpp",
                        "test/test_cartesian_planning_contract.cpp",
                        "test/test_joint_cap_contract.cpp",
                        "test/test_retained_product_consumer.cpp",
                        "test/test_roboplan_planner_options.cpp",
                        "test/test_roboplan_scene_generation.cpp",
                    ]
                },
                "roboplan": {
                    "-fopenmp": [
                        "src/core/collision_context.cpp",
                        "src/core/path_utils.cpp",
                        "src/core/pose_utils.cpp",
                        "src/core/scene.cpp",
                        "src/core/scene_utils.cpp",
                        "src/core/types.cpp",
                        "src/filters/se3_low_pass_filter.cpp",
                    ]
                },
            },
            "Release": {
                "optimization_flag": "-O3",
                "debug_flag": None,
                "require_ndebug": True,
            },
            "RelWithDebInfo": {
                "optimization_flag": "-O2",
                "debug_flag": "-g",
                "require_ndebug": True,
            },
        },
    }:
        fail("build-provenance toolchain/compile-mode contract drifted")
    exclusions = _require_object(
        value["production_exclusions"], "build-provenance production exclusions"
    )
    _exact_keys(
        exclusions,
        {
            "forbidden_target_name_patterns",
            "forbidden_repository_source_path_patterns",
            "forbidden_dependency_name_patterns",
        },
        "build-provenance production exclusions",
    )
    if exclusions != {
        "forbidden_target_name_patterns": [r"^perf_.*$", r"^test_.*$"],
        "forbidden_repository_source_path_patterns": [
            r"(^|/)test(/|$)",
            r"(^|/)tests(/|$)",
        ],
        "forbidden_dependency_name_patterns": [
            r"(^|::)GTest(::|$)",
            r"(^|::)[A-Za-z0-9_.+-]*test_support[A-Za-z0-9_.+-]*(::|$)",
            r"(?i)gmock",
            r"(?i)gtest",
        ],
    }:
        fail("build-provenance production exclusion regexes drifted")
    projects = _require_object(value["projects"], "build-provenance projects")
    if set(projects) != {"manyforge_core", "manyforge_deployment", "manyforge_roboplan"}:
        fail("build-provenance project set drifted")
    partitions = _require_object(
        value["native_lane_target_partitions"],
        "native lane target partitions",
    )
    _exact_keys(
        partitions,
        {"schema_version", "lanes"},
        "native lane target partitions",
    )
    partition_lanes = _require_object(
        partitions["lanes"], "native lane target partition rows"
    )
    if (
        partitions["schema_version"]
        != "manyforge.composable_cell_i2.native_lane_target_partitions.v1"
        or set(partition_lanes) != NATIVE_EXECUTION_LANES
    ):
        fail("native lane target partitions do not cover the exact lane set")
    for lane_id, raw in partition_lanes.items():
        row = _require_object(raw, f"native target partition {lane_id}")
        _exact_keys(
            row,
            {"production_targets", "test_only_targets"},
            f"native target partition {lane_id}",
        )
        for field in ("production_targets", "test_only_targets"):
            names = _require_list(row[field], f"native target partition {lane_id}.{field}")
            if (
                not all(isinstance(name, str) and name for name in names)
                or names != sorted(set(names))
            ):
                fail(f"native target partition {lane_id}.{field} is not exact and sorted")
    parity = _require_list(
        value["production_release_parity_lanes"],
        "production Release parity lanes",
    )
    if parity != [
        {
            "production_lane_id": "core_production_release",
            "project_id": "manyforge_core",
            "test_enabled_release_lane_id": "core_release",
        },
        {
            "production_lane_id": "deployment_production_release",
            "project_id": "manyforge_deployment",
            "test_enabled_release_lane_id": "deployment_release",
        },
    ]:
        fail("production Release parity lane contract drifted")
    binding_build = _require_object(
        value["binding_build_contract"], "binding build contract"
    )
    binding_constraints = _require_object(
        value["binding_builder_constraints"], "binding builder constraints"
    )
    if (
        binding_build.get("schema_version")
        != "manyforge.composable_cell_i2.binding_build_contract.v1"
        or binding_constraints.get("schema_version")
        != "manyforge.composable_cell_i2.binding_builder_constraints.v1"
    ):
        fail("binding build-provenance sub-contract schema drifted")
    return value


def _validate_test_inventory_contract(
    value: Any,
    manifest: dict[str, Any],
    lanes: dict[str, dict[str, Any]],
    repository_root: Path,
) -> None:
    contract = _require_object(value, "test_inventory_contract")
    _exact_keys(
        contract,
        {
            "baseline_inventory_path",
            "baseline_inventory_sha256",
            "native_target_contract_path",
            "native_target_contract_sha256",
            "build_provenance_contract_path",
            "build_provenance_contract_sha256",
            "native_execution_lanes",
            "binding_result_lanes",
            "binding_build_lanes",
            "production_build_lanes",
            "cross_lane_exclusion_proofs",
        },
        "test_inventory_contract",
    )
    _load_baseline_test_inventory(contract, manifest, repository_root)
    _load_native_target_contract(contract, manifest, repository_root)
    _load_build_provenance_contract(contract, manifest, repository_root)

    expected_native = {
        "core_release": (
            "core_release_baseline",
            "core-release/ctest-inventory.json",
            "core-release/execution-index.json",
            "core-release/native-build-provenance.json",
            "build/core-release",
            "none",
            None,
            120,
            None,
            True,
            False,
            [],
        ),
        "deployment_release": (
            "deployment_release_baseline",
            "deployment-release/ctest-inventory.json",
            "deployment-release/execution-index.json",
            "deployment-release/native-build-provenance.json",
            "build/deployment-release",
            "none",
            None,
            120,
            None,
            True,
            False,
            [],
        ),
        "c02_tail_focused_release": (
            "c02_tail_required",
            "c02-tail/ctest-inventory.json",
            "c02-tail/execution-index.json",
            "c02-tail/native-build-provenance.json",
            "build/c02-tail-release",
            "none",
            None,
            180,
            (
                r"^(Mf2JointName|CellComposition|CellApplicability|"
                r"CellApplicabilityBudget|DeploymentApplicabilityCodec|"
                r"SiteOverlayResolution|CellCompiler|BuildManifestAuthority|"
                r"RetentionQualifiedDeployment)\."
            ),
            False,
            False,
            [],
        ),
        "roboplan_release": (
            "roboplan_release_baseline",
            "roboplan-release/ctest-inventory.json",
            "roboplan-release/execution-index.json",
            "roboplan-release/native-build-provenance.json",
            "build/roboplan-release/roboplan",
            "none",
            None,
            180,
            None,
            True,
            False,
            [],
        ),
        "roboplan_consumer_focused": (
            "roboplan_consumer_required",
            "roboplan-focused/ctest-inventory.json",
            "roboplan-focused/execution-index.json",
            "roboplan-focused/native-build-provenance.json",
            "build/roboplan-focused/roboplan",
            "none",
            None,
            180,
            (
                r"^(RetainedProductRoboPlanConsumer\.|"
                r"retained_compiled_visual_fk_composer_consumer$|"
                r"retained_compiled_model_independent_standard_parser$)"
            ),
            False,
            False,
            [],
        ),
        "core_asan_ubsan": (
            "core_release_baseline",
            "core-asan/ctest-inventory.json",
            "core-asan/execution-index.json",
            "core-asan/native-build-provenance.json",
            "build/core-asan",
            "address+undefined",
            "-fsanitize=address,undefined",
            300,
            None,
            True,
            False,
            [],
        ),
        "core_tsan": (
            "core_release_baseline",
            "core-tsan/ctest-inventory.json",
            "core-tsan/execution-index.json",
            "core-tsan/native-build-provenance.json",
            "build/core-tsan",
            "thread",
            "-fsanitize=thread",
            300,
            None,
            True,
            False,
            [],
        ),
        "deployment_asan_ubsan": (
            "deployment_release_baseline",
            "deployment-asan/ctest-inventory.json",
            "deployment-asan/execution-index.json",
            "deployment-asan/native-build-provenance.json",
            "build/deployment-asan",
            "address+undefined",
            "-fsanitize=address,undefined",
            300,
            None,
            True,
            False,
            [GOLDEN_RELEASE_TEST],
        ),
        "roboplan_asan_ubsan": (
            "roboplan_scene_sanitizer_required",
            "roboplan-asan/ctest-inventory.json",
            "roboplan-asan/execution-index.json",
            "roboplan-asan/native-build-provenance.json",
            "build/roboplan-asan/build/manyforge_roboplan",
            "address+undefined",
            "-fsanitize=address,undefined",
            300,
            r"^RoboPlanSceneGenerationTest\.",
            False,
            False,
            [],
        ),
        "roboplan_tsan": (
            "roboplan_scene_sanitizer_required",
            "roboplan-tsan/ctest-inventory.json",
            "roboplan-tsan/execution-index.json",
            "roboplan-tsan/native-build-provenance.json",
            "build/roboplan-tsan/build/manyforge_roboplan",
            "thread",
            "-fsanitize=thread",
            300,
            r"^RoboPlanSceneGenerationTest\.",
            False,
            False,
            [],
        ),
    }
    native_rows = _require_list(
        contract["native_execution_lanes"], "native execution lanes"
    )
    seen: set[str] = set()
    for index, raw in enumerate(native_rows):
        row = _require_object(raw, f"native_execution_lanes[{index}]")
        _exact_keys(
            row,
            {
                "lane_id",
                "required_group",
                "inventory_output",
                "execution_index_output",
                "provenance_output",
                "build_root",
                "sanitizer",
                "required_flag",
                "timeout_ceiling",
                "test_regex",
                "allow_additional_tests",
                "allow_additional_targets",
                "allowed_exclusions",
            },
            f"native_execution_lanes[{index}]",
        )
        lane_id = row["lane_id"]
        if lane_id in seen or lane_id not in expected_native:
            fail(f"unexpected or duplicate native-execution lane: {lane_id}")
        seen.add(lane_id)
        if not isinstance(row["allow_additional_tests"], bool):
            fail(f"native-execution additional-test policy is not boolean: {lane_id}")
        if row["allow_additional_targets"] is not False:
            fail(f"native-execution lane permits an unqualified target: {lane_id}")
        actual = (
            row["required_group"],
            row["inventory_output"],
            row["execution_index_output"],
            row["provenance_output"],
            row["build_root"],
            row["sanitizer"],
            row["required_flag"],
            row["timeout_ceiling"],
            row["test_regex"],
            row["allow_additional_tests"],
            row["allow_additional_targets"],
            row["allowed_exclusions"],
        )
        if actual != expected_native[lane_id]:
            fail(f"native-execution contract drifted for lane {lane_id}")
        for output in (
            row["inventory_output"],
            row["execution_index_output"],
            row["provenance_output"],
        ):
            _safe_relative_path(output, f"native result for {lane_id}")
            if output not in lanes[lane_id]["expected_outputs"]:
                fail(f"native result is not lane-required: {lane_id}: {output}")
        _safe_relative_path(row["build_root"], f"native build root for {lane_id}")
    if seen != NATIVE_EXECUTION_LANES:
        fail("native-execution lane set differs from the frozen contract")

    expected_binding_outputs: dict[
        str, tuple[str, str, str, set[str], bool]
    ] = {
        "bindings_selector": (
            "bindings-selector/collection.txt",
            "bindings-selector/junit.xml",
            "bindings-selector/witness.json",
            REQUIRED_BINDING_CASES,
            True,
        ),
        "bindings_release": (
            "bindings-release/collection.txt",
            "bindings-release/junit.xml",
            "bindings-release/witness.json",
            REQUIRED_BINDING_CASES,
            True,
        ),
        "bindings_asan_ubsan": (
            "bindings-asan/collection.txt",
            "bindings-asan/junit.xml",
            "bindings-asan/witness.json",
            {
                        "detached_predecessor_and_final_value_lifetime",
                        "exception_translation_after_released_gil",
                        "repeated_fresh_interpreter_import_use_and_clean_shutdown",
                    },
            False,
        ),
        "bindings_tsan": (
            "bindings-tsan/collection.txt",
            "bindings-tsan/junit.xml",
            "bindings-tsan/witness.json",
            {"concurrent_pure_compiles_are_deterministic"},
            False,
        ),
        "bindings_import": (
            "bindings-contract/import.collection.txt",
            "bindings-contract/import.junit.xml",
            "bindings-contract/import.witness.json",
            {"ros_free_import_and_sole_module_inventory"},
            False,
        ),
        "bindings_parity": (
            "bindings-contract/parity.collection.txt",
            "bindings-contract/parity.junit.xml",
            "bindings-contract/parity.witness.json",
            {
                        "cpp_python_field_enum_default_ordering_and_error_parity",
                        "final_compiler_projection_nonconstructible_and_read_only",
                        "all_trusted_predecessor_projections_nonconstructible_and_read_only",
                    },
            False,
        ),
        "bindings_lifetime": (
            "bindings-contract/lifetime.collection.txt",
            "bindings-contract/lifetime.junit.xml",
            "bindings-contract/lifetime.witness.json",
            {"detached_predecessor_and_final_value_lifetime"},
            False,
        ),
        "bindings_exception": (
            "bindings-contract/exception.collection.txt",
            "bindings-contract/exception.junit.xml",
            "bindings-contract/exception.witness.json",
            {"exception_translation_after_released_gil"},
            False,
        ),
        "bindings_gil_concurrency": (
            "bindings-contract/gil-concurrency.collection.txt",
            "bindings-contract/gil-concurrency.junit.xml",
            "bindings-contract/gil-concurrency.witness.json",
            {
                        "blocking_compile_releases_gil_with_progress_witness",
                        "concurrent_pure_compiles_are_deterministic",
                    },
            False,
        ),
        "bindings_shutdown": (
            "bindings-contract/shutdown.collection.txt",
            "bindings-contract/shutdown.junit.xml",
            "bindings-contract/shutdown.witness.json",
            {"repeated_fresh_interpreter_import_use_and_clean_shutdown"},
            False,
        ),
        "bindings_generated_api": (
            "bindings-contract/generated-api.collection.txt",
            "bindings-contract/generated-api.junit.xml",
            "bindings-contract/generated-api.witness.json",
            {
                        "generated_stub_inventory_and_documentation_parity",
                        "complete_forbidden_surface_absence",
                    },
            False,
        ),
    }
    binding_rows = _require_list(contract["binding_result_lanes"], "binding result lanes")
    seen = set()
    for index, raw in enumerate(binding_rows):
        row = _require_object(raw, f"binding_result_lanes[{index}]")
        _exact_keys(
            row,
            {
                "lane_id",
                "producer_lane_id",
                "build_provenance_output",
                "collection_output",
                "result_output",
                "witness_output",
                "require_python_baseline",
                "required_case_ids",
            },
            f"binding_result_lanes[{index}]",
        )
        lane_id = row["lane_id"]
        expected_binding = expected_binding_outputs.get(lane_id)
        case_ids = _require_list(
            row["required_case_ids"], f"binding required cases for {lane_id}"
        )
        actual = (
            row["collection_output"],
            row["result_output"],
            row["witness_output"],
            set(case_ids),
            row["require_python_baseline"],
        )
        if (
            lane_id in seen
            or expected_binding is None
            or len(case_ids) != len(set(case_ids))
            or actual != expected_binding
        ):
            fail(f"unexpected, duplicate, or changed binding-result lane: {lane_id}")
        expected_producer = (
            lane_id
            if lane_id
            in {
                "bindings_selector",
                "bindings_release",
                "bindings_asan_ubsan",
                "bindings_tsan",
            }
            else "bindings_release"
        )
        expected_provenance = {
            "bindings_selector": "bindings-selector/build-provenance.json",
            "bindings_release": "bindings-release/build-provenance.json",
            "bindings_asan_ubsan": "bindings-asan/build-provenance.json",
            "bindings_tsan": "bindings-tsan/build-provenance.json",
        }.get(lane_id)
        if (
            row["producer_lane_id"] != expected_producer
            or row["build_provenance_output"] != expected_provenance
        ):
            fail(f"binding build-provenance routing drifted for {lane_id}")
        seen.add(lane_id)
        for output in (
            row["collection_output"],
            row["result_output"],
            row["witness_output"],
        ):
            _safe_relative_path(output, f"binding result for {lane_id}")
            if output not in lanes[lane_id]["expected_outputs"]:
                fail(f"binding result is not lane-required: {lane_id}: {output}")
        if expected_provenance is not None:
            _safe_relative_path(
                expected_provenance, f"binding provenance for {lane_id}"
            )
            if expected_provenance not in lanes[lane_id]["expected_outputs"]:
                fail(f"binding provenance is not lane-required: {lane_id}")
    if seen != BINDING_RESULT_LANES:
        fail("binding-result lane set differs from the frozen contract")

    expected_binding_builds = [
        {
            "lane_id": "bindings_selector",
            "build_root": "build/bindings-selector",
            "core_build_root": "build/bindings-selector/core",
            "deployment_build_root": "build/bindings-selector/deployment",
            "module_build_root": "build/bindings-selector/module",
            "stage_root": "bindings-selector/staged",
            "generated_extension": (
                "bindings-selector/generated/"
                + BINDING_EXTENSION_NAME
            ),
            "provenance_output": "bindings-selector/build-provenance.json",
            "sanitizer": "none",
        },
        {
            "lane_id": "bindings_release",
            "build_root": "build/bindings-release",
            "core_build_root": "build/bindings-release/core",
            "deployment_build_root": "build/bindings-release/deployment",
            "module_build_root": "build/bindings-release/module",
            "stage_root": "bindings-release/staged",
            "generated_extension": "generated/" + BINDING_EXTENSION_NAME,
            "provenance_output": "bindings-release/build-provenance.json",
            "sanitizer": "none",
        },
        {
            "lane_id": "bindings_asan_ubsan",
            "build_root": "build/bindings-asan",
            "core_build_root": "build/bindings-asan/core",
            "deployment_build_root": "build/bindings-asan/deployment",
            "module_build_root": "build/bindings-asan/module",
            "stage_root": "bindings-asan/staged",
            "generated_extension": None,
            "provenance_output": "bindings-asan/build-provenance.json",
            "sanitizer": "address+undefined",
        },
        {
            "lane_id": "bindings_tsan",
            "build_root": "build/bindings-tsan",
            "core_build_root": "build/bindings-tsan/core",
            "deployment_build_root": "build/bindings-tsan/deployment",
            "module_build_root": "build/bindings-tsan/module",
            "stage_root": "bindings-tsan/staged",
            "generated_extension": None,
            "provenance_output": "bindings-tsan/build-provenance.json",
            "sanitizer": "thread",
        },
    ]
    if contract["binding_build_lanes"] != expected_binding_builds:
        fail("binding-build lane contract drifted")
    for row in expected_binding_builds:
        for field in (
            "build_root",
            "core_build_root",
            "deployment_build_root",
            "module_build_root",
            "stage_root",
            "provenance_output",
        ):
            _safe_relative_path(
                row[field], f"binding build {row['lane_id']} {field}"
            )
        if row["generated_extension"] is not None:
            _safe_relative_path(
                row["generated_extension"],
                f"binding build {row['lane_id']} generated extension",
            )

    expected_production_lanes = [
        {
            "lane_id": "core_production_release",
            "project_id": "manyforge_core",
            "test_enabled_release_lane_id": "core_release",
            "source_root": "manyforge_core",
            "build_root": "build/core-production-release",
            "inventory_output": "core-production-release/ctest-inventory.json",
            "provenance_output": "core-production-release/build-provenance.json",
            "execution_index_output": "core-production-release/execution-index.json",
            "configuration": "Release",
            "build_testing": False,
            "sanitizer": "none",
            "configure_argv": [
                "/usr/bin/cmake",
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=Release",
                "-DBUILD_TESTING=OFF",
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                "--graphviz=/qualification-output/build/core-production-release/manyforge-i2-target-graph.dot",
                "-S",
                "/workspace/manyforge_core",
                "-B",
                "/qualification-output/build/core-production-release",
            ],
            "build_argv": [
                "/usr/bin/ninja",
                "-C",
                "/qualification-output/build/core-production-release",
                "-j",
                "2",
                "-d",
                "keeprsp",
                "manyforge_content_identity",
                "manyforge_core",
            ],
            "inventory_argv": [
                "/usr/bin/ctest",
                "--test-dir",
                "/qualification-output/build/core-production-release",
                "--show-only=json-v1",
            ],
        },
        {
            "lane_id": "deployment_production_release",
            "project_id": "manyforge_deployment",
            "test_enabled_release_lane_id": "deployment_release",
            "source_root": "manyforge_deployment",
            "build_root": "build/deployment-production-release",
            "inventory_output": "deployment-production-release/ctest-inventory.json",
            "provenance_output": "deployment-production-release/build-provenance.json",
            "execution_index_output": "deployment-production-release/execution-index.json",
            "configuration": "Release",
            "build_testing": False,
            "sanitizer": "none",
            "configure_argv": [
                "/usr/bin/cmake",
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=Release",
                "-DBUILD_TESTING=OFF",
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                "--graphviz=/qualification-output/build/deployment-production-release/manyforge-i2-target-graph.dot",
                "-DMANYFORGE_CORE_INCLUDE_DIR=/workspace/manyforge_core/include",
                "-S",
                "/workspace/manyforge_deployment",
                "-B",
                "/qualification-output/build/deployment-production-release",
            ],
            "build_argv": [
                "/usr/bin/ninja",
                "-C",
                "/qualification-output/build/deployment-production-release",
                "-j",
                "2",
                "-d",
                "keeprsp",
                "manyforge_build_qualified_admission_profile",
                "manyforge_content_identity",
                "manyforge_deployment",
                "manyforge_deployment_admission",
                "manyforge_deployment_canonical_package_generator",
                "manyforge_deployment_verification_core",
                "manyforge_emit_build_qualified_admission_profile",
                "manyforge_generate_canonical_empty_package",
            ],
            "inventory_argv": [
                "/usr/bin/ctest",
                "--test-dir",
                "/qualification-output/build/deployment-production-release",
                "--show-only=json-v1",
            ],
        },
    ]
    if contract["production_build_lanes"] != expected_production_lanes:
        fail("production build lane contract drifted")
    for row in expected_production_lanes:
        for field in ("build_root", "inventory_output", "provenance_output", "execution_index_output"):
            _safe_relative_path(row[field], f"production lane {row['lane_id']} {field}")
        expected_outputs = {
            row["inventory_output"],
            row["provenance_output"],
            row["execution_index_output"],
        }
        if set(lanes[row["lane_id"]]["expected_outputs"]) != expected_outputs:
            fail(f"production lane outputs drifted: {row['lane_id']}")
    proofs = _require_list(
        contract["cross_lane_exclusion_proofs"], "cross-lane exclusion proofs"
    )
    if len(proofs) != 1:
        fail("exactly one cross-lane exclusion proof is required")
    proof = _require_object(proofs[0], "cross-lane exclusion proof")
    _exact_keys(
        proof,
        {"test_name", "must_pass_lane", "excluded_lane", "reason"},
        "cross-lane exclusion proof",
    )
    if (
        proof["test_name"] != GOLDEN_RELEASE_TEST
        or proof["must_pass_lane"] != "deployment_release"
        or proof["excluded_lane"] != "deployment_asan_ubsan"
    ):
        fail("cross-lane Release-golden exclusion proof drifted")
    _require_text(proof["reason"], "cross-lane exclusion reason")


def _validate_scans(manifest: dict[str, Any], repository_root: Path) -> None:
    scans = _require_list(manifest["forbidden_scans"], "forbidden_scans")
    seen: set[str] = set()
    for index, raw in enumerate(scans):
        scan = _require_object(raw, f"forbidden_scans[{index}]")
        _exact_keys(
            scan,
            {
                "id",
                "mode",
                "roots",
                "pattern",
                "globs",
                "exclude_globs",
                "expected_matches",
            },
            f"forbidden_scans[{index}]",
        )
        scan_id = _require_text(scan["id"], f"forbidden_scans[{index}].id")
        if scan_id in seen or not IDENTIFIER.fullmatch(scan_id):
            fail(f"forbidden scan ID is invalid or duplicated: {scan_id}")
        seen.add(scan_id)
        if scan["mode"] not in {"line_rg", "file_regex"}:
            fail(f"scan {scan_id} has an unknown mode")
        roots = _require_list(scan["roots"], f"scan {scan_id} roots")
        if not roots:
            fail(f"scan {scan_id} has no roots")
        for root in roots:
            relative = _safe_relative_path(
                _require_text(root, f"scan {scan_id} root"), f"scan {scan_id} root"
            )
            if not (repository_root / relative).is_dir():
                fail(f"scan {scan_id} root is absent: {relative}")
        pattern = _require_text(scan["pattern"], f"scan {scan_id} pattern")
        if "\x00" in pattern or "\n" in pattern or "\r" in pattern:
            fail(f"scan {scan_id} pattern contains a forbidden control byte")
        for field in ("globs", "exclude_globs"):
            values = _require_list(scan[field], f"scan {scan_id} {field}")
            if any(not isinstance(value, str) or not value for value in values):
                fail(f"scan {scan_id} {field} must contain text globs")
        if scan["expected_matches"] != 0:
            fail(f"scan {scan_id} must fail closed on every match")
        if scan["mode"] == "file_regex":
            try:
                re.compile(pattern)
            except re.error as error:
                fail(f"scan {scan_id} file regex is invalid: {error}")
    if seen != REQUIRED_SCAN_IDS:
        fail("forbidden scan inventory differs from the frozen v1 contract")


def _validate_evidence(manifest: dict[str, Any]) -> None:
    evidence = _require_object(manifest["evidence_contract"], "evidence_contract")
    _exact_keys(
        evidence,
        {
            "output_root",
            "result_schema",
            "required_fields",
            "commit_raw_results",
            "require_independent_adversarial_review",
            "completion_claim",
        },
        "evidence_contract",
    )
    if evidence.get("output_root") != OUTPUT_ROOT.as_posix():
        fail("evidence output root is not the ignored bounded I2 directory")
    if evidence.get("result_schema") != RESULT_SCHEMA:
        fail("evidence result schema is not v1")
    if evidence.get("commit_raw_results") is not False:
        fail("raw qualification results must remain ignored")
    if evidence.get("require_independent_adversarial_review") is not True:
        fail("I2 cannot omit independent adversarial review")
    required_values = _require_list(evidence.get("required_fields"), "evidence fields")
    if len(required_values) != len(set(required_values)):
        fail("evidence contract contains duplicate required fields")
    if set(required_values) != REQUIRED_EVIDENCE_FIELDS:
        fail("evidence required-field set differs from the frozen contract")
    if evidence["completion_claim"] != (
        "Only one all-green cumulative result bound to one unchanged candidate tree "
        "may close C0.2m, C0.2n, C0.2o and I2 together."
    ):
        fail("evidence completion claim drifted")


def _validate_deferred_and_historical(
    manifest: dict[str, Any], repository_root: Path
) -> None:
    deferred = _require_list(manifest["deferred_requirements"], "deferred_requirements")
    rows: set[tuple[str, str]] = set()
    for index, raw in enumerate(deferred):
        row = _require_object(raw, f"deferred_requirements[{index}]")
        _exact_keys(row, {"requirement", "owner"}, f"deferred_requirements[{index}]")
        item = (
            _require_text(row["requirement"], f"deferred_requirements[{index}].requirement"),
            _require_text(row["owner"], f"deferred_requirements[{index}].owner"),
        )
        if item in rows:
            fail(f"deferred requirement is duplicated: {item[0]}")
        rows.add(item)
    if rows != REQUIRED_DEFERRED:
        fail("later-owner deferral inventory differs from the frozen set")

    historical = _require_list(
        manifest["historical_non_satisfying_inputs"], "historical inputs"
    )
    if len(historical) != 5:
        fail("historical C0.2m/n/o, E0 and image inputs must contain five rows")
    commits: dict[str, str] = {}
    image: str | None = None
    for index, raw in enumerate(historical):
        row = _require_object(raw, f"historical_non_satisfying_inputs[{index}]")
        if "commit" in row:
            _exact_keys(row, {"name", "commit"}, f"historical input {index}")
            name = _require_text(row["name"], f"historical input {index} name")
            commit = _require_text(row["commit"], f"historical input {index} commit")
            if not HEX40.fullmatch(commit) or name in commits:
                fail(f"historical commit row is malformed or duplicated: {name}")
            _git_tree(repository_root, commit)
            commits[name] = commit
        else:
            _exact_keys(row, {"name", "image"}, f"historical input {index}")
            if row["name"] != "historical implementation image" or image is not None:
                fail("historical image row is malformed or duplicated")
            image = _require_text(row["image"], "historical implementation image")
    if commits != REQUIRED_HISTORICAL_COMMITS or image != REQUIRED_HISTORICAL_IMAGE:
        fail("historical non-satisfying input inventory drifted")


def validate_manifest(manifest: dict[str, Any], repository_root: Path) -> dict[str, dict[str, Any]]:
    _exact_keys(manifest, TOP_LEVEL_KEYS, "manifest")
    if manifest["schema_version"] != SCHEMA:
        fail(f"manifest schema must be {SCHEMA}")
    if manifest["status"] != QUALIFICATION_STATUS:
        fail("manifest status is not the I2 qualification contract")
    _validate_base(manifest, repository_root)
    _validate_historical_packet_lineage(manifest, repository_root)
    _validate_candidate_identity(manifest["candidate_identity"], repository_root)
    _validate_environment(
        _require_object(manifest["qualification_environment"], "qualification_environment")
    )
    _validate_bindings(
        _require_object(manifest["binding_contract"], "binding_contract"),
        repository_root,
    )
    _validate_pinned_inputs(
        _require_object(manifest["pinned_inputs"], "pinned_inputs"),
        manifest,
        repository_root,
    )
    lanes = _validate_lanes(manifest)
    _validate_artifact_dependency_graph(manifest["artifact_dependency_graph"], lanes)
    _validate_test_inventory_contract(
        manifest["test_inventory_contract"], manifest, lanes, repository_root
    )
    _validate_fixtures(manifest, lanes, repository_root)
    _validate_rotating_identities(manifest["rotating_identities"])
    _validate_scans(manifest, repository_root)
    _validate_deferred_and_historical(manifest, repository_root)
    _validate_evidence(manifest)
    for section, expected_digest in FROZEN_SECTION_SHA256.items():
        if _canonical_json_sha256(manifest[section]) != expected_digest:
            fail(f"frozen manifest section drifted: {section}")
    return lanes


def render_lane(
    manifest: dict[str, Any],
    lane: dict[str, Any],
    repository_root: Path,
    lane_output_root: Path,
    artifact_root: Path,
) -> list[str]:
    image_id = manifest["qualification_environment"]["image"]["id"]
    replacements = {
        "{repository_root}": str(repository_root.resolve()),
        "{output_root}": str(lane_output_root.resolve()),
        "{image_id}": image_id,
        "{host_uid}": str(os.getuid()),
        "{host_gid}": str(os.getgid()),
    }
    argv: list[str] = []
    for raw in manifest["qualification_environment"]["docker_prefix"]:
        rendered = raw
        for token, value in replacements.items():
            rendered = rendered.replace(token, value)
        if "{" in rendered or "}" in rendered:
            fail(f"unresolved Docker renderer placeholder: {rendered}")
        argv.append(rendered)
    if not argv or argv[-1] != image_id:
        fail("Docker prefix must end with the exact immutable image ID")
    argv.pop()
    for producer in LANE_READ_ONLY_INPUTS.get(lane["id"], ()):
        producer_root = _lane_artifact_root(artifact_root, producer).resolve()
        argv.extend(
            [
                "--mount",
                (
                    f"type=bind,src={producer_root},"
                    f"dst=/qualification-input/{producer},readonly"
                ),
            ]
        )
    argv.append(image_id)
    argv.extend(lane["container_command"])
    return argv


def check_environment(manifest: dict[str, Any], repository_root: Path) -> dict[str, Any]:
    expected = manifest["qualification_environment"]
    image_id = expected["image"]["id"]
    inspected = json.loads(
        _run_checked(
            ["docker", "image", "inspect", image_id],
            cwd=repository_root,
            label="Docker image inspection",
        ).stdout
    )
    if not isinstance(inspected, list) or len(inspected) != 1:
        fail("Docker image inspection did not return exactly one image")
    image = inspected[0]
    if image.get("Id") != image_id or image.get("Os") != "linux" or image.get("Architecture") != "amd64":
        fail("resolved Docker image identity or platform drifted")
    probe_program = (
        "import hashlib,json,platform,struct,subprocess,sys,sysconfig; "
        "import coal,pinocchio,pybind11; "
        "h=hashlib.sha256(open('/usr/local/bin/check_urdf','rb').read()).hexdigest(); "
        "print(json.dumps({'machine':platform.machine(),'python':sys.version.split()[0],"
        "'cache_tag':sys.implementation.cache_tag,'soabi':sysconfig.get_config_var('SOABI'),"
        "'extension_suffix':sysconfig.get_config_var('EXT_SUFFIX'),"
        "'multiarch':sysconfig.get_config_var('MULTIARCH'),'address_bits':struct.calcsize('P')*8,"
        "'glibc':platform.libc_ver()[1],"
        "'cmake':subprocess.check_output(['cmake','--version'],text=True).split()[2],"
        "'ninja':subprocess.check_output(['ninja','--version'],text=True).strip(),"
        "'cxx_compiler':subprocess.check_output(['g++','--version'],text=True).splitlines()[0],"
        "'py_debug':bool(sysconfig.get_config_var('Py_DEBUG')),'gil_disabled':"
        "bool(sysconfig.get_config_var('Py_GIL_DISABLED')),'pybind11':pybind11.__version__,"
        "'pybind11_cmake_dir':pybind11.get_cmake_dir(),"
        "'pinocchio':pinocchio.__version__,'coal':coal.__version__,"
        "'check_urdf_sha256':h},sort_keys=True))"
    )
    probe = json.loads(
        _run_checked(
            [
                "docker",
                "run",
                "--rm",
                "--platform",
                "linux/amd64",
                "--network",
                "none",
                image_id,
                "python3",
                "-c",
                probe_program,
            ],
            cwd=repository_root,
            label="container ABI probe",
        ).stdout
    )
    python = expected["python"]
    expected_probe = {
        "machine": expected["machine"],
        "python": python["version"],
        "cache_tag": python["cache_tag"],
        "soabi": python["soabi"],
        "extension_suffix": python["extension_suffix"],
        "multiarch": python["multiarch"],
        "address_bits": expected["address_bits"],
        "glibc": expected["glibc"],
        "cmake": expected["cmake"],
        "ninja": expected["ninja"],
        "cxx_compiler": expected["cxx_compiler"],
        "py_debug": python["py_debug"],
        "gil_disabled": python["gil_disabled"],
        "pybind11": expected["pybind11"]["version"],
        "pybind11_cmake_dir": expected["pybind11"]["cmake_dir"],
        "pinocchio": manifest["pinned_inputs"]["consumer_stack"]["pinocchio"],
        "coal": manifest["pinned_inputs"]["consumer_stack"]["coal"],
        "check_urdf_sha256": manifest["pinned_inputs"]["consumer_stack"][
            "check_urdf_sha256"
        ],
    }
    if probe != expected_probe:
        fail(f"container ABI/dependency probe drifted: expected={expected_probe}, got={probe}")
    return {"image": {key: image.get(key) for key in ("Id", "Os", "Architecture")}, "probe": probe}


def _matches_any_glob(relative: str, name: str, globs: list[str]) -> bool:
    return any(fnmatch.fnmatch(relative, glob) or fnmatch.fnmatch(name, glob) for glob in globs)


def _scan_candidate_files(
    scan: dict[str, Any], repository_root: Path
) -> list[Path]:
    files: dict[str, Path] = {}
    for root_value in scan["roots"]:
        root = repository_root / root_value
        if not root.is_dir() or root.is_symlink():
            fail(f"scan {scan['id']} root is absent or unsafe: {root_value}")
        for candidate in root.rglob("*"):
            if candidate.is_symlink():
                fail(
                    f"scan {scan['id']} encountered a symlinked source path: "
                    f"{candidate.relative_to(repository_root)}"
                )
            if not candidate.is_file():
                continue
            relative = candidate.relative_to(repository_root).as_posix()
            if not _matches_any_glob(relative, candidate.name, scan["globs"]):
                continue
            if _matches_any_glob(relative, candidate.name, scan["exclude_globs"]):
                continue
            files[relative] = _require_contained_regular_file(
                candidate,
                repository_root,
                f"scan {scan['id']} source {relative}",
            )
    if not files:
        fail(f"scan {scan['id']} examines zero source files")
    return [files[relative] for relative in sorted(files)]


def _scan_inventory_evidence(paths: list[Path], repository_root: Path) -> dict[str, Any]:
    rows = [
        {
            "path": path.relative_to(repository_root).as_posix(),
            "sha256": _sha256_file(path),
            "size": path.stat().st_size,
        }
        for path in paths
    ]
    return {
        "examined_file_count": len(rows),
        "examined_files_sha256": _canonical_json_sha256(rows),
    }


def _evaluate_file_regex_scan(
    scan: dict[str, Any], repository_root: Path
) -> dict[str, Any]:
    expression = re.compile(scan["pattern"])
    matches: list[dict[str, Any]] = []
    paths = _scan_candidate_files(scan, repository_root)
    for path in paths:
        relative = path.relative_to(repository_root).as_posix()
        try:
            content = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as error:
            fail(f"scan {scan['id']} cannot read {relative}: {error}")
        for match in expression.finditer(content):
            matches.append(
                {
                    "path": relative,
                    "line": content.count("\n", 0, match.start()) + 1,
                    "excerpt": re.sub(r"\s+", " ", match.group(0))[:240],
                }
            )
    normalized = json.dumps(matches, sort_keys=True, separators=(",", ":"))
    return {
        "id": scan["id"],
        "mode": scan["mode"],
        **_scan_inventory_evidence(paths, repository_root),
        "matches": len(matches),
        "expected_matches": scan["expected_matches"],
        "passed": len(matches) == scan["expected_matches"],
        "matches_sha256": hashlib.sha256(normalized.encode()).hexdigest(),
        "details": matches,
    }


def _evaluate_line_rg_scan(
    scan: dict[str, Any], repository_root: Path
) -> dict[str, Any]:
    paths = _scan_candidate_files(scan, repository_root)
    relatives = [path.relative_to(repository_root).as_posix() for path in paths]
    argv = ["rg", "-n", "--no-heading", "--hidden", "--no-ignore"]
    argv.extend(["--", scan["pattern"], *relatives])
    completed = subprocess.run(
        argv,
        cwd=repository_root,
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode not in (0, 1):
        fail(f"forbidden scan {scan['id']} failed to execute: {completed.stderr.strip()}")
    lines = completed.stdout.splitlines()
    return {
        "id": scan["id"],
        "mode": scan["mode"],
        "argv": argv,
        **_scan_inventory_evidence(paths, repository_root),
        "matches": len(lines),
        "expected_matches": scan["expected_matches"],
        "passed": len(lines) == scan["expected_matches"],
        "matches_sha256": hashlib.sha256(completed.stdout.encode()).hexdigest(),
        "details": lines,
    }


def evaluate_scan(scan: dict[str, Any], repository_root: Path) -> dict[str, Any]:
    if scan["mode"] == "file_regex":
        return _evaluate_file_regex_scan(scan, repository_root)
    if scan["mode"] == "line_rg":
        return _evaluate_line_rg_scan(scan, repository_root)
    fail(f"forbidden scan {scan['id']} has an unknown mode")


def _forbidden_scan_output_root(requested_output_root: Path | None) -> Path:
    if requested_output_root is None:
        fail("--scan requires --output-root")
    try:
        metadata = requested_output_root.lstat()
        output_root = requested_output_root.resolve(strict=True)
    except OSError as error:
        fail(f"forbidden-surface scan output root is absent: {error}")
    if (
        output_root != Path("/qualification-output")
        or stat.S_ISLNK(metadata.st_mode)
        or not stat.S_ISDIR(metadata.st_mode)
    ):
        fail("forbidden-surface scan output root is not the exact safe artifact mount")
    return output_root


def run_scans(
    manifest: dict[str, Any],
    repository_root: Path,
    requested_output_root: Path | None,
) -> list[dict[str, Any]]:
    output_root = _forbidden_scan_output_root(requested_output_root)
    results = [evaluate_scan(scan, repository_root) for scan in manifest["forbidden_scans"]]
    _write_exclusive_json(output_root, FORBIDDEN_SCAN_OUTPUT, results)
    print(json.dumps(results, indent=2, sort_keys=True))
    if any(not result["passed"] for result in results):
        raise SystemExit(1)
    return results


def capture_source_identity(tool: Path, repository: Path) -> dict[str, Any]:
    result = _run_checked(
        [sys.executable, str(tool), "--repo-root", str(repository)],
        cwd=repository,
        label=f"source identity capture for {repository}",
    )
    try:
        decoded = json.loads(result.stdout, object_pairs_hook=_object_without_duplicates)
    except json.JSONDecodeError as error:
        fail(f"source identity tool returned malformed JSON for {repository}: {error}")
    return _validate_source_identity(
        decoded,
        repository=repository,
        label=f"captured source identity for {repository}",
    )


def _require_clean_git_identity(
    repository: Path,
    *,
    expected_commit: str,
    expected_tree: str,
    label: str,
) -> None:
    if _git_head(repository) != expected_commit:
        fail(f"{label} HEAD differs from the pinned commit")
    if _git_tree(repository, expected_commit) != expected_tree:
        fail(f"{label} tree differs from the pinned tree")
    status = _run_checked(
        [
            "git",
            "--no-optional-locks",
            "-C",
            str(repository),
            "status",
            "--porcelain=v1",
            "--untracked-files=all",
            "--ignore-submodules=none",
        ],
        cwd=repository,
        label=f"{label} status",
    ).stdout
    if status:
        fail(f"{label} is not clean")


def _validate_candidate_snapshot(
    snapshot_root: Path,
    run_root: Path,
    identity_tool_relative: Path,
    expected_manyforge: dict[str, Any],
    roboplan_contract: dict[str, Any],
) -> dict[str, Any]:
    try:
        relative = snapshot_root.relative_to(run_root)
    except ValueError:
        fail("candidate snapshot escaped its qualification run")
    if relative != Path("source"):
        fail("candidate snapshot must use the canonical source directory")
    current = run_root
    for part in relative.parts:
        current = current / part
        try:
            metadata = current.lstat()
        except OSError as error:
            fail(f"candidate snapshot is absent: {error}")
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
            fail(f"candidate snapshot contains an unsafe directory: {current}")
    identity_tool = _require_contained_regular_file(
        snapshot_root / identity_tool_relative,
        snapshot_root,
        "snapshot source-identity tool",
    )
    snapshot_identity = capture_source_identity(identity_tool, snapshot_root)
    if snapshot_identity != expected_manyforge:
        fail("materialized candidate snapshot differs from the captured candidate")
    roboplan_root = snapshot_root / roboplan_contract["checkout_path"]
    _require_clean_git_identity(
        roboplan_root,
        expected_commit=roboplan_contract["commit"],
        expected_tree=roboplan_contract["tree"],
        label="snapshot RoboPlan checkout",
    )
    docker_root = snapshot_root / ".docker"
    try:
        docker_entries = sorted(entry.name for entry in docker_root.iterdir())
    except OSError as error:
        fail(f"candidate snapshot .docker inventory is unreadable: {error}")
    if docker_entries != ["roboplan"]:
        fail("candidate snapshot contains an undeclared ignored .docker input")
    return {
        "path": relative.as_posix(),
        "materialization": "local_git_clone_without_hardlinks_at_exact_commit",
        "manyforge_source_identity": snapshot_identity,
        "roboplan": {
            "path": roboplan_contract["checkout_path"],
            "head_commit": roboplan_contract["commit"],
            "head_tree": roboplan_contract["tree"],
            "clean": True,
        },
        "ignored_input_roots": [roboplan_contract["checkout_path"]],
    }


def _materialize_candidate_snapshot(
    repository_root: Path,
    run_root: Path,
    identity_tool_relative: Path,
    expected_manyforge: dict[str, Any],
    roboplan_contract: dict[str, Any],
) -> tuple[Path, dict[str, Any]]:
    snapshot_root = run_root / "source"
    roboplan_source = repository_root / roboplan_contract["checkout_path"]
    _require_clean_git_identity(
        roboplan_source,
        expected_commit=roboplan_contract["commit"],
        expected_tree=roboplan_contract["tree"],
        label="live pinned RoboPlan checkout",
    )
    _run_checked(
        [
            "git",
            "clone",
            "--local",
            "--no-hardlinks",
            "--no-checkout",
            "--",
            str(repository_root),
            str(snapshot_root),
        ],
        cwd=run_root,
        label="candidate snapshot clone",
    )
    _run_checked(
        [
            "git",
            "-c",
            "core.hooksPath=/dev/null",
            "-C",
            str(snapshot_root),
            "checkout",
            "--detach",
            expected_manyforge["head_commit"],
        ],
        cwd=run_root,
        label="candidate snapshot checkout",
    )
    docker_root = snapshot_root / ".docker"
    try:
        docker_root.mkdir()
    except OSError as error:
        fail(f"cannot create candidate snapshot dependency root: {error}")
    snapshot_roboplan = docker_root / "roboplan"
    _run_checked(
        [
            "git",
            "clone",
            "--local",
            "--no-hardlinks",
            "--no-checkout",
            "--",
            str(roboplan_source),
            str(snapshot_roboplan),
        ],
        cwd=run_root,
        label="RoboPlan snapshot clone",
    )
    _run_checked(
        [
            "git",
            "-c",
            "core.hooksPath=/dev/null",
            "-C",
            str(snapshot_roboplan),
            "checkout",
            "--detach",
            roboplan_contract["commit"],
        ],
        cwd=run_root,
        label="RoboPlan snapshot checkout",
    )
    evidence = _validate_candidate_snapshot(
        snapshot_root,
        run_root,
        identity_tool_relative,
        expected_manyforge,
        roboplan_contract,
    )
    return snapshot_root, evidence


def _safe_output_root(repository_root: Path) -> Path:
    repository_root = repository_root.resolve(strict=True)
    unresolved = repository_root / OUTPUT_ROOT
    current = repository_root
    for part in OUTPUT_ROOT.parts:
        current = current / part
        if current.is_symlink():
            fail(f"I2 output root contains a symlinked path component: {current}")
    output_root = unresolved.resolve()
    expected_parent = (repository_root / ".manyforge-var/qualification").resolve()
    if output_root.parent != expected_parent or not output_root.is_relative_to(repository_root):
        fail("I2 output root escaped the ignored qualification directory")
    return output_root


def _exclusive_output_file(
    directory: Path, name: str, *, binary: bool
) -> Any:
    """Create one host-owned evidence file without following attacker links."""
    if Path(name).name != name or name in {".", ".."}:
        fail(f"host evidence filename must be one safe path component: {name}")
    directory_flags = os.O_RDONLY | os.O_DIRECTORY
    if hasattr(os, "O_NOFOLLOW"):
        directory_flags |= os.O_NOFOLLOW
    try:
        directory_fd = os.open(directory, directory_flags)
    except OSError as error:
        fail(f"cannot open host evidence directory {directory}: {error}")
    file_flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        file_flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(name, file_flags, 0o600, dir_fd=directory_fd)
    except OSError as error:
        fail(f"cannot exclusively create host evidence file {name}: {error}")
    finally:
        os.close(directory_fd)
    metadata = os.fstat(descriptor)
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
        os.close(descriptor)
        fail(f"host evidence target is not one regular file: {name}")
    if binary:
        return os.fdopen(descriptor, "wb")
    return os.fdopen(descriptor, "w", encoding="utf-8")


def _write_exclusive_json(directory: Path, name: str, value: Any) -> Path:
    path = directory / name
    with _exclusive_output_file(directory, name, binary=False) as destination:
        json.dump(value, destination, indent=2, sort_keys=True)
        destination.write("\n")
    return path


def _write_graphviz_options(build_root: Path) -> Path:
    path = build_root / "CMakeGraphVizOptions.cmake"
    with _exclusive_output_file(
        build_root, "CMakeGraphVizOptions.cmake", binary=True
    ) as destination:
        destination.write(GRAPHVIZ_OPTIONS)
    return path


def _lane_artifact_root(artifact_root: Path, lane_id: str) -> Path:
    if lane_id not in REQUIRED_LANES:
        fail(f"unknown lane artifact namespace: {lane_id}")
    return artifact_root / "lanes" / lane_id


def _artifact_tree_inventory(root: Path, label: str) -> list[dict[str, Any]]:
    """Capture one exact, non-following inventory for a stopped lane."""
    if not root.is_dir() or root.is_symlink():
        fail(f"{label} root is absent or unsafe")
    rows: list[dict[str, Any]] = []

    def visit(directory: Path) -> None:
        try:
            entries = sorted(os.scandir(directory), key=lambda entry: os.fsencode(entry.name))
        except OSError as error:
            fail(f"cannot enumerate {label}: {error}")
        for entry in entries:
            path = Path(entry.path)
            try:
                metadata = entry.stat(follow_symlinks=False)
            except OSError as error:
                fail(f"cannot stat {label} entry {path}: {error}")
            relative = path.relative_to(root).as_posix()
            common = {
                "path": relative,
                "mode": stat.S_IMODE(metadata.st_mode),
                "device": metadata.st_dev,
                "inode": metadata.st_ino,
                "nlink": metadata.st_nlink,
                "mtime_ns": metadata.st_mtime_ns,
            }
            if stat.S_ISDIR(metadata.st_mode):
                rows.append({**common, "type": "directory"})
                visit(path)
            elif stat.S_ISREG(metadata.st_mode):
                rows.append(
                    {
                        **common,
                        "type": "regular",
                        "size": metadata.st_size,
                        "sha256": _sha256_file(path),
                    }
                )
            elif stat.S_ISLNK(metadata.st_mode):
                try:
                    target = os.readlink(path)
                except OSError as error:
                    fail(f"cannot read {label} symlink {path}: {error}")
                rows.append({**common, "type": "symlink", "target": target})
            else:
                fail(f"{label} contains a socket/device/FIFO: {relative}")
    visit(root)
    if not rows:
        fail(f"{label} contains no artifacts")
    return rows


def _validate_bounded_tree_inventory(
    value: Any,
    *,
    lane_root: Path,
    label: str,
) -> dict[str, Any]:
    """Re-enumerate one in-container relative tree without following aliases."""

    inventory = _require_object(value, label)
    _exact_keys(
        inventory,
        {
            "root",
            "entry_count",
            "total_file_bytes",
            "entries_sha256",
            "entries",
        },
        label,
    )
    logical_root = _require_text(inventory["root"], f"{label}.root")
    if (
        posixpath.normpath(logical_root) != logical_root
        or not logical_root.startswith("/qualification-output/")
    ):
        fail(f"{label}.root is not a canonical qualification-output path")
    relative_root = Path(logical_root).relative_to("/qualification-output")
    physical_root = lane_root / relative_root
    current = lane_root
    for part in relative_root.parts:
        current = current / part
        if current.is_symlink():
            fail(f"{label}.root contains a symlinked path component")
    if not physical_root.is_dir():
        fail(f"{label}.root is not a retained directory")

    expected_rows = _require_list(inventory["entries"], f"{label}.entries")
    normalized_expected: list[dict[str, Any]] = []
    for index, raw in enumerate(expected_rows):
        row_label = f"{label}.entries[{index}]"
        row = _require_object(raw, row_label)
        kind = _require_text(row.get("kind"), f"{row_label}.kind")
        expected_keys = (
            {"relative_path", "kind"}
            if kind == "directory"
            else {"relative_path", "kind", "size", "mode", "sha256"}
        )
        _exact_keys(row, expected_keys, row_label)
        relative = _safe_relative_path(
            _require_text(row["relative_path"], f"{row_label}.relative_path"),
            f"{row_label}.relative_path",
        ).as_posix()
        if kind == "directory":
            normalized_expected.append(
                {"relative_path": relative, "kind": "directory"}
            )
            continue
        if kind != "file":
            fail(f"{row_label}.kind is unsupported")
        size = row["size"]
        mode = row["mode"]
        digest = row["sha256"]
        if (
            not isinstance(size, int)
            or isinstance(size, bool)
            or size < 0
            or not isinstance(mode, str)
            or re.fullmatch(r"[0-7]{4}", mode) is None
            or not isinstance(digest, str)
            or HEX64.fullmatch(digest) is None
        ):
            fail(f"{row_label} file metadata is malformed")
        normalized_expected.append(
            {
                "relative_path": relative,
                "kind": "file",
                "size": size,
                "mode": mode,
                "sha256": digest,
            }
        )
    if normalized_expected != sorted(
        normalized_expected,
        key=lambda row: (row["relative_path"], row["kind"]),
    ) or len({row["relative_path"] for row in normalized_expected}) != len(
        normalized_expected
    ):
        fail(f"{label}.entries are not exact, sorted, and unique")

    actual_rows: list[dict[str, Any]] = []
    pending = [physical_root]
    total_bytes = 0
    while pending:
        directory = pending.pop()
        try:
            with os.scandir(directory) as iterator:
                children = sorted(iterator, key=lambda entry: entry.name)
        except OSError as error:
            fail(f"cannot enumerate {label}: {error}")
        for child in children:
            path = Path(child.path)
            try:
                metadata = child.stat(follow_symlinks=False)
            except OSError as error:
                fail(f"cannot stat {label} entry {path}: {error}")
            relative = path.relative_to(physical_root).as_posix()
            if len(actual_rows) >= 20_000 or len(PurePosixPath(relative).parts) > 32:
                fail(f"{label} exceeds its entry/depth bound")
            if stat.S_ISLNK(metadata.st_mode):
                fail(f"{label} contains a symlink: {relative}")
            if stat.S_ISDIR(metadata.st_mode):
                actual_rows.append(
                    {"relative_path": relative, "kind": "directory"}
                )
                pending.append(path)
                continue
            if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
                fail(f"{label} contains a special or hardlinked entry: {relative}")
            total_bytes += metadata.st_size
            if total_bytes > 256 * 1024 * 1024:
                fail(f"{label} exceeds its total-byte bound")
            actual_rows.append(
                {
                    "relative_path": relative,
                    "kind": "file",
                    "size": metadata.st_size,
                    "mode": f"{stat.S_IMODE(metadata.st_mode):04o}",
                    "sha256": _sha256_file(path),
                }
            )
    actual_rows.sort(key=lambda row: (row["relative_path"], row["kind"]))
    if not actual_rows or actual_rows != normalized_expected:
        fail(f"{label} retained tree differs from its inventory")
    entries_digest = _canonical_json_sha256(actual_rows)
    if (
        inventory["entry_count"] != len(actual_rows)
        or inventory["total_file_bytes"] != total_bytes
        or inventory["entries_sha256"] != entries_digest
    ):
        fail(f"{label} counts or digest drifted")
    return {
        "root": logical_root,
        "entry_count": len(actual_rows),
        "total_file_bytes": total_bytes,
        "entries_sha256": entries_digest,
        "entries": actual_rows,
    }


def _validate_single_file_inventory(
    value: Any,
    *,
    lane_root: Path,
    label: str,
) -> dict[str, Any]:
    """Rehash one owned file without claiming exclusivity over its parent."""

    inventory = _require_object(value, label)
    _exact_keys(
        inventory,
        {
            "policy",
            "root",
            "entry_count",
            "total_file_bytes",
            "entries_sha256",
            "entries",
        },
        label,
    )
    if inventory["policy"] != "single_file":
        fail(f"{label}.policy drifted")
    logical_root = _require_text(inventory["root"], f"{label}.root")
    if (
        posixpath.normpath(logical_root) != logical_root
        or not logical_root.startswith("/qualification-output/")
    ):
        fail(f"{label}.root is not canonical")
    entries = _require_list(inventory["entries"], f"{label}.entries")
    if len(entries) != 1:
        fail(f"{label} does not contain exactly one file entry")
    row = _require_object(entries[0], f"{label}.entries[0]")
    _exact_keys(
        row,
        {"relative_path", "kind", "size", "mode", "sha256"},
        f"{label}.entries[0]",
    )
    relative = _safe_relative_path(
        _require_text(row["relative_path"], f"{label}.relative_path"),
        f"{label}.relative_path",
    )
    if row["kind"] != "file":
        fail(f"{label} entry is not a file")
    physical_root = lane_root / Path(logical_root).relative_to(
        "/qualification-output"
    )
    physical_file = _require_contained_regular_file(
        physical_root / relative,
        lane_root,
        f"{label} file",
        require_single_link=True,
    )
    size = physical_file.stat().st_size
    mode = f"{stat.S_IMODE(physical_file.stat().st_mode):04o}"
    digest = _sha256_file(physical_file)
    normalized_row = {
        "relative_path": relative.as_posix(),
        "kind": "file",
        "size": size,
        "mode": mode,
        "sha256": digest,
    }
    if (
        row != normalized_row
        or inventory["entry_count"] != 1
        or inventory["total_file_bytes"] != size
        or inventory["entries_sha256"]
        != _canonical_json_sha256([normalized_row])
    ):
        fail(f"{label} file metadata/digest drifted")
    return {
        "policy": "single_file",
        "root": logical_root,
        "entry_count": 1,
        "total_file_bytes": size,
        "entries_sha256": inventory["entries_sha256"],
        "entries": [normalized_row],
    }


def _seal_lane_artifacts(
    artifact_root: Path, seal_root: Path, lane_id: str
) -> dict[str, Any]:
    lane_root = _lane_artifact_root(artifact_root, lane_id)
    inventory = _artifact_tree_inventory(lane_root, f"{lane_id} lane artifacts")
    inventory_sha256 = _canonical_json_sha256(inventory)
    seal = {
        "schema_version": "manyforge.composable_cell_i2.lane_artifact_seal.v1",
        "lane_id": lane_id,
        "inventory_count": len(inventory),
        "inventory_sha256": inventory_sha256,
        "inventory": inventory,
    }
    seal_path = _write_exclusive_json(seal_root, f"{lane_id}.json", seal)
    return {
        "lane_id": lane_id,
        "inventory_count": len(inventory),
        "inventory_sha256": inventory_sha256,
        "seal": _file_evidence(seal_path, seal_root.parent),
    }


def _validate_lane_artifact_seals(
    artifact_root: Path,
    seal_root: Path,
    lane_ids: set[str],
) -> list[dict[str, Any]]:
    summaries: list[dict[str, Any]] = []
    for lane_id in sorted(lane_ids):
        seal_path = _require_output_file(
            seal_root, f"{lane_id}.json", f"{lane_id} host artifact seal"
        )
        seal = _read_json_object(seal_path, f"{lane_id} host artifact seal")
        _exact_keys(
            seal,
            {
                "schema_version",
                "lane_id",
                "inventory_count",
                "inventory_sha256",
                "inventory",
            },
            f"{lane_id} host artifact seal",
        )
        current = _artifact_tree_inventory(
            _lane_artifact_root(artifact_root, lane_id),
            f"{lane_id} lane artifacts",
        )
        if (
            seal["schema_version"]
            != "manyforge.composable_cell_i2.lane_artifact_seal.v1"
            or seal["lane_id"] != lane_id
            or seal["inventory_count"] != len(current)
            or seal["inventory_sha256"] != _canonical_json_sha256(current)
            or seal["inventory"] != current
        ):
            fail(f"{lane_id} artifacts changed after their host seal")
        summaries.append(
            {
                "lane_id": lane_id,
                "inventory_count": len(current),
                "inventory_sha256": seal["inventory_sha256"],
                "seal": _file_evidence(seal_path, seal_root.parent),
            }
        )
    return summaries


def _file_evidence(path: Path, root: Path) -> dict[str, Any]:
    return {
        "path": path.relative_to(root).as_posix(),
        "sha256": _sha256_file(path),
        "size": path.stat().st_size,
    }


def _require_output_file(run_root: Path, relative: str, label: str) -> Path:
    safe = _safe_relative_path(relative, label)
    path = _require_contained_regular_file(
        run_root / safe,
        run_root,
        label,
        require_single_link=True,
    )
    if path.stat().st_size == 0:
        fail(f"{label} must not be empty: {relative}")
    return path


def _read_json_object(path: Path, label: str) -> dict[str, Any]:
    try:
        decoded = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_object_without_duplicates,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"{label} is unreadable JSON: {error}")
    return _require_object(decoded, label)


def _validate_source_identity_files(
    artifact_root: Path,
    successful_lanes: set[str],
    expected_manyforge: dict[str, Any],
    expected_specs: dict[str, Any],
) -> list[dict[str, Any]]:
    run_root = artifact_root.parent
    required: list[tuple[str, Path, dict[str, Any]]] = [
        (
            "host_manyforge_before",
            _require_output_file(
                run_root,
                "host-manyforge-source-before.json",
                "host ManyForge source identity before lanes",
            ),
            expected_manyforge,
        ),
        (
            "host_manyforge_after",
            _require_output_file(
                run_root,
                "host-manyforge-source-after.json",
                "host ManyForge source identity after lanes",
            ),
            expected_manyforge,
        ),
        (
            "host_specs_before",
            _require_output_file(
                run_root,
                "host-specs-source-before.json",
                "host Specs source identity before lanes",
            ),
            expected_specs,
        ),
        (
            "host_specs_after",
            _require_output_file(
                run_root,
                "host-specs-source-after.json",
                "host Specs source identity after lanes",
            ),
            expected_specs,
        ),
    ]
    if "preflight" in successful_lanes:
        required.append(
            (
                "container_manyforge_before",
                _require_output_file(
                    _lane_artifact_root(artifact_root, "preflight"),
                    "candidate-source.json",
                    "container ManyForge source identity before lanes",
                ),
                expected_manyforge,
            )
        )
    if "final_recheck" in successful_lanes:
        required.append(
            (
                "container_manyforge_after",
                _require_output_file(
                    _lane_artifact_root(artifact_root, "final_recheck"),
                    "final-candidate-source.json",
                    "container ManyForge source identity after lanes",
                ),
                expected_manyforge,
            )
        )
    rows: list[dict[str, Any]] = []
    for role, path, expected in required:
        decoded = _read_json_object(path, f"{role} source identity")
        identity = _validate_source_identity(
            decoded,
            repository=None,
            label=f"{role} source identity",
        )
        if identity != expected:
            fail(f"{role} source identity is not the exact candidate identity")
        rows.append(
            {
                "role": role,
                **_file_evidence(path, run_root),
                "identity": identity,
            }
        )
    return rows


def _name_digest(names: set[str]) -> str:
    payload = "".join(f"{name}\n" for name in sorted(names, key=lambda value: value.encode()))
    return hashlib.sha256(payload.encode()).hexdigest()


def _ctest_inventory_names(path: Path, label: str) -> set[str]:
    decoded = _read_json_object(path, label)
    tests = _require_list(decoded.get("tests"), f"{label}.tests")
    if not tests:
        fail(f"{label} contains zero tests")
    names: list[str] = []
    for index, raw in enumerate(tests):
        test = _require_object(raw, f"{label}.tests[{index}]")
        name = _require_text(test.get("name"), f"{label}.tests[{index}].name")
        names.append(name)
        properties = test.get("properties", [])
        if not isinstance(properties, list):
            fail(f"{label}.tests[{index}].properties must be an array")
        for property_raw in properties:
            if not isinstance(property_raw, dict):
                fail(f"{label}.tests[{index}] contains a malformed property")
            disabled_value = property_raw.get("value")
            if property_raw.get("name") == "DISABLED" and (
                disabled_value is True
                or (
                    isinstance(disabled_value, (int, str))
                    and str(disabled_value).upper() in {"1", "ON", "TRUE"}
                )
            ):
                fail(f"{label} contains a disabled test: {name}")
    if len(names) != len(set(names)):
        fail(f"{label} contains duplicate test names")
    return set(names)


def _validate_utility_target_relationships(
    value: Any,
    target_value: Any,
    label: str,
    *,
    lane_root: Path,
) -> list[dict[str, Any]]:
    relationships = _require_list(value, f"{label} relationships")
    targets = _require_list(target_value, f"{label} targets")
    target_index: dict[tuple[str, str], dict[str, Any]] = {}
    for index, raw_target in enumerate(targets):
        target = _require_object(raw_target, f"{label} target[{index}]")
        project_id = _require_text(
            target.get("project_id"), f"{label} target[{index}].project_id"
        )
        target_name = _require_text(
            target.get("target"), f"{label} target[{index}].target"
        )
        key = (project_id, target_name)
        if key in target_index:
            fail(f"{label} target proof repeats {project_id}:{target_name}")
        target_index[key] = target

    relationship_index: dict[tuple[str, str], dict[str, Any]] = {}
    normalized: list[dict[str, Any]] = []
    expected_keys = {
        key
        for key, target in target_index.items()
        if target.get("target_type") == "UTILITY"
    }
    for index, raw_relationship in enumerate(relationships):
        row_label = f"{label} relationship[{index}]"
        row = _require_object(raw_relationship, row_label)
        _exact_keys(
            row,
            {
                "project_id",
                "target",
                "primary_output_relative_path",
                "generated_output_sha256",
                "output_inventory_sha256",
                "producer_target",
                "producer_artifact_relative_path",
                "producer_artifact_sha256",
                "consumer_relationships",
            },
            row_label,
        )
        project_id = _require_text(row["project_id"], f"{row_label}.project_id")
        target_name = _require_text(row["target"], f"{row_label}.target")
        key = (project_id, target_name)
        if key in relationship_index:
            fail(f"{label} repeats utility relationship {project_id}:{target_name}")
        generated_relative = _safe_relative_path(
            _require_text(
                row["primary_output_relative_path"],
                f"{row_label}.primary_output_relative_path",
            ),
            f"{row_label}.primary_output_relative_path",
        ).as_posix()
        producer_relative = _safe_relative_path(
            _require_text(
                row["producer_artifact_relative_path"],
                f"{row_label}.producer_artifact_relative_path",
            ),
            f"{row_label}.producer_artifact_relative_path",
        ).as_posix()
        generated_digest = _require_text(
            row["generated_output_sha256"], f"{row_label}.generated_output_sha256"
        )
        inventory_digest = _require_text(
            row["output_inventory_sha256"],
            f"{row_label}.output_inventory_sha256",
        )
        producer_digest = _require_text(
            row["producer_artifact_sha256"],
            f"{row_label}.producer_artifact_sha256",
        )
        if not all(
            HEX64.fullmatch(digest)
            for digest in (generated_digest, inventory_digest, producer_digest)
        ):
            fail(f"{row_label} contains an invalid SHA-256 digest")
        producer_target = _require_text(
            row["producer_target"], f"{row_label}.producer_target"
        )
        consumer_values = _require_list(
            row["consumer_relationships"], f"{row_label}.consumer_relationships"
        )
        consumer_relationships: list[tuple[str, str]] = []
        for consumer_index, raw_consumer in enumerate(consumer_values):
            consumer_label = (
                f"{row_label}.consumer_relationships[{consumer_index}]"
            )
            consumer = _require_object(raw_consumer, consumer_label)
            _exact_keys(consumer, {"target", "kind"}, consumer_label)
            consumer_target = _require_text(
                consumer["target"], f"{consumer_label}.target"
            )
            consumer_kind = _require_text(
                consumer["kind"], f"{consumer_label}.kind"
            )
            if consumer_kind not in {
                "compiled_generated_input",
                "target_dependency",
            }:
                fail(f"{consumer_label}.kind is unsupported")
            consumer_relationships.append((consumer_target, consumer_kind))
        if (
            not consumer_relationships
            or len(consumer_relationships) != len(set(consumer_relationships))
            or any(
                (project_id, consumer) not in target_index
                for consumer, _kind in consumer_relationships
            )
        ):
            fail(
                f"{row_label}.consumer_relationships are empty, duplicated, "
                "or unproved"
            )

        utility = target_index.get(key)
        producer = target_index.get((project_id, producer_target))
        if utility is None or producer is None:
            fail(f"{row_label} omits its utility or producer target proof")
        utility_artifact = _require_object(
            utility.get("artifact"), f"{row_label} utility artifact"
        )
        producer_artifact = _require_object(
            producer.get("artifact"), f"{row_label} producer artifact"
        )
        generated_path = _require_text(
            utility_artifact.get("path"), f"{row_label} generated path"
        )
        producer_path = _require_text(
            producer_artifact.get("path"), f"{row_label} producer path"
        )
        producer_resolved_path = _require_text(
            producer_artifact.get("resolved_path"),
            f"{row_label} resolved producer path",
        )
        if (
            posixpath.normpath(producer_resolved_path) != producer_resolved_path
            or not producer_resolved_path.startswith("/qualification-output/")
        ):
            fail(f"{row_label} resolved producer path escaped the lane")
        physical_producer = _require_contained_regular_file(
            lane_root
            / Path(producer_resolved_path).relative_to("/qualification-output"),
            lane_root,
            f"{row_label} retained producer artifact",
            require_single_link=True,
        )
        if (
            _sha256_file(physical_producer) != producer_digest
            or not os.access(physical_producer, os.X_OK)
        ):
            fail(f"{row_label} retained producer artifact is not executable/exact")
        generator = _require_object(
            utility.get("generator"), f"{row_label} generator"
        )
        raw_output_inventory = _require_object(
            utility.get("output_inventory"), f"{row_label} output inventory"
        )
        output_policy = raw_output_inventory.get("policy")
        if output_policy == "single_file":
            output_inventory = _validate_single_file_inventory(
                raw_output_inventory,
                lane_root=lane_root,
                label=f"{row_label} output inventory",
            )
        elif output_policy == "exact_bounded_tree":
            tree_inventory = dict(raw_output_inventory)
            del tree_inventory["policy"]
            output_inventory = _validate_bounded_tree_inventory(
                tree_inventory,
                lane_root=lane_root,
                label=f"{row_label} output inventory",
            )
            output_inventory["policy"] = "exact_bounded_tree"
        else:
            fail(f"{row_label} output inventory policy drifted")
        if (
            utility.get("target_type") != "UTILITY"
            or utility.get("contract_artifact") != generated_relative
            or utility_artifact.get("sha256") != generated_digest
            or producer.get("target_type") != "EXECUTABLE"
            or producer.get("contract_artifact") != producer_relative
            or producer_artifact.get("sha256") != producer_digest
            or generator.get("project_id") != project_id
            or generator.get("target") != producer_target
            or generator.get("path") != producer_path
            or generator.get("sha256") != producer_digest
        ):
            fail(f"{row_label} utility/producer byte-owner join drifted")

        try:
            generated_in_inventory = PurePosixPath(generated_path).relative_to(
                PurePosixPath(output_inventory["root"])
            ).as_posix()
        except ValueError:
            fail(f"{row_label} primary output escapes its output inventory")
        inventory_matches = [
            entry
            for entry in output_inventory["entries"]
            if entry["relative_path"] == generated_in_inventory
            and entry["kind"] == "file"
        ]
        if (
            len(inventory_matches) != 1
            or inventory_matches[0]["sha256"] != generated_digest
            or output_inventory["entries_sha256"] != inventory_digest
        ):
            fail(f"{row_label} primary output/inventory byte join drifted")

        output_edge = _require_object(
            utility.get("custom_command_output_edge"),
            f"{row_label} custom-command output edge",
        )
        _exact_keys(
            output_edge,
            {
                "explicit_outputs",
                "implicit_outputs",
                "rule",
                "edge_sha256",
                "build_ninja",
            },
            f"{row_label} custom-command output edge",
        )
        build_root = _require_text(
            utility.get("build_root"), f"{row_label} utility build root"
        )
        try:
            primary_edge_relative = PurePosixPath(generated_path).relative_to(
                PurePosixPath(build_root)
            ).as_posix()
        except ValueError:
            fail(f"{row_label} primary output escapes its utility build")
        declared_byproducts = _require_list(
            utility.get("declared_byproducts"),
            f"{row_label} declared byproducts",
        )
        expected_edge_outputs = {primary_edge_relative}
        for byproduct_index, raw_byproduct in enumerate(declared_byproducts):
            byproduct_label = (
                f"{row_label} declared byproduct[{byproduct_index}]"
            )
            byproduct = _require_object(raw_byproduct, byproduct_label)
            _exact_keys(byproduct, {"path", "sha256"}, byproduct_label)
            byproduct_path = _require_text(
                byproduct["path"], f"{byproduct_label}.path"
            )
            byproduct_digest = _require_text(
                byproduct["sha256"], f"{byproduct_label}.sha256"
            )
            try:
                byproduct_relative = PurePosixPath(byproduct_path).relative_to(
                    PurePosixPath(build_root)
                ).as_posix()
            except ValueError:
                fail(f"{byproduct_label} escapes its utility build")
            if HEX64.fullmatch(byproduct_digest) is None:
                fail(f"{byproduct_label} digest is malformed")
            expected_edge_outputs.add(byproduct_relative)
        expected_implicit_outputs = {
            f"${{cmake_ninja_workdir}}{path}" for path in expected_edge_outputs
        }
        build_ninja = _require_object(
            output_edge["build_ninja"], f"{row_label} build.ninja owner"
        )
        _exact_keys(
            build_ninja,
            {"path", "sha256"},
            f"{row_label} build.ninja owner",
        )
        if (
            output_edge["explicit_outputs"] != sorted(expected_edge_outputs)
            or output_edge["implicit_outputs"]
            != sorted(expected_implicit_outputs)
            or output_edge["rule"] != "CUSTOM_COMMAND"
            or not isinstance(output_edge["edge_sha256"], str)
            or HEX64.fullmatch(output_edge["edge_sha256"]) is None
            or not isinstance(build_ninja["sha256"], str)
            or HEX64.fullmatch(build_ninja["sha256"]) is None
        ):
            fail(f"{row_label} custom-command output edge drifted")

        compiled_relationships: dict[tuple[str, str], dict[str, Any]] = {}
        for (proof_project, proof_target), proof in target_index.items():
            if proof_project != project_id:
                continue
            matches = [
                generated
                for raw_generated in _require_list(
                    proof.get("required_generated_inputs", []),
                    f"{row_label} {proof_target} required generated inputs",
                )
                if (
                    generated := _require_object(
                        raw_generated,
                        f"{row_label} {proof_target} generated input",
                    )
                ).get("path")
                == generated_path
            ]
            if len(matches) > 1:
                fail(f"{row_label} consumer {proof_target} repeats generated input")
            if not matches:
                continue
            generated = matches[0]
            if (
                generated.get("sha256") != generated_digest
                or generated.get("producer_target") != producer_target
                or generated.get("producer_artifact") != producer_path
                or generated.get("producer_sha256") != producer_digest
            ):
                fail(f"{row_label} consumer {proof_target} owner/byte join drifted")
            compiled_relationships[(proof_target, "compiled_generated_input")] = generated

        utility_relationships: set[tuple[str, str]] = set()
        for relationship_index, raw_utility_relationship in enumerate(
            _require_list(
                utility.get("consumer_relationships"),
                f"{row_label} utility consumer relationships",
            )
        ):
            utility_label = (
                f"{row_label} utility consumer relationship[{relationship_index}]"
            )
            utility_relationship = _require_object(
                raw_utility_relationship, utility_label
            )
            utility_kind = _require_text(
                utility_relationship.get("kind"), f"{utility_label}.kind"
            )
            expected_utility_keys = (
                {"target", "kind", "graph_style"}
                if utility_kind == "target_dependency"
                else {"target", "kind"}
            )
            _exact_keys(utility_relationship, expected_utility_keys, utility_label)
            utility_target = _require_text(
                utility_relationship["target"], f"{utility_label}.target"
            )
            if utility_kind not in {
                "compiled_generated_input",
                "target_dependency",
            }:
                fail(f"{utility_label}.kind is unsupported")
            if (
                (project_id, utility_target) not in target_index
                or (
                    utility_kind == "target_dependency"
                    and utility_relationship["graph_style"] != "solid"
                )
            ):
                fail(f"{utility_label} is not joined to one proved consumer")
            relationship_key = (utility_target, utility_kind)
            if relationship_key in utility_relationships:
                fail(f"{row_label} utility repeats a consumer relationship")
            utility_relationships.add(relationship_key)

        expected_relationships = set(consumer_relationships)
        if utility_relationships != expected_relationships:
            fail(
                f"{row_label} utility relationship set drifted: "
                f"expected={sorted(expected_relationships)}, "
                f"actual={sorted(utility_relationships)}"
            )
        expected_compiled = {
            relationship
            for relationship in expected_relationships
            if relationship[1] == "compiled_generated_input"
        }
        if set(compiled_relationships) != expected_compiled:
            fail(
                f"{row_label} compiled consumer set drifted: "
                f"expected={sorted(expected_compiled)}, "
                f"actual={sorted(compiled_relationships)}"
            )
        relationship_index[key] = row
        normalized.append(dict(row))
    if set(relationship_index) != expected_keys:
        fail(
            f"{label} utility relationship set drifted: "
            f"expected={sorted(expected_keys)}, actual={sorted(relationship_index)}"
        )
    return sorted(
        normalized, key=lambda row: (row["project_id"], row["target"])
    )


def _validate_git_tracked_file_evidence(
    value: Any,
    *,
    physical_file: Path,
    expected_git_root: str,
    expected_repository_path: str,
    label: str,
) -> None:
    identity = _require_object(value, label)
    _exact_keys(identity, {"git_root", "path", "mode", "blob"}, label)
    mode = _require_text(identity["mode"], f"{label}.mode")
    blob = _require_text(identity["blob"], f"{label}.blob")
    if mode not in {"100644", "100755"} or HEX40.fullmatch(blob) is None:
        fail(f"{label} mode/blob is malformed")
    try:
        raw = physical_file.read_bytes()
    except OSError as error:
        fail(f"{label} source bytes are unreadable: {error}")
    actual_blob = hashlib.sha1(
        f"blob {len(raw)}\0".encode("ascii") + raw
    ).hexdigest()
    executable = bool(physical_file.stat().st_mode & stat.S_IXUSR)
    if (
        identity["git_root"] != expected_git_root
        or identity["path"] != expected_repository_path
        or actual_blob != blob
        or executable != (mode == "100755")
    ):
        fail(f"{label} differs from its exact tracked-file identity")


def _validate_auxiliary_install_projects(
    value: Any,
    *,
    qualified_inputs_value: Any,
    installed_copies_value: Any,
    target_proofs_value: Any,
    lane_root: Path,
    build_contract: dict[str, Any],
    expected_install_roots: dict[str, str],
    label: str,
) -> None:
    projects = _require_list(value, f"{label} projects")
    qualified_inputs = _require_list(
        qualified_inputs_value, f"{label} qualified install inputs"
    )
    installed_copies = _require_list(
        installed_copies_value, f"{label} installed copies"
    )
    target_proofs = _require_list(target_proofs_value, f"{label} target proofs")
    contract_projects = _require_object(
        _require_object(
            build_contract["auxiliary_install_contract"],
            f"{label} auxiliary install contract",
        )["projects"],
        f"{label} auxiliary install contract projects",
    )
    if not expected_install_roots:
        if projects or qualified_inputs or installed_copies:
            fail(f"{label} has install owners without an install project")
        return
    if set(expected_install_roots) != set(contract_projects):
        fail(f"{label} expected install-root project set drifted")
    expected_libraries: dict[tuple[str, str], dict[str, Any]] = {}
    for project_id in sorted(contract_projects):
        project = _require_object(
            contract_projects[project_id], f"{label} contract {project_id}"
        )
        for raw_library in _require_list(
            project.get("library_files"), f"{label} contract {project_id} libraries"
        ):
            library = _require_object(raw_library, f"{label} contract library")
            owner = (
                project_id,
                _require_text(library.get("target"), f"{label} library target"),
            )
            if owner in expected_libraries:
                fail(f"{label} repeats contracted auxiliary library {owner}")
            expected_libraries[owner] = library
    target_index: dict[tuple[str, str], dict[str, Any]] = {}
    for index, raw_target in enumerate(target_proofs):
        target_label = f"{label} target proof[{index}]"
        target = _require_object(raw_target, target_label)
        project_id = target.get("project_id")
        if project_id not in contract_projects:
            continue
        owner = (
            _require_text(project_id, f"{target_label}.project_id"),
            _require_text(target.get("target"), f"{target_label}.target"),
        )
        if owner not in expected_libraries or owner in target_index:
            fail(f"{target_label} auxiliary owner is unknown or duplicated")
        library = expected_libraries[owner]
        artifact = _require_object(target.get("artifact"), f"{target_label}.artifact")
        _exact_keys(
            artifact,
            {"path", "resolved_path", "sha256"},
            f"{target_label}.artifact",
        )
        sanitizer_root = PurePosixPath(expected_install_roots[owner[0]]).parents[1]
        expected_build_root = sanitizer_root / "build" / owner[0]
        expected_artifact = (
            expected_build_root / library["build_artifact_relative_path"]
        ).as_posix()
        expected_source_root = sanitizer_root / "source" / owner[0]
        source_paths: list[str] = []
        for source_index, raw_source in enumerate(
            _require_list(target.get("file_api_sources"), f"{target_label}.sources")
        ):
            source = _require_object(
                raw_source, f"{target_label}.sources[{source_index}]"
            )
            _exact_keys(
                source,
                {"path", "sha256", "cross_profile_sha256"},
                f"{target_label}.sources[{source_index}]",
            )
            source_paths.append(
                _require_text(source["path"], f"{target_label}.source path")
            )
        expected_source_paths = [
            (expected_source_root / path).as_posix()
            for path in library["required_sources"]
        ]
        if (
            target.get("target_type") != library["type"]
            or target.get("contract_artifact")
            != library["build_artifact_relative_path"]
            or artifact.get("path") != expected_artifact
            or not isinstance(artifact.get("resolved_path"), str)
            or not artifact["resolved_path"].startswith(
                f"{expected_build_root.as_posix()}/"
            )
            or HEX64.fullmatch(
                _require_text(artifact.get("sha256"), f"{target_label}.artifact digest")
            )
            is None
            or source_paths != expected_source_paths
        ):
            fail(f"{target_label} auxiliary target contract drifted")
        target_index[owner] = target
    if set(target_index) != set(expected_libraries):
        fail(f"{label} auxiliary target proof set drifted")
    project_index: dict[str, dict[str, Any]] = {}
    inventories: dict[str, dict[str, Any]] = {}
    for index, raw_project in enumerate(projects):
        project_label = f"{label} project[{index}]"
        project = _require_object(raw_project, project_label)
        _exact_keys(
            project,
            {
                "project_id",
                "install_root",
                "manifest",
                "install_tree_inventory",
                "owner_counts",
                "non_library_owner_projection_sha256",
                "owner_partition_sha256",
            },
            project_label,
        )
        project_id = _require_text(
            project["project_id"], f"{project_label}.project_id"
        )
        if project_id in project_index:
            fail(f"{label} repeats install project {project_id}")
        if project_id not in contract_projects:
            fail(f"{project_label} references an uncontracted project")
        contract_project = _require_object(
            contract_projects[project_id], f"{project_label} contract"
        )
        contract_manifest = _require_object(
            contract_project["manifest"], f"{project_label} manifest contract"
        )
        install_root = _require_text(
            project["install_root"], f"{project_label}.install_root"
        )
        if (
            posixpath.normpath(install_root) != install_root
            or not install_root.startswith("/qualification-output/")
            or install_root != expected_install_roots[project_id]
        ):
            fail(f"{project_label}.install_root is not canonical")
        inventory = _validate_bounded_tree_inventory(
            project["install_tree_inventory"],
            lane_root=lane_root,
            label=f"{project_label} install tree",
        )
        if inventory["root"] != install_root:
            fail(f"{project_label} install-tree root drifted")
        manifest = _require_object(project["manifest"], f"{project_label} manifest")
        _exact_keys(
            manifest,
            {
                "path",
                "sha256",
                "row_count",
                "distinct_path_count",
                "relative_rows_sha256",
                "repeated_relative_paths",
            },
            f"{project_label} manifest",
        )
        manifest_path = _require_text(
            manifest["path"], f"{project_label} manifest.path"
        )
        manifest_digest = _require_text(
            manifest["sha256"], f"{project_label} manifest.sha256"
        )
        if (
            posixpath.normpath(manifest_path) != manifest_path
            or not manifest_path.startswith("/qualification-output/")
            or HEX64.fullmatch(manifest_digest) is None
            or manifest_path
            != (
                PurePosixPath(install_root).parents[1]
                / "build"
                / project_id
                / "install_manifest.txt"
            ).as_posix()
        ):
            fail(f"{project_label} manifest path/digest is malformed")
        physical_manifest = _require_contained_regular_file(
            lane_root / Path(manifest_path).relative_to("/qualification-output"),
            lane_root,
            f"{project_label} manifest",
            require_single_link=True,
        )
        raw_manifest = physical_manifest.read_bytes()
        if hashlib.sha256(raw_manifest).hexdigest() != manifest_digest:
            fail(f"{project_label} manifest changed after native validation")
        try:
            manifest_text = raw_manifest.decode("utf-8")
        except UnicodeDecodeError as error:
            fail(f"{project_label} manifest is not UTF-8: {error}")
        lines = manifest_text.splitlines()
        if (
            not raw_manifest.endswith(b"\n")
            or not lines
            or any(not line or "\r" in line for line in lines)
        ):
            fail(f"{project_label} manifest line format drifted")
        relative_rows: list[str] = []
        install_root_pure = PurePosixPath(install_root)
        for line_index, line in enumerate(lines):
            if posixpath.normpath(line) != line or not line.startswith("/"):
                fail(f"{project_label} manifest[{line_index}] is not canonical")
            try:
                relative = PurePosixPath(line).relative_to(install_root_pure)
            except ValueError:
                fail(f"{project_label} manifest[{line_index}] escapes its prefix")
            if not relative.parts:
                fail(f"{project_label} manifest[{line_index}] names the prefix")
            relative_rows.append(relative.as_posix())
        counts = Counter(relative_rows)
        repeated_rows = [
            {"path": path, "count": count}
            for path, count in sorted(counts.items())
            if count > 1
        ]
        file_paths = {
            row["relative_path"]
            for row in inventory["entries"]
            if row["kind"] == "file"
        }
        if (
            manifest["row_count"] != len(relative_rows)
            or manifest["distinct_path_count"] != len(counts)
            or manifest["relative_rows_sha256"]
            != _canonical_json_sha256(relative_rows)
            or manifest["repeated_relative_paths"] != repeated_rows
            or set(counts) != file_paths
            or {
                key: manifest[key]
                for key in (
                    "row_count",
                    "distinct_path_count",
                    "relative_rows_sha256",
                    "repeated_relative_paths",
                )
            }
            != contract_manifest
        ):
            fail(f"{project_label} manifest/tree multiplicity drifted")
        owner_counts = _require_object(
            project["owner_counts"], f"{project_label}.owner_counts"
        )
        _exact_keys(
            owner_counts,
            {"git_source_copy", "cmake_generated", "target_artifact"},
            f"{project_label}.owner_counts",
        )
        if (
            not all(
                isinstance(count, int)
                and not isinstance(count, bool)
                and count >= 0
                for count in owner_counts.values()
            )
            or sum(owner_counts.values()) != len(file_paths)
            or not all(
                isinstance(project[field], str)
                and HEX64.fullmatch(project[field]) is not None
                for field in (
                    "non_library_owner_projection_sha256",
                    "owner_partition_sha256",
                )
            )
            or owner_counts
            != {
                "git_source_copy": contract_project["expected_source_copy_count"],
                "cmake_generated": len(contract_project["generated_files"]),
                "target_artifact": len(contract_project["library_files"]),
            }
        ):
            fail(f"{project_label} owner-count/digest shape drifted")
        project_index[project_id] = project
        inventories[project_id] = inventory

    if set(project_index) != {"roboplan", "roboplan_example_models"}:
        fail(f"{label} auxiliary install project set drifted")
    owner_projection: dict[str, dict[str, dict[str, str]]] = {
        project_id: {} for project_id in project_index
    }
    non_library_projection: dict[str, list[dict[str, str]]] = {
        project_id: [] for project_id in project_index
    }
    actual_owner_counts: dict[str, Counter[str]] = {
        project_id: Counter() for project_id in project_index
    }
    for index, raw_input in enumerate(qualified_inputs):
        input_label = f"{label} qualified input[{index}]"
        row = _require_object(raw_input, input_label)
        _exact_keys(
            row,
            {
                "path",
                "resolved_path",
                "symlink_chain",
                "sha256",
                "project_id",
                "install_relative_path",
                "install_manifest",
                "producer",
            },
            input_label,
        )
        project_id = _require_text(row["project_id"], f"{input_label}.project_id")
        if project_id not in project_index:
            fail(f"{input_label} references an unknown install project")
        relative = _safe_relative_path(
            _require_text(
                row["install_relative_path"],
                f"{input_label}.install_relative_path",
            ),
            f"{input_label}.install_relative_path",
        ).as_posix()
        producer = _require_object(row["producer"], f"{input_label}.producer")
        producer_kind = _require_text(
            producer.get("kind"), f"{input_label}.producer.kind"
        )
        if producer_kind not in {
            "git_tracked_install_source",
            "cmake_generated_install_source",
        }:
            fail(f"{input_label} producer kind drifted")
        owner = _require_text(producer.get("path"), f"{input_label}.producer.path")
        contract_project = contract_projects[project_id]
        sanitizer_root = PurePosixPath(expected_install_roots[project_id]).parents[1]
        source_root = sanitizer_root / "source" / project_id
        build_root = sanitizer_root / "build" / project_id
        source_git_root = sanitizer_root / "source"
        if producer_kind == "git_tracked_install_source":
            _exact_keys(
                producer,
                {"kind", "path", "sha256", "source_identity"},
                f"{input_label}.producer",
            )
            candidates: list[str] = []
            relative_path = PurePosixPath(relative)
            for raw_prefix in contract_project["source_copy_prefixes"]:
                prefix = _require_object(raw_prefix, f"{input_label} source prefix")
                install_prefix = PurePosixPath(prefix["install_prefix"])
                if relative_path.is_relative_to(install_prefix):
                    suffix = relative_path.relative_to(install_prefix)
                    candidates.append(
                        (
                            source_root
                            / PurePosixPath(prefix["source_prefix"])
                            / suffix
                        ).as_posix()
                    )
            if candidates != [owner] or producer.get("sha256") != row["sha256"]:
                fail(f"{input_label} Git install owner mapping drifted")
            physical_owner = _require_contained_regular_file(
                lane_root / Path(owner).relative_to("/qualification-output"),
                lane_root,
                f"{input_label} Git install owner",
                require_single_link=True,
            )
            if _sha256_file(physical_owner) != row["sha256"]:
                fail(f"{input_label} Git install owner bytes drifted")
            _validate_git_tracked_file_evidence(
                producer["source_identity"],
                physical_file=physical_owner,
                expected_git_root=source_git_root.as_posix(),
                expected_repository_path=PurePosixPath(owner)
                .relative_to(source_git_root)
                .as_posix(),
                label=f"{input_label} Git install owner identity",
            )
        else:
            _exact_keys(
                producer,
                {
                    "kind",
                    "producer_kind",
                    "path",
                    "sha256",
                    "cmake_recipe",
                    "template",
                    "required_logical_targets",
                },
                f"{input_label}.producer",
            )
            generated_matches = [
                raw
                for raw in contract_project["generated_files"]
                if raw["install_relative_path"] == relative
            ]
            if len(generated_matches) != 1:
                fail(f"{input_label} generated install owner is uncontracted")
            generated = generated_matches[0]
            cmake_recipe = _require_object(
                producer["cmake_recipe"], f"{input_label}.cmake_recipe"
            )
            _exact_keys(
                cmake_recipe,
                {"path", "sha256", "source_identity"},
                f"{input_label}.cmake_recipe",
            )
            expected_template = generated["template_source_relative_path"]
            template = producer["template"]
            if expected_template is None:
                if template is not None:
                    fail(f"{input_label} generated owner has an extra template")
            else:
                template = _require_object(template, f"{input_label}.template")
                _exact_keys(
                    template,
                    {"path", "sha256", "source_identity"},
                    f"{input_label}.template",
                )
                if template.get("path") != (source_root / expected_template).as_posix():
                    fail(f"{input_label} generated owner template drifted")
            if (
                producer["producer_kind"] != generated["producer_kind"]
                or owner != (build_root / generated["build_relative_path"]).as_posix()
                or producer["sha256"] != row["sha256"]
                or cmake_recipe.get("path")
                != (source_root / generated["cmake_source_relative_path"]).as_posix()
                or producer["required_logical_targets"]
                != generated["required_logical_targets"]
            ):
                fail(f"{input_label} generated install owner mapping drifted")
            physical_generated = _require_contained_regular_file(
                lane_root / Path(owner).relative_to("/qualification-output"),
                lane_root,
                f"{input_label} generated install owner",
                require_single_link=True,
            )
            if _sha256_file(physical_generated) != row["sha256"]:
                fail(f"{input_label} generated install owner bytes drifted")
            recipe_path = _require_text(
                cmake_recipe["path"], f"{input_label}.cmake_recipe.path"
            )
            physical_recipe = _require_contained_regular_file(
                lane_root / Path(recipe_path).relative_to("/qualification-output"),
                lane_root,
                f"{input_label} generated install recipe",
                require_single_link=True,
            )
            recipe_digest = _require_text(
                cmake_recipe["sha256"], f"{input_label}.cmake_recipe.sha256"
            )
            if (
                HEX64.fullmatch(recipe_digest) is None
                or _sha256_file(physical_recipe) != recipe_digest
            ):
                fail(f"{input_label} generated install recipe bytes drifted")
            _validate_git_tracked_file_evidence(
                cmake_recipe["source_identity"],
                physical_file=physical_recipe,
                expected_git_root=source_git_root.as_posix(),
                expected_repository_path=(
                    PurePosixPath(project_id)
                    / generated["cmake_source_relative_path"]
                ).as_posix(),
                label=f"{input_label} generated install recipe identity",
            )
            if template is not None:
                template_path = _require_text(
                    template["path"], f"{input_label}.template.path"
                )
                physical_template = _require_contained_regular_file(
                    lane_root
                    / Path(template_path).relative_to("/qualification-output"),
                    lane_root,
                    f"{input_label} generated install template",
                    require_single_link=True,
                )
                template_digest = _require_text(
                    template["sha256"], f"{input_label}.template.sha256"
                )
                if (
                    HEX64.fullmatch(template_digest) is None
                    or _sha256_file(physical_template) != template_digest
                ):
                    fail(f"{input_label} generated install template bytes drifted")
                _validate_git_tracked_file_evidence(
                    template["source_identity"],
                    physical_file=physical_template,
                    expected_git_root=source_git_root.as_posix(),
                    expected_repository_path=(
                        PurePosixPath(project_id) / expected_template
                    ).as_posix(),
                    label=f"{input_label} generated install template identity",
                )
            try:
                generated_text = physical_generated.read_text(encoding="utf-8")
                recipe_text = physical_recipe.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as error:
                fail(f"{input_label} generated CMake evidence is unreadable: {error}")
            producer_kind_value = generated["producer_kind"]
            if producer_kind_value == "configure_package_config_file_final":
                output_name = PurePosixPath(generated["build_relative_path"]).name
                writer_kinds = [
                    kind
                    for kind, body in re.findall(
                        r"\b(configure_file|configure_package_config_file)\s*\((.*?)\)",
                        recipe_text,
                        flags=re.DOTALL,
                    )
                    if output_name in body
                ]
                if writer_kinds != [
                    "configure_file",
                    "configure_package_config_file",
                ]:
                    fail(f"{input_label} generated Config.cmake writer order drifted")
            elif producer_kind_value == "write_basic_package_version_file":
                output_name = PurePosixPath(generated["build_relative_path"]).name
                matches = [
                    body
                    for body in re.findall(
                        r"\bwrite_basic_package_version_file\s*\((.*?)\)",
                        recipe_text,
                        flags=re.DOTALL,
                    )
                    if output_name in body
                ]
                if len(matches) != 1:
                    fail(f"{input_label} generated ConfigVersion writer drifted")
            elif producer_kind_value == "install_export_main":
                actual_targets = sorted(
                    set(
                        re.findall(
                            r"\badd_library\(([^ )]+) "
                            r"(?:STATIC|SHARED|MODULE|INTERFACE|UNKNOWN) IMPORTED\)",
                            generated_text,
                        )
                    )
                )
                if actual_targets != generated["required_logical_targets"]:
                    fail(f"{input_label} generated main-export targets drifted")
            elif producer_kind_value == "install_export_configuration":
                actual_targets = sorted(
                    set(
                        re.findall(
                            r"\bset_property\(TARGET ([^ )]+) APPEND PROPERTY "
                            r"IMPORTED_CONFIGURATIONS RELWITHDEBINFO\)",
                            generated_text,
                        )
                    )
                )
                if actual_targets != generated["required_logical_targets"]:
                    fail(
                        f"{input_label} generated configuration-export targets "
                        "drifted"
                    )
            else:
                fail(f"{input_label} generated install producer kind is unsupported")
        expected_path = f"{project_index[project_id]['install_root']}/{relative}"
        if (
            row["path"] != expected_path
            or row["resolved_path"] != expected_path
            or row["symlink_chain"] != []
            or row["install_manifest"] != project_index[project_id]["manifest"]
        ):
            fail(f"{input_label} install path/manifest join drifted")
        if relative in owner_projection[project_id]:
            fail(f"{input_label} repeats an install owner")
        owner_projection[project_id][relative] = {
            "path": relative,
            "kind": producer_kind,
            "owner": owner,
        }
        non_library_projection[project_id].append(
            {
                "install_relative_path": relative,
                "kind": producer_kind,
                "owner": owner,
            }
        )
        actual_owner_counts[project_id][
            "git_source_copy"
            if producer_kind == "git_tracked_install_source"
            else "cmake_generated"
        ] += 1
    for index, raw_copy in enumerate(installed_copies):
        copy_label = f"{label} installed copy[{index}]"
        row = _require_object(raw_copy, copy_label)
        _exact_keys(
            row,
            {
                "project_id",
                "path",
                "resolved_path",
                "symlink_chain",
                "sha256",
                "install_relative_path",
                "producer_project",
                "producer_target",
                "producer_artifact",
                "install_manifest",
            },
            copy_label,
        )
        project_id = _require_text(row["project_id"], f"{copy_label}.project_id")
        if project_id not in project_index or row["producer_project"] != project_id:
            fail(f"{copy_label} has the wrong project owner")
        relative = _safe_relative_path(
            _require_text(
                row["install_relative_path"],
                f"{copy_label}.install_relative_path",
            ),
            f"{copy_label}.install_relative_path",
        ).as_posix()
        expected_path = f"{project_index[project_id]['install_root']}/{relative}"
        library_matches = [
            raw
            for raw in contract_projects[project_id]["library_files"]
            if raw["install_relative_path"] == relative
        ]
        if len(library_matches) != 1:
            fail(f"{copy_label} installed library copy is uncontracted")
        target_proof = target_index[(project_id, library_matches[0]["target"])]
        target_artifact = _require_object(
            target_proof["artifact"], f"{copy_label} target artifact"
        )
        if (
            row["path"] != expected_path
            or row["resolved_path"] != expected_path
            or row["symlink_chain"] != []
            or row["install_manifest"] != project_index[project_id]["manifest"]
            or relative in owner_projection[project_id]
            or row["producer_target"] != library_matches[0]["target"]
            or row["producer_artifact"]
            != target_artifact["resolved_path"]
            or row["sha256"] != target_artifact["sha256"]
        ):
            fail(f"{copy_label} install path/manifest join drifted")
        owner_projection[project_id][relative] = {
            "path": relative,
            "kind": "target_artifact",
            "owner": _require_text(
                row["producer_target"], f"{copy_label}.producer_target"
            ),
        }
        actual_owner_counts[project_id]["target_artifact"] += 1

    for project_id, project in project_index.items():
        files = {
            row["relative_path"]
            for row in inventories[project_id]["entries"]
            if row["kind"] == "file"
        }
        projection = owner_projection[project_id]
        expected_counts = project["owner_counts"]
        actual_counts = actual_owner_counts[project_id]
        if (
            set(projection) != files
            or {
                key: actual_counts.get(key, 0)
                for key in (
                    "git_source_copy",
                    "cmake_generated",
                    "target_artifact",
                )
            }
            != expected_counts
            or _canonical_json_sha256(
                [projection[path] for path in sorted(projection)]
            )
            != project["owner_partition_sha256"]
            or _canonical_json_sha256(
                sorted(
                    non_library_projection[project_id],
                    key=lambda row: row["install_relative_path"],
                )
            )
            != project["non_library_owner_projection_sha256"]
        ):
            fail(f"{label} {project_id} install owner partition drifted")


def _require_test_enabled_release_product_parity(
    production_value: Any,
    native_provenance_value: Any,
    *,
    production_lane_id: str,
    native_lane_id: str,
) -> None:
    production = _require_list(
        production_value, f"{production_lane_id} production parity"
    )
    native_provenance = _require_object(
        native_provenance_value, f"{native_lane_id} native provenance"
    )
    if native_provenance.get("production_parity") != production:
        fail(
            f"{production_lane_id} BUILD_TESTING=OFF products differ from "
            f"{native_lane_id} test-enabled Release products"
        )


def _validate_production_builds(
    manifest: dict[str, Any],
    run_root: Path,
    successful_lanes: set[str],
    repository_root: Path,
    expected_manyforge_generation: dict[str, Any],
) -> list[dict[str, Any]]:
    summaries: list[dict[str, Any]] = []
    build_contract = _load_build_provenance_contract(
        manifest["test_inventory_contract"], manifest, repository_root
    )
    native_rows = {
        row["lane_id"]: row
        for row in manifest["test_inventory_contract"]["native_execution_lanes"]
    }
    for row in manifest["test_inventory_contract"]["production_build_lanes"]:
        lane_id = row["lane_id"]
        if lane_id not in successful_lanes:
            continue
        lane_root = _lane_artifact_root(run_root, lane_id)
        inventory_path = _require_output_file(
            lane_root,
            row["inventory_output"],
            f"{lane_id} production CTest inventory",
        )
        decoded = _read_json_object(
            inventory_path, f"{lane_id} production CTest inventory"
        )
        tests = _require_list(
            decoded.get("tests"), f"{lane_id} production CTest inventory tests"
        )
        if tests:
            fail(f"{lane_id} BUILD_TESTING=OFF build registered tests")
        provenance_path = _require_output_file(
            lane_root,
            row["provenance_output"],
            f"{lane_id} production build provenance",
        )
        provenance = _read_json_object(
            provenance_path, f"{lane_id} production build provenance"
        )
        _exact_keys(
            provenance,
            {
                "schema_version",
                "lane_id",
                "project_id",
                "build_root",
                "provenance_output",
                "empty_ctest_inventory_sha256",
                "project",
                "targets",
                "utility_target_relationships",
                "production_parity",
                "response_files",
                "inspection_tools",
                "passed",
                "evidence_sha256",
            },
            f"{lane_id} production build provenance",
        )
        evidence_digest = _require_text(
            provenance["evidence_sha256"],
            f"{lane_id} production provenance evidence digest",
        )
        digest_input = dict(provenance)
        del digest_input["evidence_sha256"]
        parity = _require_list(
            provenance["production_parity"], f"{lane_id} production parity"
        )
        if (
            provenance["schema_version"]
            != "manyforge.composable_cell_i2.production_build_provenance.v1"
            or provenance["lane_id"] != lane_id
            or provenance["project_id"] != row["project_id"]
            or provenance["build_root"]
            != f"/qualification-output/{row['build_root']}"
            or provenance["provenance_output"]
            != f"/qualification-output/{row['provenance_output']}"
            or provenance["empty_ctest_inventory_sha256"]
            != _canonical_json_sha256(decoded)
            or not _require_list(
                provenance["targets"], f"{lane_id} production targets"
            )
            or not parity
            or provenance["passed"] is not True
            or not HEX64.fullmatch(evidence_digest)
            or _canonical_json_sha256(digest_input) != evidence_digest
        ):
            fail(f"{lane_id} production build provenance identity drifted")
        utility_relationships = _validate_utility_target_relationships(
            provenance["utility_target_relationships"],
            provenance["targets"],
            f"{lane_id} production utility targets",
            lane_root=lane_root,
        )
        _validate_project_source_generations(
            [provenance["project"]],
            build_contract=build_contract,
            lane_root=lane_root,
            repository_root=repository_root,
            expected_manyforge_generation=expected_manyforge_generation,
            expected_projects={
                row["project_id"]: (
                    f"/workspace/{row['source_root']}",
                    f"/qualification-output/{row['build_root']}",
                )
            },
            expected_configuration="Release",
            expected_build_testing={row["project_id"]: False},
            expected_auxiliary_git_root=None,
            expected_auxiliary_capture_path=None,
            label=f"{lane_id} production build provenance",
        )
        project_contract = _require_object(
            _require_object(build_contract["projects"], "production projects").get(
                row["project_id"]
            ),
            f"{lane_id} production target contract",
        )
        production_names = {
            _require_text(
                _require_object(raw, f"{lane_id} production target spec").get("name"),
                f"{lane_id} production target name",
            )
            for raw in _require_list(
                project_contract.get("targets"), f"{lane_id} production target specs"
            )
        }
        production_specs = _product_projection_specs(
            build_contract,
            project_id=row["project_id"],
            target_names=production_names,
            catalogs=("targets",),
            label=f"{lane_id} production projection",
        )
        production_build_roots = {
            row["project_id"]: f"/qualification-output/{row['build_root']}"
        }
        _validate_target_command_file_evidence(
            provenance["targets"],
            project_build_roots=production_build_roots,
            target_output_owners=provenance["targets"],
            installed_output_owners=[],
            expected_roles=frozenset({"production"}),
            sanitizer_profile="none",
            lane_root=lane_root,
            repository_root=repository_root,
            label=f"{lane_id} production target command evidence",
        )
        _validate_target_proofs_against_projection(
            provenance["targets"],
            parity,
            expected=production_specs,
            build_roots=production_build_roots,
            source_roots={
                row["project_id"]: f"/workspace/{row['source_root']}"
            },
            install_roots={},
            configure_generated_specs=_configure_generated_dependency_specs(
                build_contract, label=f"{lane_id} production parity"
            ),
            qualified_install_owners={},
            project_provenance={row["project_id"]: provenance["project"]},
            repository_root=repository_root,
            lane_root=lane_root,
            allow_unexpected_proofs=False,
            label=f"{lane_id} production parity",
        )
        authenticated_files = _authenticate_provenance_files(
            provenance,
            lane_root=lane_root,
            repository_root=repository_root,
            label=f"{lane_id} production build provenance",
        )
        index_path = _require_output_file(
            lane_root,
            row["execution_index_output"],
            f"{lane_id} production build execution index",
        )
        index = _read_json_object(
            index_path, f"{lane_id} production build execution index"
        )
        _exact_keys(
            index,
            {
                "schema_version",
                "lane_id",
                "project_id",
                "environment",
                "configure_argv",
                "configure_exit_status",
                "build_argv",
                "build_exit_status",
                "inventory_argv",
                "inventory_exit_status",
                "inventory_path",
                "inventory_sha256",
                "provenance_path",
                "provenance_sha256",
                "provenance_evidence_sha256",
                "passed",
            },
            f"{lane_id} production build execution index",
        )
        if index != {
            "schema_version": "manyforge.composable_cell_i2.production_build_execution.v1",
            "lane_id": lane_id,
            "project_id": row["project_id"],
            "environment": _production_build_environment(lane_id),
            "configure_argv": row["configure_argv"],
            "configure_exit_status": 0,
            "build_argv": row["build_argv"],
            "build_exit_status": 0,
            "inventory_argv": row["inventory_argv"],
            "inventory_exit_status": 0,
            "inventory_path": row["inventory_output"],
            "inventory_sha256": _sha256_file(inventory_path),
            "provenance_path": row["provenance_output"],
            "provenance_sha256": _sha256_file(provenance_path),
            "provenance_evidence_sha256": evidence_digest,
            "passed": True,
        }:
            fail(f"{lane_id} production build execution index drifted")

        parity_lane_id = row["test_enabled_release_lane_id"]
        if parity_lane_id not in successful_lanes:
            fail(f"{lane_id} lacks its required test-enabled Release parity lane")
        native_row = native_rows[parity_lane_id]
        native_provenance_path = _require_output_file(
            _lane_artifact_root(run_root, parity_lane_id),
            native_row["provenance_output"],
            f"{parity_lane_id} native provenance for production parity",
        )
        native_provenance = _read_json_object(
            native_provenance_path,
            f"{parity_lane_id} native provenance for production parity",
        )
        _require_test_enabled_release_product_parity(
            parity,
            native_provenance,
            production_lane_id=lane_id,
            native_lane_id=parity_lane_id,
        )
        parity_evidence = _file_evidence(native_provenance_path, run_root)
        summaries.append(
            {
                "lane_id": lane_id,
                "project_id": row["project_id"],
                "test_count": 0,
                "test_names_sha256": _name_digest(set()),
                "inventory": _file_evidence(inventory_path, run_root),
                "production_build_provenance": _file_evidence(
                    provenance_path, run_root
                ),
                "execution_index": _file_evidence(index_path, run_root),
                "authenticated_provenance_files": authenticated_files,
                "test_enabled_release_lane_id": parity_lane_id,
                "test_enabled_release_provenance": parity_evidence,
            }
        )
    return summaries


def _unique_text_set(value: Any, label: str) -> set[str]:
    items = _require_list(value, label)
    if not all(isinstance(item, str) and item for item in items):
        fail(f"{label} must contain non-empty strings")
    if len(items) != len(set(items)):
        fail(f"{label} contains duplicates")
    return set(items)


def _gtest_json_cases(path: Path, label: str) -> dict[str, str]:
    decoded = _read_json_object(path, label)
    suites = _require_list(decoded.get("testsuites"), f"{label}.testsuites")
    cases: dict[str, str] = {}
    for suite_index, raw_suite in enumerate(suites):
        suite = _require_object(raw_suite, f"{label}.testsuites[{suite_index}]")
        suite_name = _require_text(
            suite.get("name"), f"{label}.testsuites[{suite_index}].name"
        )
        tests = _require_list(
            suite.get("testsuite"), f"{label}.testsuites[{suite_index}].testsuite"
        )
        for case_index, raw_case in enumerate(tests):
            case = _require_object(
                raw_case,
                f"{label}.testsuites[{suite_index}].testsuite[{case_index}]",
            )
            case_name = _require_text(
                case.get("name"),
                f"{label}.testsuites[{suite_index}].testsuite[{case_index}].name",
            )
            name = f"{suite_name}.{case_name}"
            if name in cases:
                fail(f"{label} contains a duplicate GTest case: {name}")
            failures = case.get("failures", [])
            if not isinstance(failures, list):
                fail(f"{label} GTest failure list is malformed: {name}")
            status = "passed"
            if failures or case.get("result") != "COMPLETED":
                status = "failed"
            elif case.get("status") not in {None, "RUN"}:
                status = "skipped"
            cases[name] = status
    if not cases:
        fail(f"{label} contains zero native GTest cases")
    if decoded.get("failures") not in (None, 0) or decoded.get("errors") not in (
        None,
        0,
    ):
        fail(f"{label} native GTest aggregate reports failure")
    return cases


def _has_sanitizer_signature(content: str) -> bool:
    signatures = (
        "ERROR: AddressSanitizer",
        "ERROR: LeakSanitizer",
        "SUMMARY: AddressSanitizer",
        "SUMMARY: UndefinedBehaviorSanitizer",
        "WARNING: ThreadSanitizer",
        "SUMMARY: ThreadSanitizer",
        "FATAL: ThreadSanitizer",
        "runtime error:",
    )
    return any(signature in content for signature in signatures)


def _ctest_truthy(value: Any) -> bool:
    if value is True:
        return True
    if isinstance(value, int) and not isinstance(value, bool):
        return value != 0
    return isinstance(value, str) and value.upper() in {"1", "ON", "TRUE", "YES"}


def _ctest_properties(test: dict[str, Any], label: str) -> dict[str, Any]:
    properties = _require_list(test.get("properties", []), f"{label}.properties")
    result: dict[str, Any] = {}
    for index, raw in enumerate(properties):
        row = _require_object(raw, f"{label}.properties[{index}]")
        name = _require_text(row.get("name"), f"{label}.properties[{index}].name")
        if name in result:
            fail(f"{label} contains a duplicate CTest property: {name}")
        result[name] = row.get("value")
    return result


def _ctest_skip_marker_is_exact(value: Any) -> bool:
    """Match CTest 3.25's JSON representation of the exact GTest skip marker."""

    return value == [r"\[  SKIPPED \]"]


def _ctest_environment(value: Any, label: str) -> dict[str, str]:
    if value is None:
        return {}
    if isinstance(value, str):
        entries = value.split(";") if value else []
    elif isinstance(value, list) and all(isinstance(entry, str) for entry in value):
        entries = value
    else:
        fail(f"{label} CTest ENVIRONMENT property is malformed")
    result: dict[str, str] = {}
    for entry in entries:
        name, separator, item = entry.partition("=")
        if (
            separator != "="
            or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name)
            or name in result
            or "\x00" in item
        ):
            fail(f"{label} CTest ENVIRONMENT entry is malformed or duplicated")
        result[name] = item
    return result


def _expected_native_ordinary(
    name: str, build_root: str, source: str
) -> tuple[list[str], dict[str, str]]:
    build = f"/qualification-output/{build_root}"
    if name == "BuildManifestAuthorityRejectsMultiConfigGenerator":
        return (
            [
                "/usr/bin/cmake",
                "-DMANYFORGE_TEST_SOURCE_DIR=/workspace/manyforge_deployment",
                f"-DMANYFORGE_TEST_BINARY_DIR={build}/multi_config_rejection",
                "-P",
                source,
            ],
            {},
        )
    if name == "CanonicalEmptyGeneratorCli":
        return (
            [
                "/usr/bin/cmake",
                (
                    "-DMANYFORGE_GENERATOR="
                    f"{build}/manyforge_generate_canonical_empty_package"
                ),
                f"-DMANYFORGE_TEST_ROOT={build}/canonical_empty_cli_package",
                "-P",
                source,
            ],
            {},
        )
    if name == "BuildQualifiedAdmissionProfileIsDeterministic":
        return (
            [
                "/usr/bin/cmake",
                (
                    "-DMANYFORGE_PROFILE_EMITTER="
                    f"{build}/manyforge_emit_build_qualified_admission_profile"
                ),
                (
                    "-DMANYFORGE_GENERATED_PROFILE="
                    f"{build}/generated/build_qualified_admission_profile_generated.hpp"
                ),
                f"-DMANYFORGE_TEST_ROOT={build}/profile_determinism",
                "-P",
                source,
            ],
            {},
        )
    common_environment = {
        "PYTHONOPTIMIZE": "1",
        "PYTHONPATH": "/workspace",
        "LD_LIBRARY_PATH": ROBOPLAN_PINNED_LD_LIBRARY_PATH,
    }
    if name == "retained_compiled_visual_fk_composer_consumer":
        return (
            [
                "/usr/local/bin/python3",
                source,
                "--fixture-root",
                f"{build}/lite6_retained_consumer_fixture",
            ],
            common_environment,
        )
    if name == "retained_compiled_model_independent_standard_parser":
        return (
            [
                "/usr/local/bin/python3",
                source,
                "--fixture-root",
                f"{build}/lite6_retained_consumer_fixture",
                "--urdfdom-checker",
                "/usr/local/bin/check_urdf",
            ],
            common_environment,
        )
    fail(f"ordinary native test has no exact command contract: {name}")


def _validate_native_gtest_environment(
    *, lane_id: str, build_root: str, required_group: str, environment: dict[str, str]
) -> None:
    if not required_group.startswith("roboplan_"):
        if environment:
            fail(f"{lane_id} native GTest unexpectedly mutates its environment")
        return
    if set(environment) != {"LD_LIBRARY_PATH"}:
        fail(f"{lane_id} RoboPlan GTest environment keys drifted")
    value = environment["LD_LIBRARY_PATH"]
    if lane_id in {"roboplan_release", "roboplan_consumer_focused"}:
        if value != ROBOPLAN_PINNED_LD_LIBRARY_PATH:
            fail(f"{lane_id} RoboPlan GTest library path drifted")
        return
    sanitizer_root = Path("/qualification-output") / Path(build_root).parents[1]
    allowed_local = {
        (sanitizer_root / "install/roboplan/lib").as_posix(),
        (sanitizer_root / "install/roboplan_example_models/lib").as_posix(),
    }
    allowed_external = {
        "/usr/local/lib",
        "/usr/local/lib/python3.12/site-packages/cmeel.prefix/lib",
    }
    entries = value.split(":")
    if (
        not entries
        or any(not entry for entry in entries)
        or not allowed_local.issubset(set(entries))
        or not allowed_external.issubset(set(entries))
        or not set(entries).issubset(allowed_local | allowed_external)
    ):
        fail(f"{lane_id} RoboPlan sanitizer library path escaped its exact closure")


def _validate_native_target_mapping(
    inventory_path: Path,
    *,
    lane_id: str,
    build_root: str,
    required_group: str,
    required_names: set[str],
    allow_additional_tests: bool,
    target_groups: dict[str, dict[str, tuple[int, str]]],
    ordinary_sources: dict[str, tuple[str, str, str]],
) -> dict[str, Any]:
    """Bind every frozen CTest name to its exact native target or ordinary source."""

    inventory = _read_json_object(
        inventory_path, f"{lane_id} native target-mapping inventory"
    )
    tests = _require_list(
        inventory.get("tests"), f"{lane_id} native target-mapping tests"
    )
    expected_targets = target_groups[required_group]
    expected_ordinary = NATIVE_ORDINARY_BY_GROUP[required_group]
    actual_targets: dict[str, set[str]] = {
        target: set() for target in expected_targets
    }
    actual_target_artifacts: dict[str, set[str]] = {
        target: set() for target in expected_targets
    }
    actual_ordinary: dict[str, list[str]] = {}
    seen: set[str] = set()
    expected_build = Path("/qualification-output") / build_root

    for index, raw in enumerate(tests):
        test = _require_object(raw, f"{lane_id} native target test {index}")
        name = _require_text(test.get("name"), f"{lane_id} native target name")
        if name in seen:
            fail(f"{lane_id} native target registry duplicates {name}")
        seen.add(name)
        command = _require_list(
            test.get("command"), f"{lane_id} native target command {name}"
        )
        if not command or not all(
            isinstance(value, str) and value for value in command
        ):
            fail(f"{lane_id} native target command is malformed: {name}")
        properties = _ctest_properties(
            test, f"{lane_id} native target properties {name}"
        )
        filters = [
            value.removeprefix("--gtest_filter=")
            for value in command[1:]
            if value.startswith("--gtest_filter=")
        ]
        if filters:
            if len(filters) != 1 or filters[0] != name:
                fail(f"{lane_id} GTest registration is not exact: {name}")
            if posixpath.normpath(command[0]) != command[0]:
                fail(f"{lane_id} GTest binary path is not canonical: {name}")
            binary = Path(command[0])
            if not binary.is_absolute() or not binary.is_relative_to(expected_build):
                fail(f"{lane_id} GTest binary escapes its exact build: {name}")
            target = binary.name
            if target not in expected_targets:
                fail(
                    f"{lane_id} CTest name is registered by an unqualified target: "
                    f"{name} -> {target}"
                )
            if command != [
                command[0],
                f"--gtest_filter={name}",
                "--gtest_also_run_disabled_tests",
            ]:
                fail(f"{lane_id} GTest argv drifted: {name}")
            required_properties = {
                "WORKING_DIRECTORY",
                "SKIP_REGULAR_EXPRESSION",
                "TIMEOUT",
            }
            allowed_properties = set(required_properties)
            if required_group.startswith("roboplan_"):
                allowed_properties.add("ENVIRONMENT")
                required_properties.add("ENVIRONMENT")
            if (
                not required_properties.issubset(properties)
                or not set(properties).issubset(allowed_properties)
                or properties["WORKING_DIRECTORY"] != binary.parent.as_posix()
                or not _ctest_skip_marker_is_exact(
                    properties["SKIP_REGULAR_EXPRESSION"]
                )
                or _native_timeout(properties, name)
                != NATIVE_GTEST_TIMEOUTS[lane_id]
            ):
                fail(f"{lane_id} GTest properties drifted: {name}")
            actual_targets[target].add(name)
            actual_target_artifacts[target].add(
                binary.relative_to(expected_build).as_posix()
            )
            _validate_native_gtest_environment(
                lane_id=lane_id,
                build_root=build_root,
                required_group=required_group,
                environment=_ctest_environment(
                    properties.get("ENVIRONMENT"),
                    f"{lane_id} native GTest {name}",
                ),
            )
            continue

        if name not in expected_ordinary:
            fail(f"{lane_id} has an unqualified ordinary CTest entry: {name}")
        expected_source = ordinary_sources[name][0]
        expected_command, expected_environment = _expected_native_ordinary(
            name, build_root, expected_source
        )
        if name.startswith("retained_compiled_"):
            if (
                set(properties)
                != {"ENVIRONMENT", "TIMEOUT", "WORKING_DIRECTORY"}
                or _native_timeout(properties, name) != 60.0
            ):
                fail(f"{lane_id} ordinary RoboPlan properties drifted: {name}")
        elif set(properties) != {"WORKING_DIRECTORY"}:
            fail(f"{lane_id} ordinary Deployment properties drifted: {name}")
        actual_environment = _ctest_environment(
            properties.get("ENVIRONMENT"), f"{lane_id} ordinary test {name}"
        )
        working_directory = properties.get(
            "WORKING_DIRECTORY", expected_build.as_posix()
        )
        if (
            command != expected_command
            or actual_environment != expected_environment
            or working_directory != expected_build.as_posix()
        ):
            fail(f"{lane_id} ordinary command/properties drifted: {name}")
        actual_ordinary[name] = command

    if set(actual_ordinary) != expected_ordinary:
        fail(
            f"{lane_id} ordinary native registry drifted: "
            f"expected={sorted(expected_ordinary)}, actual={sorted(actual_ordinary)}"
        )

    required_by_target: set[str] = set()
    target_evidence: list[dict[str, Any]] = []
    for target, (expected_count, expected_digest) in expected_targets.items():
        names = actual_targets[target]
        frozen_names = names.intersection(required_names)
        if (
            len(frozen_names) != expected_count
            or _name_digest(frozen_names) != expected_digest
        ):
            fail(
                f"{lane_id} frozen names moved to or from native target {target}"
            )
        additions = names - frozen_names
        artifacts = actual_target_artifacts[target]
        if len(artifacts) != 1:
            fail(f"{lane_id} native target has no unique registered artifact: {target}")
        if additions and not allow_additional_tests:
            fail(f"{lane_id} focused target {target} contains added tests")
        required_by_target.update(frozen_names)
        target_evidence.append(
            {
                "target": target,
                "artifact_relative": next(iter(artifacts)),
                "registered_test_count": len(names),
                "registered_test_names_sha256": _name_digest(names),
                "frozen_test_count": len(frozen_names),
                "frozen_test_names_sha256": _name_digest(frozen_names),
                "additional_test_count": len(additions),
                "additional_test_names_sha256": _name_digest(additions),
            }
        )

    if required_by_target | expected_ordinary != required_names:
        fail(f"{lane_id} native target mapping does not cover its frozen baseline")
    if not allow_additional_tests and seen != required_names:
        fail(f"{lane_id} exact native target registry contains added names")
    return {
        "build_root": build_root,
        "required_group": required_group,
        "targets": target_evidence,
        "ordinary_tests": [
            {
                "name": name,
                "source_path": ordinary_sources[name][0],
                "baseline_sha256": ordinary_sources[name][1],
                "candidate_sha256": ordinary_sources[name][2],
            }
            for name in sorted(expected_ordinary)
        ],
    }


def _container_artifact_relative(value: str, label: str) -> Path:
    if posixpath.normpath(value) != value:
        fail(f"{label} is not one canonical absolute path: {value}")
    path = Path(value)
    container_root = Path("/qualification-output")
    if not path.is_absolute() or not path.is_relative_to(container_root):
        fail(f"{label} is outside the exact container artifact root: {value}")
    relative = path.relative_to(container_root)
    return _safe_relative_path(relative.as_posix(), label)


def _native_timeout(
    properties: dict[str, Any], label: str, ceiling: float = 300.0
) -> float:
    raw = properties.get("TIMEOUT", ceiling)
    if isinstance(raw, bool):
        fail(f"{label} timeout is not numeric")
    try:
        timeout = float(raw)
    except (TypeError, ValueError):
        fail(f"{label} timeout is not numeric")
    if not 0 < timeout <= ceiling:
        fail(f"{label} timeout is outside the frozen ceiling")
    return timeout


def _derive_native_process_contract(
    inventory_path: Path,
    row: dict[str, Any],
) -> tuple[list[dict[str, Any]], set[str], list[str], str | None]:
    inventory = _read_json_object(
        inventory_path, f"{row['lane_id']} native CTest inventory contract"
    )
    tests = _require_list(
        inventory.get("tests"), f"{row['lane_id']} native CTest inventory tests"
    )
    groups: dict[tuple[str, str, tuple[tuple[str, str], ...]], dict[str, Any]] = {}
    ordinary: list[dict[str, Any]] = []
    registered: set[str] = set()
    exclusions = set(row["allowed_exclusions"])
    found_exclusions: set[str] = set()
    allowed_gtest_arguments = {"--gtest_also_run_disabled_tests"}
    test_regex = row["test_regex"]
    build_absolute = f"/qualification-output/{row['build_root']}"
    inventory_argv = [
        "/usr/bin/ctest",
        "--test-dir",
        build_absolute,
        "--show-only=json-v1",
    ]
    if test_regex is not None:
        inventory_argv.extend(["-R", test_regex])

    for index, raw in enumerate(tests):
        test = _require_object(raw, f"{row['lane_id']} CTest test {index}")
        name = _require_text(test.get("name"), f"{row['lane_id']} CTest name {index}")
        if name in registered:
            fail(f"{row['lane_id']} CTest registry duplicates {name}")
        registered.add(name)
        command = _require_list(
            test.get("command"), f"{row['lane_id']} CTest command {name}"
        )
        if not command or not all(isinstance(value, str) and value for value in command):
            fail(f"{row['lane_id']} CTest command is malformed: {name}")
        properties = _ctest_properties(test, f"{row['lane_id']} CTest test {name}")
        if _ctest_truthy(properties.get("DISABLED")):
            fail(f"{row['lane_id']} CTest registry disables {name}")
        for forbidden in (
            "PASS_REGULAR_EXPRESSION",
            "SKIP_RETURN_CODE",
            "ENVIRONMENT_MODIFICATION",
        ):
            if forbidden in properties:
                fail(
                    f"{row['lane_id']} CTest registry uses unsupported "
                    f"result/environment property {forbidden}: {name}"
                )
        if "WILL_FAIL" in properties and _ctest_truthy(properties["WILL_FAIL"]):
            fail(f"{row['lane_id']} CTest registry inverts failure: {name}")
        environment = _ctest_environment(
            properties.get("ENVIRONMENT"), f"{row['lane_id']} {name}"
        )
        excluded = name in exclusions
        if excluded:
            found_exclusions.add(name)

        filters = [
            value.removeprefix("--gtest_filter=")
            for value in command[1:]
            if value.startswith("--gtest_filter=")
        ]
        gtest_arguments = {
            value for value in command[1:] if value.startswith("--gtest_")
        }
        if not filters and not gtest_arguments:
            if "SKIP_REGULAR_EXPRESSION" in properties:
                fail(
                    f"{row['lane_id']} ordinary CTest entry permits an output skip: {name}"
                )
            if excluded:
                continue
            working_value = str(properties.get("WORKING_DIRECTORY", build_absolute))
            working_relative = _container_artifact_relative(
                working_value,
                f"{row['lane_id']} ordinary CTest working directory",
            )
            if not working_relative.is_relative_to(Path(row["build_root"])):
                fail(
                    f"{row['lane_id']} ordinary CTest working directory escapes its build"
                )
            ordinary.append(
                {
                    "name": name,
                    "command": command,
                    "cwd": working_relative.as_posix(),
                    "environment": environment,
                    "timeout_seconds": _native_timeout(
                        properties, name, float(row["timeout_ceiling"])
                    ),
                }
            )
            continue
        if not _ctest_skip_marker_is_exact(
            properties.get("SKIP_REGULAR_EXPRESSION")
        ):
            fail(
                f"{row['lane_id']} GTest CMake skip marker drifted: {name}"
            )
        if excluded:
            continue
        extras = set(command[1:]) - {f"--gtest_filter={value}" for value in filters}
        if (
            len(filters) != 1
            or not filters[0]
            or ":" in filters[0]
            or not extras.issubset(allowed_gtest_arguments)
        ):
            fail(f"{row['lane_id']} has an unsupported GTest registration: {name}")
        binary_relative = _container_artifact_relative(
            command[0], f"{row['lane_id']} registered GTest binary"
        )
        if not binary_relative.is_relative_to(Path(row["build_root"])):
            fail(f"{row['lane_id']} registered GTest binary escapes its build")
        working_value = str(properties.get("WORKING_DIRECTORY", Path(command[0]).parent))
        working_relative = _container_artifact_relative(
            working_value, f"{row['lane_id']} registered GTest working directory"
        )
        if not working_relative.is_relative_to(Path(row["build_root"])):
            fail(f"{row['lane_id']} registered GTest working directory escapes its build")
        key = (
            binary_relative.as_posix(),
            working_relative.as_posix(),
            tuple(sorted(environment.items())),
        )
        if key not in groups:
            groups[key] = {
                "binary_path": binary_relative.as_posix(),
                "cwd": working_relative.as_posix(),
                "registered_tests": [],
                "filters": [],
                "environment": environment,
                "timeout_seconds": _native_timeout(
                    properties, name, float(row["timeout_ceiling"])
                ),
            }
        group = groups[key]
        group["registered_tests"].append(name)
        group["filters"].append(filters[0])
        group["timeout_seconds"] = min(
            group["timeout_seconds"],
            _native_timeout(properties, name, float(row["timeout_ceiling"])),
        )
    if found_exclusions != exclusions:
        fail(f"{row['lane_id']} native exclusions are absent from the registry")

    native_prefix = Path(row["execution_index_output"]).parent / "native"
    expected: list[dict[str, Any]] = []
    ordinal = 0
    for group in groups.values():
        ordinal += 1
        result_relative = native_prefix / f"{ordinal:03d}-gtest.json"
        log_relative = native_prefix / f"{ordinal:03d}-gtest.log"
        binary_absolute = f"/qualification-output/{group['binary_path']}"
        expected.append(
            {
                "kind": "gtest_json",
                "cwd": group["cwd"],
                "argv": [
                    binary_absolute,
                    f"--gtest_filter={':'.join(group['filters'])}",
                    "--gtest_color=no",
                    f"--gtest_output=json:/qualification-output/{result_relative.as_posix()}",
                ],
                "timeout_seconds": group["timeout_seconds"],
                "environment": group["environment"],
                "result_path": result_relative.as_posix(),
                "log_path": log_relative.as_posix(),
                "binary_path": group["binary_path"],
                "registered_tests": group["registered_tests"],
            }
        )
    for test in ordinary:
        ordinal += 1
        result_relative = native_prefix / f"{ordinal:03d}-ordinary.json"
        log_relative = native_prefix / f"{ordinal:03d}-ordinary.log"
        expected.append(
            {
                "kind": "ordinary_direct",
                "cwd": test["cwd"],
                "argv": test["command"],
                "timeout_seconds": test["timeout_seconds"],
                "environment": test["environment"],
                "result_path": result_relative.as_posix(),
                "log_path": log_relative.as_posix(),
                "binary_path": None,
                "registered_tests": [test["name"]],
            }
        )
    return expected, registered, inventory_argv, test_regex


def _authenticate_provenance_files(
    evidence: dict[str, Any],
    *,
    lane_root: Path,
    repository_root: Path,
    label: str,
) -> list[dict[str, Any]]:
    """Rehash every local file named by one stopped-lane provenance graph."""

    authenticated: dict[str, str] = {}
    external: dict[str, str] = {}

    def authenticate(logical: Any, digest: Any, item_label: str) -> None:
        logical_path = _require_text(logical, f"{item_label} path")
        expected_digest = _require_text(digest, f"{item_label} digest")
        if (
            posixpath.normpath(logical_path) != logical_path
            or not logical_path.startswith("/")
            or not HEX64.fullmatch(expected_digest)
        ):
            fail(f"{item_label} path/digest is not canonical")
        if logical_path.startswith("/qualification-output/"):
            root = lane_root
            relative = Path(logical_path).relative_to("/qualification-output")
        elif logical_path.startswith("/workspace/"):
            root = repository_root
            relative = Path(logical_path).relative_to("/workspace")
        elif logical_path.startswith(
            ("/usr/", "/lib/", "/lib64/", "/usr/local/")
        ):
            previous = external.setdefault(logical_path, expected_digest)
            if previous != expected_digest:
                fail(f"{item_label} gives one external file conflicting digests")
            return
        else:
            fail(f"{item_label} references an unauthenticated filesystem root")
        file_path = _require_contained_regular_file(
            root / relative,
            root,
            item_label,
            require_single_link=True,
        )
        if _sha256_file(file_path) != expected_digest:
            fail(f"{item_label} changed after in-container provenance validation")
        previous = authenticated.setdefault(logical_path, expected_digest)
        if previous != expected_digest:
            fail(f"{item_label} gives one local file conflicting digests")

    def visit(value: Any, item_label: str) -> None:
        if isinstance(value, dict):
            if isinstance(value.get("sha256"), str):
                logical = value.get("resolved_path", value.get("path"))
                if logical is not None:
                    authenticate(logical, value["sha256"], item_label)
                elif value.get("producer_object") is not None:
                    authenticate(value["producer_object"], value["sha256"], item_label)
            if "output_sha256" in value:
                authenticate(value.get("output"), value["output_sha256"], item_label)
            if "candidate_sha256" in value and "source_path" in value:
                authenticate(value["source_path"], value["candidate_sha256"], item_label)
            for key, child in value.items():
                if key in {
                    "production_parity",
                    "test_only_parity",
                    "execution_parity",
                    "cross_profile_parity",
                    "deterministic_parity",
                }:
                    # These are deliberately alias-normalized semantic projections.
                    # Their physical inputs are rehashed through the surrounding raw
                    # target proofs; dedicated validators below check the projections.
                    continue
                visit(child, f"{item_label}.{key}")
        elif isinstance(value, list):
            for index, child in enumerate(value):
                visit(child, f"{item_label}[{index}]")

    visit(evidence, label)
    return [
        {"path": logical, "sha256": authenticated[logical]}
        for logical in sorted(authenticated)
    ]


def _validate_configure_fixture_inventory(
    value: Any,
    *,
    expected_root: str,
    expected_paths: list[str],
    lane_root: Path,
    label: str,
) -> list[dict[str, Any]]:
    inventory = _require_object(value, label)
    _exact_keys(
        inventory,
        {"root", "entry_count", "total_file_bytes", "entries_sha256", "entries"},
        label,
    )
    if inventory["root"] != expected_root:
        fail(f"{label} root drifted")
    entries = _require_list(inventory["entries"], f"{label}.entries")
    normalized: list[dict[str, Any]] = []
    total_bytes = 0
    for index, raw_entry in enumerate(entries):
        entry_label = f"{label}.entries[{index}]"
        entry = _require_object(raw_entry, entry_label)
        _exact_keys(
            entry,
            {"relative_path", "kind", "size", "mode", "sha256"},
            entry_label,
        )
        relative = _require_text(entry["relative_path"], f"{entry_label}.path")
        digest = _require_text(entry["sha256"], f"{entry_label}.sha256")
        size = entry["size"]
        if (
            entry["kind"] != "file"
            or not isinstance(size, int)
            or isinstance(size, bool)
            or size < 0
            or not re.fullmatch(r"0[0-7]{3}", _require_text(entry["mode"], f"{entry_label}.mode"))
            or not HEX64.fullmatch(digest)
        ):
            fail(f"{entry_label} shape drifted")
        logical = f"{expected_root}/{relative}"
        if not logical.startswith("/qualification-output/"):
            fail(f"{entry_label} escapes qualification output")
        physical = _require_contained_regular_file(
            lane_root / Path(logical).relative_to("/qualification-output"),
            lane_root,
            entry_label,
            require_single_link=True,
        )
        if physical.stat().st_size != size or _sha256_file(physical) != digest:
            fail(f"{entry_label} retained bytes drifted")
        total_bytes += size
        normalized.append(dict(entry))
    if (
        [entry["relative_path"] for entry in normalized] != expected_paths
        or normalized != sorted(
            normalized, key=lambda entry: (entry["relative_path"], entry["kind"])
        )
        or inventory["entry_count"] != len(normalized)
        or inventory["total_file_bytes"] != total_bytes
        or inventory["entries_sha256"] != _canonical_json_sha256(normalized)
    ):
        fail(f"{label} membership/digest drifted")
    return normalized


def _validated_frozen_tool_identity(
    value: Any,
    *,
    contract: Any,
    label: str,
    version_field: str | None = None,
    expected_version: str | None = None,
) -> dict[str, str]:
    """Join one producer tool row to the exact frozen toolchain identity."""

    tool = _require_object(value, label)
    expected_keys = {"path", "resolved_path", "sha256"}
    if version_field is not None:
        expected_keys.add(version_field)
    _exact_keys(tool, expected_keys, label)
    frozen = _require_object(contract, f"{label} frozen contract")
    identity = {
        "path": _canonical_absolute_path(tool.get("path"), f"{label}.path"),
        "resolved_path": _canonical_absolute_path(
            tool.get("resolved_path"), f"{label}.resolved_path"
        ),
        "sha256": _require_text(tool.get("sha256"), f"{label}.sha256"),
    }
    frozen_identity = {
        "path": _require_text(
            frozen.get("lexical_path"), f"{label} frozen lexical path"
        ),
        "resolved_path": _require_text(
            frozen.get("resolved_path"), f"{label} frozen resolved path"
        ),
        "sha256": _require_text(
            frozen.get("sha256"), f"{label} frozen digest"
        ),
    }
    if (
        identity != frozen_identity
        or identity["path"] not in PINNED_CONTAINER_BUILD_TOOLS
        or HEX64.fullmatch(identity["sha256"]) is None
    ):
        fail(f"{label} differs from the exact frozen build-tool identity")
    if version_field is not None:
        frozen_version = (
            expected_version
            if expected_version is not None
            else _require_text(
                frozen.get(version_field), f"{label} frozen {version_field}"
            )
        )
        if tool.get(version_field) != frozen_version:
            fail(f"{label} differs from the exact frozen build-tool version")
    return identity


def _validated_project_build_tool_identities(
    project: dict[str, Any],
    *,
    build_contract: dict[str, Any],
    label: str,
) -> list[dict[str, str]]:
    toolchain = _require_object(
        build_contract.get("toolchain_contract"), f"{label} toolchain contract"
    )
    identities: list[dict[str, str]] = []
    for evidence_field, contract_field, version_field, expected_version in (
        PROJECT_BUILD_TOOL_FIELDS
    ):
        identities.append(
            _validated_frozen_tool_identity(
                project.get(evidence_field),
                contract=toolchain.get(contract_field),
                label=f"{label}.{evidence_field}",
                version_field=version_field,
                expected_version=expected_version,
            )
        )
    return identities


def _validate_project_configure_fixtures(
    project: dict[str, Any],
    *,
    build_contract: dict[str, Any],
    lane_root: Path,
    repository_root: Path,
    label: str,
) -> None:
    project_id = project["project_id"]
    build_root = project["build_root"]
    contracted = sorted(
        (
            _require_object(raw, "configure fixture contract")
            for raw in _require_list(
                build_contract["configure_executed_fixture_contracts"],
                "configure fixture contracts",
            )
            if _require_object(raw, "configure fixture contract").get(
                "owner_project_id"
            )
            == project_id
        ),
        key=lambda row: row["output_root_relative_path"],
    )
    expected = contracted if project["build_testing"] else []
    evidence = _require_list(
        project["configure_executed_fixtures"], f"{label}.configure fixtures"
    )
    if len(evidence) != len(expected):
        fail(f"{label} configure fixture count drifted")
    python_contract = build_contract["toolchain_contract"]["python"]
    for index, (raw_row, contract) in enumerate(zip(evidence, expected, strict=True)):
        row_label = f"{label}.configure fixtures[{index}]"
        row = _require_object(raw_row, row_label)
        _exact_keys(
            row,
            {
                "kind",
                "owner_project_id",
                "output_root",
                "actual_tree_sha256",
                "replay_tree_sha256",
                "required_inputs_sha256",
                "argv_sha256",
                "cross_profile_argv_sha256",
                "cmake",
                "tool",
                "interpreter",
                "required_inputs",
                "actual_output_inventory",
                "replay_output_inventory",
                "replay_stdout_sha256",
            },
            row_label,
        )
        output_relative = contract["output_root_relative_path"]
        actual_root = f"{build_root}/{output_relative}"
        replay_root = f"{build_root}/i2-configure-fixture-replay/{output_relative}"
        output_paths = contract["exact_output_relative_paths"]
        actual_entries = _validate_configure_fixture_inventory(
            row["actual_output_inventory"],
            expected_root=actual_root,
            expected_paths=output_paths,
            lane_root=lane_root,
            label=f"{row_label}.actual inventory",
        )
        replay_entries = _validate_configure_fixture_inventory(
            row["replay_output_inventory"],
            expected_root=replay_root,
            expected_paths=output_paths,
            lane_root=lane_root,
            label=f"{row_label}.replay inventory",
        )
        expected_input_paths = sorted(
            f"/workspace/{path}"
            for path in (
                contract["cmake_repository_path"],
                contract["tool_repository_path"],
                contract["urdf_repository_path"],
                contract["srdf_repository_path"],
                *contract["resource_repository_paths"],
            )
        )
        required_inputs = _require_list(
            row["required_inputs"], f"{row_label}.required_inputs"
        )
        input_paths: list[str] = []
        for input_index, raw_input in enumerate(required_inputs):
            input_label = f"{row_label}.required_inputs[{input_index}]"
            item = _require_object(raw_input, input_label)
            _exact_keys(
                item,
                {
                    "path",
                    "sha256",
                    "is_generated",
                    "is_external",
                    "is_cmake",
                    "identity",
                },
                input_label,
            )
            path = _require_text(item["path"], f"{input_label}.path")
            identity = _require_object(item["identity"], f"{input_label}.identity")
            if (
                item["is_generated"] is not False
                or item["is_external"] is not False
                or item["is_cmake"] is not False
                or identity.get("kind") != "git_tracked_source"
                or not HEX64.fullmatch(
                    _require_text(item["sha256"], f"{input_label}.sha256")
                )
            ):
                fail(f"{input_label} source identity drifted")
            input_paths.append(path)
        interpreter = _require_object(row["interpreter"], f"{row_label}.interpreter")
        if (
            row["kind"] != "cmake_execute_process_replayed_fixture"
            or row["owner_project_id"] != project_id
            or row["output_root"] != actual_root
            or row["actual_tree_sha256"]
            != row["actual_output_inventory"]["entries_sha256"]
            or row["replay_tree_sha256"]
            != row["replay_output_inventory"]["entries_sha256"]
            or actual_entries != replay_entries
            or input_paths != expected_input_paths
            or row["required_inputs_sha256"]
            != _canonical_json_sha256(required_inputs)
            or row["cmake"] != f"/workspace/{contract['cmake_repository_path']}"
            or row["tool"] != f"/workspace/{contract['tool_repository_path']}"
            or interpreter != {
                "path": python_contract["lexical_path"],
                "resolved_path": python_contract["resolved_path"],
                "sha256": python_contract["sha256"],
            }
            or any(
                not HEX64.fullmatch(
                    _require_text(row[field], f"{row_label}.{field}")
                )
                for field in (
                    "argv_sha256",
                    "cross_profile_argv_sha256",
                    "replay_stdout_sha256",
                )
            )
        ):
            fail(f"{row_label} identity/replay join drifted")


def _validate_project_source_generations(
    value: Any,
    *,
    build_contract: dict[str, Any],
    lane_root: Path,
    repository_root: Path,
    expected_manyforge_generation: dict[str, Any],
    expected_projects: dict[str, tuple[str, str]],
    expected_configuration: str,
    expected_build_testing: dict[str, bool],
    expected_auxiliary_git_root: str | None,
    expected_auxiliary_capture_path: str | None,
    label: str,
) -> None:
    """Replay exact clean-repository and patched auxiliary source generations."""

    project_keys = {
        "project_id",
        "source_root",
        "source_git_generation",
        "build_root",
        "configuration",
        "compile_mode_contract",
        "build_testing",
        "build_testing_cache_key",
        "cmake_cache",
        "build_ninja",
        "rules_ninja",
        "file_api_query",
        "file_api_cmake_files_query",
        "file_api_index",
        "file_api_codemodel",
        "file_api_cmake_files",
        "configure_inputs",
        "configure_executed_fixtures",
        "configured_target_graph",
        "file_api_document_count",
        "file_api_documents",
        "compile_commands",
        "cmake",
        "git",
        "python",
        "ninja",
        "compiler",
        "archiver",
        "ranlib",
    }
    generation_keys = {
        "git_root",
        "base_commit",
        "base_tree",
        "patched_index_tree",
        "staged_diff_sha256",
        "staged_path_count",
        "staged_paths_sha256",
        "unstaged_path_count",
        "untracked_path_count",
        "allowed_untracked_paths",
        "undeclared_untracked_path_count",
        "unmerged_path_count",
        "submodule_path_count",
        "tracked_path_count",
        "tracked_index_projection_sha256",
        "prebuild_capture",
        "prebuild_matches_postbuild",
    }
    fact_keys = {
        "base_commit",
        "base_tree",
        "patched_index_tree",
        "staged_diff_sha256",
        "staged_path_count",
        "staged_paths_sha256",
        "unstaged_path_count",
        "untracked_path_count",
        "allowed_untracked_paths",
        "undeclared_untracked_path_count",
        "unmerged_path_count",
        "submodule_path_count",
        "tracked_path_count",
        "tracked_index_projection_sha256",
    }
    empty_sha256 = hashlib.sha256(b"").hexdigest()
    auxiliary = _require_object(
        build_contract["auxiliary_install_contract"],
        f"{label} auxiliary install contract",
    )
    expected_auxiliary = _require_object(
        auxiliary["source_generation"],
        f"{label} auxiliary source generation contract",
    )
    projects = _require_list(value, f"{label} projects")
    if not projects:
        fail(f"{label} contains no project provenance")
    seen_projects: set[str] = set()
    repository_generations: list[dict[str, Any]] = []
    auxiliary_projects: set[str] = set()
    auxiliary_capture: dict[str, Any] | None = None
    replayed_auxiliary: dict[str, Any] | None = None
    for index, raw_project in enumerate(projects):
        project_label = f"{label} project[{index}]"
        project = _require_object(raw_project, project_label)
        _exact_keys(project, project_keys, project_label)
        project_id = _require_text(project["project_id"], f"{project_label}.project_id")
        if project_id in seen_projects:
            fail(f"{label} repeats project provenance {project_id}")
        if project_id not in expected_projects:
            fail(f"{label} contains unexpected project provenance {project_id}")
        seen_projects.add(project_id)
        source_root = _require_text(
            project["source_root"], f"{project_label}.source_root"
        )
        build_root = _require_text(
            project["build_root"], f"{project_label}.build_root"
        )
        if (source_root, build_root) != expected_projects[project_id]:
            fail(f"{project_label} source/build route drifted")
        build_testing = project["build_testing"]
        if (
            project["configuration"] != expected_configuration
            or build_testing is not expected_build_testing[project_id]
            or project["compile_mode_contract"]
            != build_contract["toolchain_contract"]["compile_modes"]
            or project["build_testing_cache_key"] != "BUILD_TESTING"
        ):
            fail(f"{project_label} configuration/testing contract drifted")
        _validated_project_build_tool_identities(
            project,
            build_contract=build_contract,
            label=project_label,
        )
        if not build_root.startswith("/qualification-output/"):
            fail(f"{project_label} build root is outside qualification output")
        physical_build_root = lane_root / Path(build_root).relative_to(
            "/qualification-output"
        )
        try:
            replayed_graph = replay_configured_target_graph(
                repository_root,
                lane_root,
                project_id=project_id,
                build_root=physical_build_root,
                build_testing=build_testing,
                build_contract=build_contract,
            )
        except NativeBuildProvenanceError as error:
            fail(f"{project_label} configured-target graph replay failed: {error}")
        if replayed_graph != project["configured_target_graph"]:
            fail(f"{project_label} configured-target graph evidence drifted")
        _validate_project_configure_fixtures(
            project,
            build_contract=build_contract,
            lane_root=lane_root,
            repository_root=repository_root,
            label=project_label,
        )
        generation = _require_object(
            project["source_git_generation"],
            f"{project_label}.source_git_generation",
        )
        _exact_keys(
            generation,
            generation_keys,
            f"{project_label}.source_git_generation",
        )
        git_root = _require_text(
            generation["git_root"], f"{project_label}.source Git root"
        )
        if (
            posixpath.normpath(source_root) != source_root
            or posixpath.normpath(git_root) != git_root
            or not source_root.startswith(f"{git_root}/")
        ):
            fail(f"{project_label} source/Git roots are not canonical and nested")
        facts = {key: generation[key] for key in fact_keys}
        if (
            HEX40.fullmatch(
                _require_text(facts["base_commit"], f"{project_label}.base_commit")
            )
            is None
            or any(
                HEX40.fullmatch(
                    _require_text(facts[key], f"{project_label}.{key}")
                )
                is None
                for key in ("base_tree", "patched_index_tree")
            )
            or any(
                HEX64.fullmatch(
                    _require_text(facts[key], f"{project_label}.{key}")
                )
                is None
                for key in ("staged_diff_sha256", "staged_paths_sha256")
            )
            or HEX64.fullmatch(
                _require_text(
                    facts["tracked_index_projection_sha256"],
                    f"{project_label}.tracked_index_projection_sha256",
                )
            )
            is None
            or any(
                not isinstance(facts[key], int)
                or isinstance(facts[key], bool)
                or facts[key] < 0
                for key in (
                    "staged_path_count",
                    "unstaged_path_count",
                    "untracked_path_count",
                    "undeclared_untracked_path_count",
                    "unmerged_path_count",
                    "submodule_path_count",
                    "tracked_path_count",
                )
            )
            or any(
                facts[key] != 0
                for key in (
                    "unstaged_path_count",
                    "undeclared_untracked_path_count",
                    "unmerged_path_count",
                    "submodule_path_count",
                )
            )
            or generation["prebuild_matches_postbuild"] is not True
            or not isinstance(facts["allowed_untracked_paths"], list)
            or any(
                not isinstance(path, str) or not path
                for path in facts["allowed_untracked_paths"]
            )
            or facts["untracked_path_count"]
            != len(facts["allowed_untracked_paths"])
            or facts["tracked_path_count"] < 1
        ):
            fail(f"{project_label} source-generation fact shape drifted")

        capture_value = generation["prebuild_capture"]
        if project_id in {"roboplan", "roboplan_example_models"}:
            auxiliary_projects.add(project_id)
            expected_subset = {
                key: facts[key] for key in expected_auxiliary
            }
            if (
                expected_subset != expected_auxiliary
                or source_root != f"{git_root}/{project_id}"
                or facts["allowed_untracked_paths"] != []
                or facts["untracked_path_count"] != 0
                or expected_auxiliary_git_root is None
                or git_root != expected_auxiliary_git_root
            ):
                fail(f"{project_label} patched auxiliary generation drifted")
            capture = _require_object(
                capture_value, f"{project_label}.prebuild_capture"
            )
            _exact_keys(
                capture,
                {"path", "sha256"},
                f"{project_label}.prebuild_capture",
            )
            capture_path = _require_text(
                capture["path"], f"{project_label}.prebuild_capture.path"
            )
            capture_digest = _require_text(
                capture["sha256"], f"{project_label}.prebuild_capture.sha256"
            )
            if (
                posixpath.normpath(capture_path) != capture_path
                or not capture_path.startswith("/qualification-output/")
                or HEX64.fullmatch(capture_digest) is None
                or capture_path != expected_auxiliary_capture_path
            ):
                fail(f"{project_label} prebuild capture path/digest drifted")
            physical_capture = _require_contained_regular_file(
                lane_root
                / Path(capture_path).relative_to("/qualification-output"),
                lane_root,
                f"{project_label} prebuild capture",
                require_single_link=True,
            )
            if _sha256_file(physical_capture) != capture_digest:
                fail(f"{project_label} prebuild capture changed after validation")
            decoded_capture = _read_json_object(
                physical_capture, f"{project_label} prebuild capture"
            )
            expected_capture = {
                "schema_version": (
                    "manyforge.composable_cell_i2.patched_source_generation.v1"
                ),
                **facts,
            }
            if decoded_capture != expected_capture:
                fail(f"{project_label} prebuild/postbuild source facts disagree")
            if replayed_auxiliary is None:
                physical_git_root = lane_root / Path(git_root).relative_to(
                    "/qualification-output"
                )
                try:
                    replayed_auxiliary = replay_git_source_generation(
                        physical_git_root, []
                    )
                except NativeBuildProvenanceError as error:
                    fail(f"{project_label} retained auxiliary source replay failed: {error}")
            if replayed_auxiliary != facts:
                fail(f"{project_label} retained auxiliary source facts drifted")
            if auxiliary_capture is None:
                auxiliary_capture = {"identity": capture, "facts": facts}
            elif auxiliary_capture != {"identity": capture, "facts": facts}:
                fail(f"{label} auxiliary projects do not share one source capture")
        else:
            if (
                git_root != "/workspace"
                or capture_value is not None
                or facts["allowed_untracked_paths"] != [".docker/roboplan/"]
                or facts["untracked_path_count"] != 1
                or facts["base_tree"] != facts["patched_index_tree"]
                or facts["staged_path_count"] != 0
                or facts["staged_diff_sha256"] != empty_sha256
                or facts["staged_paths_sha256"] != empty_sha256
            ):
                fail(f"{project_label} is not one clean committed repository generation")
            if facts != expected_manyforge_generation:
                fail(f"{project_label} does not use the captured candidate generation")
            repository_generations.append(facts)
    if seen_projects != set(expected_projects):
        fail(
            f"{label} project set drifted: expected={sorted(expected_projects)}, "
            f"actual={sorted(seen_projects)}"
        )
    expected_auxiliary_projects = set(expected_projects).intersection(
        {"roboplan", "roboplan_example_models"}
    )
    has_auxiliary_route = (
        expected_auxiliary_git_root is not None
        and expected_auxiliary_capture_path is not None
    )
    has_partial_auxiliary_route = (
        (expected_auxiliary_git_root is None)
        != (expected_auxiliary_capture_path is None)
    )
    if (
        auxiliary_projects != expected_auxiliary_projects
        or has_partial_auxiliary_route
        or bool(expected_auxiliary_projects) != has_auxiliary_route
    ):
        fail(f"{label} auxiliary source route drifted")
    if repository_generations and any(
        facts != repository_generations[0]
        for facts in repository_generations[1:]
    ):
        fail(f"{label} repository projects use different source generations")


def _validate_native_build_provenance_artifact(
    path: Path,
    *,
    lane_root: Path,
    repository_root: Path,
    row: dict[str, Any],
    target_mapping: dict[str, Any],
    build_contract: dict[str, Any],
    expected_manyforge_generation: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, str], list[dict[str, Any]]]:
    """Authenticate the stopped-lane provenance and every bound local file."""

    evidence = _read_json_object(path, f"{row['lane_id']} native provenance")
    _exact_keys(
        evidence,
        {
            "schema_version",
            "lane_id",
            "required_group",
            "sanitizer",
            "build_root",
            "provenance_output",
            "projects",
            "targets",
            "production_targets",
            "test_only_targets",
            "utility_target_relationships",
            "production_parity",
            "test_only_parity",
            "execution_parity",
            "cross_profile_parity",
            "ordinary_tests",
            "installed_copies",
            "qualified_install_inputs",
            "install_projects",
            "response_files",
            "external_dependencies",
            "inspection_tools",
            "passed",
            "evidence_sha256",
        },
        f"{row['lane_id']} native provenance",
    )
    evidence_digest = _require_text(
        evidence["evidence_sha256"],
        f"{row['lane_id']} native provenance evidence digest",
    )
    digest_input = dict(evidence)
    del digest_input["evidence_sha256"]
    if (
        evidence["schema_version"]
        != "manyforge.composable_cell_i2.native_build_provenance.v1"
        or evidence["lane_id"] != row["lane_id"]
        or evidence["required_group"] != row["required_group"]
        or evidence["sanitizer"] != row["sanitizer"]
        or evidence["build_root"]
        != f"/qualification-output/{row['build_root']}"
        or evidence["provenance_output"]
        != f"/qualification-output/{row['provenance_output']}"
        or evidence["passed"] is not True
        or not HEX64.fullmatch(evidence_digest)
        or _canonical_json_sha256(digest_input) != evidence_digest
    ):
        fail(f"{row['lane_id']} native provenance identity drifted")

    _validate_utility_target_relationships(
        evidence["utility_target_relationships"],
        [
            *_require_list(
                evidence["production_targets"],
                f"{row['lane_id']} native production targets",
            ),
            *_require_list(
                evidence["test_only_targets"],
                f"{row['lane_id']} native test-only targets",
            ),
        ],
        f"{row['lane_id']} native utility targets",
        lane_root=lane_root,
    )
    package_by_group = {
        "core_release_baseline": "manyforge_core",
        "deployment_release_baseline": "manyforge_deployment",
        "c02_tail_required": "manyforge_deployment",
        "roboplan_release_baseline": "manyforge_roboplan",
        "roboplan_consumer_required": "manyforge_roboplan",
        "roboplan_scene_sanitizer_required": "manyforge_roboplan",
    }
    package = package_by_group[row["required_group"]]
    expected_projects = {
        package: (
            f"/workspace/{package}",
            f"/qualification-output/{row['build_root']}",
        )
    }
    auxiliary_git_root: str | None = None
    auxiliary_capture_path: str | None = None
    expected_install_roots: dict[str, str] = {}
    if row["required_group"] == "roboplan_scene_sanitizer_required":
        sanitizer_root = PurePosixPath(row["build_root"]).parents[1].as_posix()
        auxiliary_git_root = f"/qualification-output/{sanitizer_root}/source"
        auxiliary_capture_path = (
            f"/qualification-output/{sanitizer_root}/"
            "patched-source-generation-pre.json"
        )
        for project_id in ("roboplan", "roboplan_example_models"):
            expected_projects[project_id] = (
                f"{auxiliary_git_root}/{project_id}",
                f"/qualification-output/{sanitizer_root}/build/{project_id}",
            )
            expected_install_roots[project_id] = (
                f"/qualification-output/{sanitizer_root}/install/{project_id}"
            )
    _validate_project_source_generations(
        evidence["projects"],
        build_contract=build_contract,
        lane_root=lane_root,
        repository_root=repository_root,
        expected_manyforge_generation=expected_manyforge_generation,
        expected_projects=expected_projects,
        expected_configuration=(
            "Release" if row["sanitizer"] == "none" else "RelWithDebInfo"
        ),
        expected_build_testing={
            project_id: project_id not in {"roboplan", "roboplan_example_models"}
            for project_id in expected_projects
        },
        expected_auxiliary_git_root=auxiliary_git_root,
        expected_auxiliary_capture_path=auxiliary_capture_path,
        label=f"{row['lane_id']} native provenance",
    )
    _validate_auxiliary_install_projects(
        evidence["install_projects"],
        qualified_inputs_value=evidence["qualified_install_inputs"],
        installed_copies_value=evidence["installed_copies"],
        target_proofs_value=evidence["targets"],
        lane_root=lane_root,
        build_contract=build_contract,
        expected_install_roots=expected_install_roots,
        label=f"{row['lane_id']} native auxiliary installs",
    )
    production_specs, cross_profile_specs = _native_projection_specs(
        build_contract, row=row, project_id=package
    )
    test_only_specs = _native_test_only_projection_specs(
        build_contract, row=row, project_id=package
    )
    execution_specs, helper_by_test = _native_execution_projection_specs(
        build_contract,
        row=row,
        project_id=package,
        target_mapping=target_mapping,
        scene_specs=cross_profile_specs,
    )
    build_roots = {
        project_id: build_root
        for project_id, (_source_root, build_root) in expected_projects.items()
    }
    source_roots = {
        project_id: source_root
        for project_id, (source_root, _build_root) in expected_projects.items()
    }
    configure_generated_specs = _configure_generated_dependency_specs(
        build_contract, label=f"{row['lane_id']} native parity"
    )
    qualified_install_owners = _qualified_install_owner_index(
        evidence["qualified_install_inputs"],
        label=f"{row['lane_id']} qualified install owners",
    )
    project_provenance = {
        _require_text(
            _require_object(raw, f"{row['lane_id']} project provenance").get(
                "project_id"
            ),
            f"{row['lane_id']} project provenance project_id",
        ): _require_object(raw, f"{row['lane_id']} project provenance")
        for raw in _require_list(
            evidence["projects"], f"{row['lane_id']} project provenance"
        )
    }
    native_target_output_owners = [
        *_require_list(
            evidence["production_targets"],
            f"{row['lane_id']} native production command owners",
        ),
        *_require_list(
            evidence["test_only_targets"],
            f"{row['lane_id']} native test-only command owners",
        ),
        *_require_list(
            evidence["targets"], f"{row['lane_id']} native command owners"
        ),
    ]
    for family_name, family_value, family_roles in (
        (
            "production",
            evidence["production_targets"],
            frozenset({"production"}),
        ),
        (
            "test-only",
            evidence["test_only_targets"],
            frozenset({"test_support"}),
        ),
        (
            "execution/auxiliary",
            evidence["targets"],
            frozenset({"gtest", "ordinary_helper", "local_library"}),
        ),
    ):
        _validate_target_command_file_evidence(
            family_value,
            project_build_roots=build_roots,
            target_output_owners=native_target_output_owners,
            installed_output_owners=evidence["installed_copies"],
            expected_roles=family_roles,
            sanitizer_profile=row["sanitizer"],
            lane_root=lane_root,
            repository_root=repository_root,
            label=(
                f"{row['lane_id']} native {family_name} target command evidence"
            ),
        )
    _validate_target_proofs_against_projection(
        evidence["production_targets"],
        evidence["production_parity"],
        expected=production_specs,
        build_roots=build_roots,
        source_roots=source_roots,
        install_roots=expected_install_roots,
        configure_generated_specs=configure_generated_specs,
        qualified_install_owners=qualified_install_owners,
        project_provenance=project_provenance,
        repository_root=repository_root,
        lane_root=lane_root,
        allow_unexpected_proofs=False,
        label=f"{row['lane_id']} production parity",
    )
    _validate_target_proofs_against_projection(
        evidence["test_only_targets"],
        evidence["test_only_parity"],
        expected=test_only_specs,
        build_roots=build_roots,
        source_roots=source_roots,
        install_roots=expected_install_roots,
        configure_generated_specs=configure_generated_specs,
        qualified_install_owners=qualified_install_owners,
        project_provenance=project_provenance,
        repository_root=repository_root,
        lane_root=lane_root,
        allow_unexpected_proofs=False,
        label=f"{row['lane_id']} test-only parity",
    )
    _validate_target_proofs_against_projection(
        evidence["targets"],
        evidence["execution_parity"],
        expected=execution_specs,
        build_roots=build_roots,
        source_roots=source_roots,
        install_roots=expected_install_roots,
        configure_generated_specs=configure_generated_specs,
        qualified_install_owners=qualified_install_owners,
        project_provenance=project_provenance,
        repository_root=repository_root,
        lane_root=lane_root,
        allow_unexpected_proofs=False,
        label=f"{row['lane_id']} execution parity",
    )
    _validate_native_execution_annotations(
        proofs_value=evidence["targets"],
        ordinary_value=evidence["ordinary_tests"],
        target_mapping=target_mapping,
        helper_by_test=helper_by_test,
        label=f"{row['lane_id']} native execution annotations",
    )
    _validate_target_proofs_against_projection(
        (
            evidence["targets"]
            if row["required_group"] == "roboplan_scene_sanitizer_required"
            else evidence["production_targets"]
        ),
        evidence["cross_profile_parity"],
        expected=cross_profile_specs,
        build_roots=build_roots,
        source_roots=source_roots,
        install_roots=expected_install_roots,
        configure_generated_specs=configure_generated_specs,
        qualified_install_owners=qualified_install_owners,
        project_provenance=project_provenance,
        repository_root=repository_root,
        lane_root=lane_root,
        allow_unexpected_proofs=False,
        label=f"{row['lane_id']} cross-profile parity",
    )

    file_rows = _authenticate_provenance_files(
        evidence,
        lane_root=lane_root,
        repository_root=repository_root,
        label=f"{row['lane_id']} native provenance",
    )
    authenticated = {row["path"]: row["sha256"] for row in file_rows}
    targets = _require_list(
        evidence["targets"], f"{row['lane_id']} native provenance targets"
    )
    gtest_artifacts: dict[str, str] = {}
    for index, raw_target in enumerate(targets):
        target = _require_object(
            raw_target, f"{row['lane_id']} native provenance target {index}"
        )
        if target.get("role") != "gtest":
            continue
        artifact = _require_object(
            target.get("artifact"),
            f"{row['lane_id']} native provenance GTest artifact",
        )
        logical = _require_text(
            artifact.get("path"),
            f"{row['lane_id']} native provenance GTest artifact path",
        )
        digest = _require_text(
            artifact.get("sha256"),
            f"{row['lane_id']} native provenance GTest artifact digest",
        )
        if not logical.startswith("/qualification-output/"):
            fail(f"{row['lane_id']} proven GTest artifact escapes its lane")
        relative = Path(logical).relative_to("/qualification-output").as_posix()
        if relative in gtest_artifacts or authenticated.get(logical) != digest:
            fail(f"{row['lane_id']} proven GTest artifact is duplicated or unauthenticated")
        gtest_artifacts[relative] = digest
    if not gtest_artifacts:
        fail(f"{row['lane_id']} native provenance proves no GTest artifacts")
    return evidence, gtest_artifacts, file_rows


def _comparison_target_owners(value: Any, label: str) -> list[tuple[str, str]]:
    rows = _require_list(value, label)
    owners: list[tuple[str, str]] = []
    for index, raw in enumerate(rows):
        row_label = f"{label}[{index}]"
        row = _require_object(raw, row_label)
        _exact_keys(row, {"project_id", "target"}, row_label)
        owners.append(
            (
                _require_text(row["project_id"], f"{row_label}.project_id"),
                _require_text(row["target"], f"{row_label}.target"),
            )
        )
    if not owners or owners != sorted(set(owners)):
        fail(f"{label} is empty, duplicated, or unsorted")
    return owners


def _projection_target_owners(value: Any, label: str) -> list[tuple[str, str]]:
    rows = _require_list(value, label)
    owners: list[tuple[str, str]] = []
    for index, raw in enumerate(rows):
        row = _require_object(raw, f"{label}[{index}]")
        owners.append(
            (
                _require_text(row.get("project_id"), f"{label}[{index}].project_id"),
                _require_text(row.get("target"), f"{label}[{index}].target"),
            )
        )
    if owners != sorted(set(owners)):
        fail(f"{label} owner set is duplicated or unsorted")
    return owners


def _product_projection_specs(
    build_contract: dict[str, Any],
    *,
    project_id: str,
    target_names: set[str],
    catalogs: tuple[str, ...],
    label: str,
) -> dict[tuple[str, str], dict[str, Any]]:
    project = _require_object(
        _require_object(build_contract["projects"], "build projects").get(project_id),
        f"{label} project {project_id}",
    )
    available: dict[str, dict[str, Any]] = {}
    for catalog in catalogs:
        for index, raw_spec in enumerate(
            _require_list(project.get(catalog), f"{label} {project_id}.{catalog}")
        ):
            spec = _require_object(
                raw_spec, f"{label} {project_id}.{catalog}[{index}]"
            )
            name = _require_text(spec.get("name"), f"{label} target name")
            if name in available:
                fail(f"{label} repeats target contract {project_id}:{name}")
            available[name] = spec
    if not target_names.issubset(available):
        fail(
            f"{label} references unknown targets for {project_id}: "
            f"{sorted(target_names - set(available))}"
        )
    result: dict[tuple[str, str], dict[str, Any]] = {}
    for target_name in sorted(target_names):
        spec = available[target_name]
        compiled_sources = [
            f"/workspace/{_require_text(path, f'{label} required source')}"
            for path in _require_list(
                spec.get("required_sources", []),
                f"{label} {project_id}:{target_name} required sources",
            )
        ]
        dependency_only_sources = [
            f"<build:{project_id}>/"
            f"{_require_text(path, f'{label} generated source')}"
            for path in _require_list(
                spec.get("required_generated_sources", []),
                f"{label} {project_id}:{target_name} generated sources",
            )
        ]
        direct_sources = sorted([*compiled_sources, *dependency_only_sources])
        result[(project_id, target_name)] = {
            "target_type": _require_text(
                spec.get("type"), f"{label} {project_id}:{target_name} type"
            ),
            "artifact_relative": _require_text(
                spec.get("artifact"),
                f"{label} {project_id}:{target_name} artifact",
            ),
            "direct_source_paths": direct_sources,
            "compiled_source_paths": sorted(compiled_sources),
            "dependency_only_source_paths": sorted(dependency_only_sources),
        }
    return result


def _auxiliary_projection_specs(
    build_contract: dict[str, Any], *, label: str
) -> dict[tuple[str, str], dict[str, Any]]:
    auxiliary = _require_object(
        build_contract["auxiliary_install_contract"],
        f"{label} auxiliary install contract",
    )
    projects = _require_object(
        auxiliary.get("projects"), f"{label} auxiliary projects"
    )
    result: dict[tuple[str, str], dict[str, Any]] = {}
    for project_id in sorted(projects):
        project = _require_object(projects[project_id], f"{label} {project_id}")
        for index, raw_library in enumerate(
            _require_list(project.get("library_files"), f"{label} {project_id} libraries")
        ):
            library_label = f"{label} {project_id} library[{index}]"
            library = _require_object(raw_library, library_label)
            target = _require_text(library.get("target"), f"{library_label}.target")
            owner = (project_id, target)
            if owner in result:
                fail(f"{label} repeats auxiliary target {project_id}:{target}")
            direct_source_paths = [
                f"<source:{project_id}>/"
                f"{_require_text(path, f'{library_label}.required source')}"
                for path in _require_list(
                    library.get("required_sources"),
                    f"{library_label}.required sources",
                )
            ]
            result[owner] = {
                "target_type": _require_text(
                    library.get("type"), f"{library_label}.type"
                ),
                "artifact_relative": _require_text(
                    library.get("build_artifact_relative_path"),
                    f"{library_label}.artifact",
                ),
                "direct_source_paths": direct_source_paths,
                "compiled_source_paths": direct_source_paths,
                "dependency_only_source_paths": [],
            }
    return result


def _native_projection_specs(
    build_contract: dict[str, Any],
    *,
    row: dict[str, Any],
    project_id: str,
) -> tuple[
    dict[tuple[str, str], dict[str, Any]],
    dict[tuple[str, str], dict[str, Any]],
]:
    label = f"{row['lane_id']} native projection contract"
    partitions = _require_object(
        _require_object(
            build_contract["native_lane_target_partitions"],
            f"{label} partitions",
        ).get("lanes"),
        f"{label} lanes",
    )
    partition = _require_object(partitions.get(row["lane_id"]), f"{label} lane")
    production_names = set(
        _unique_text_set(
            partition.get("production_targets"), f"{label} production targets"
        )
    )
    production = _product_projection_specs(
        build_contract,
        project_id=project_id,
        target_names=production_names,
        catalogs=("targets",),
        label=label,
    )
    if row["required_group"] != "roboplan_scene_sanitizer_required":
        return production, dict(production)

    comparisons = [
        _require_object(raw, f"{label} comparison")
        for raw in _require_list(
            _require_object(
                build_contract["cross_profile_parity_contract"],
                f"{label} parity contract",
            ).get("comparisons"),
            f"{label} comparisons",
        )
        if _require_object(raw, f"{label} comparison").get("kind")
        == "native_cross_profile"
        and row["lane_id"]
        in _require_list(
            _require_object(raw, f"{label} comparison").get("lane_ids"),
            f"{label} comparison lanes",
        )
    ]
    if len(comparisons) != 1:
        fail(f"{label} scene comparison is absent or ambiguous")
    owners = _comparison_target_owners(
        comparisons[0].get("target_owners"), f"{label} scene owners"
    )
    primary_names = {target for owner_project, target in owners if owner_project == project_id}
    scene = _product_projection_specs(
        build_contract,
        project_id=project_id,
        target_names=primary_names,
        catalogs=("targets", "test_only_targets"),
        label=label,
    )
    auxiliaries = _auxiliary_projection_specs(build_contract, label=label)
    for owner in owners:
        if owner in scene:
            continue
        if owner not in auxiliaries:
            fail(f"{label} has no auxiliary target contract for {owner}")
        scene[owner] = auxiliaries[owner]
    if set(scene) != set(owners):
        fail(f"{label} scene target contract membership drifted")
    return production, scene


def _native_test_only_projection_specs(
    build_contract: dict[str, Any],
    *,
    row: dict[str, Any],
    project_id: str,
) -> dict[tuple[str, str], dict[str, Any]]:
    label = f"{row['lane_id']} native test-only projection contract"
    partitions = _require_object(
        _require_object(
            build_contract["native_lane_target_partitions"],
            f"{label} partitions",
        ).get("lanes"),
        f"{label} lanes",
    )
    partition = _require_object(partitions.get(row["lane_id"]), f"{label} lane")
    names = set(
        _unique_text_set(
            partition.get("test_only_targets"), f"{label} targets"
        )
    )
    return _product_projection_specs(
        build_contract,
        project_id=project_id,
        target_names=names,
        catalogs=("test_only_targets",),
        label=label,
    )


def _native_execution_projection_specs(
    build_contract: dict[str, Any],
    *,
    row: dict[str, Any],
    project_id: str,
    target_mapping: dict[str, Any],
    scene_specs: dict[tuple[str, str], dict[str, Any]],
) -> tuple[
    dict[tuple[str, str], dict[str, Any]],
    dict[str, dict[str, str] | None],
]:
    """Derive execution owners from frozen CTest/native and product contracts."""

    label = f"{row['lane_id']} native execution projection contract"
    expected: dict[tuple[str, str], dict[str, Any]] = {}
    for index, raw_target in enumerate(
        _require_list(target_mapping.get("targets"), f"{label} GTest targets")
    ):
        target_label = f"{label} GTest target[{index}]"
        target = _require_object(raw_target, target_label)
        name = _require_text(target.get("target"), f"{target_label}.target")
        artifact_relative = _require_text(
            target.get("artifact_relative"), f"{target_label}.artifact_relative"
        )
        owner = (project_id, name)
        derived = {
            "target_type": "EXECUTABLE",
            "artifact_relative": artifact_relative,
            "direct_source_paths": [f"/workspace/{project_id}/test/{name}.cpp"],
            "compiled_source_paths": [f"/workspace/{project_id}/test/{name}.cpp"],
            "dependency_only_source_paths": [],
        }
        explicit = _product_projection_specs(
            build_contract,
            project_id=project_id,
            target_names={name},
            catalogs=("test_only_targets",),
            label=f"{target_label} optional explicit contract",
        ) if any(
            _require_object(raw, f"{target_label} explicit candidate").get("name")
            == name
            for raw in _require_list(
                _require_object(
                    _require_object(
                        build_contract["projects"], f"{label} projects"
                    ).get(project_id),
                    f"{label} project",
                ).get("test_only_targets"),
                f"{label} explicit test targets",
            )
        ) else {}
        if explicit and explicit.get(owner) != derived:
            fail(f"{target_label} differs from its explicit build contract")
        if owner in expected:
            fail(f"{label} repeats GTest owner {owner}")
        expected[owner] = derived

    project = _require_object(
        _require_object(build_contract["projects"], f"{label} projects").get(
            project_id
        ),
        f"{label} project",
    )
    production_specs = {
        _require_text(
            _require_object(raw, f"{label} production target").get("name"),
            f"{label} production target name",
        ): _require_object(raw, f"{label} production target")
        for raw in _require_list(
            project.get("targets"), f"{label} production targets"
        )
    }
    helper_by_test: dict[str, dict[str, str] | None] = {}
    helper_names: set[str] = set()
    for index, raw_ordinary in enumerate(
        _require_list(
            target_mapping.get("ordinary_tests"), f"{label} ordinary tests"
        )
    ):
        ordinary_label = f"{label} ordinary test[{index}]"
        ordinary = _require_object(raw_ordinary, ordinary_label)
        test_name = _require_text(
            ordinary.get("name"), f"{ordinary_label}.name"
        )
        source_path = _require_text(
            ordinary.get("source_path"), f"{ordinary_label}.source_path"
        )
        command, _environment = _expected_native_ordinary(
            test_name, row["build_root"], source_path
        )
        matching_helpers: list[str] = []
        for target_name, spec in production_specs.items():
            if spec.get("type") != "EXECUTABLE":
                continue
            artifact = _require_text(
                spec.get("artifact"), f"{ordinary_label} candidate artifact"
            )
            expected_path = (
                f"/qualification-output/{row['build_root']}/{artifact}"
            )
            if expected_path in command or any(
                item.endswith(f"={expected_path}") for item in command
            ):
                matching_helpers.append(target_name)
        if len(matching_helpers) > 1:
            fail(f"{ordinary_label} names ambiguous production helpers")
        if not matching_helpers:
            helper_by_test[test_name] = None
            continue
        helper_name = matching_helpers[0]
        helper_names.add(helper_name)
        spec = _product_projection_specs(
            build_contract,
            project_id=project_id,
            target_names={helper_name},
            catalogs=("targets",),
            label=f"{ordinary_label} helper contract",
        )[(project_id, helper_name)]
        if spec["target_type"] != "EXECUTABLE":
            fail(f"{ordinary_label} helper is not an executable target")
        owner = (project_id, helper_name)
        if owner in expected:
            fail(f"{ordinary_label} helper owner is duplicated")
        expected[owner] = spec
        helper_by_test[test_name] = {
            "project_id": project_id,
            "target": helper_name,
            "artifact": (
                f"/qualification-output/{row['build_root']}/"
                f"{spec['artifact_relative']}"
            ),
        }

    if row["required_group"] == "roboplan_scene_sanitizer_required":
        if expected != {
            owner: spec for owner, spec in scene_specs.items() if owner[0] == project_id
        }:
            fail(f"{label} primary scene owner contract drifted")
        expected = dict(scene_specs)
    return expected, helper_by_test


def _validate_native_execution_annotations(
    *,
    proofs_value: Any,
    ordinary_value: Any,
    target_mapping: dict[str, Any],
    helper_by_test: dict[str, dict[str, str] | None],
    label: str,
) -> None:
    proofs = {
        (
            _require_text(
                _require_object(raw, f"{label} proof").get("project_id"),
                f"{label} proof project",
            ),
            _require_text(
                _require_object(raw, f"{label} proof").get("target"),
                f"{label} proof target",
            ),
        ): _require_object(raw, f"{label} proof")
        for raw in _require_list(proofs_value, f"{label} proofs")
    }
    gtest_rows = _require_list(
        target_mapping.get("targets"), f"{label} target mapping"
    )
    for index, raw_target in enumerate(gtest_rows):
        target_label = f"{label} GTest[{index}]"
        target = _require_object(raw_target, target_label)
        target_name = _require_text(
            target.get("target"), f"{target_label}.target"
        )
        matches = [
            proof
            for owner, proof in proofs.items()
            if owner[1] == target_name and proof.get("role") == "gtest"
        ]
        if len(matches) != 1:
            fail(f"{target_label} has no unique GTest proof")
        proof = matches[0]
        if (
            proof.get("registered_test_count")
            != target.get("registered_test_count")
            or proof.get("registered_test_names_sha256")
            != target.get("registered_test_names_sha256")
            or proof.get("contracted_test_count")
            != target.get("frozen_test_count")
            or proof.get("contracted_test_names_sha256")
            != target.get("frozen_test_names_sha256")
            or proof.get("additional_test_count")
            != target.get("additional_test_count")
        ):
            fail(f"{target_label} registration/proof join drifted")

    ordinary_rows = [
        _require_object(raw, f"{label} ordinary evidence")
        for raw in _require_list(ordinary_value, f"{label} ordinary evidence")
    ]
    expected_ordinary = {
        _require_text(
            _require_object(raw, f"{label} ordinary mapping").get("name"),
            f"{label} ordinary mapping name",
        ): _require_object(raw, f"{label} ordinary mapping")
        for raw in _require_list(
            target_mapping.get("ordinary_tests"), f"{label} ordinary mapping"
        )
    }
    if [row.get("name") for row in ordinary_rows] != sorted(expected_ordinary):
        fail(f"{label} ordinary evidence owner set/order drifted")
    for row in ordinary_rows:
        _exact_keys(
            row,
            {
                "name",
                "source_path",
                "baseline_sha256",
                "candidate_sha256",
                "helper_target",
            },
            f"{label} ordinary evidence",
        )
        name = _require_text(row["name"], f"{label} ordinary name")
        expected_row = expected_ordinary[name]
        if (
            row.get("source_path") != expected_row.get("source_path")
            or row.get("baseline_sha256") != expected_row.get("baseline_sha256")
            or row.get("candidate_sha256") != expected_row.get("candidate_sha256")
            or row.get("helper_target") != helper_by_test.get(name)
        ):
            fail(f"{label} ordinary evidence differs from its frozen mapping")


def _validate_exact_target_projection(
    value: Any,
    *,
    expected: dict[tuple[str, str], dict[str, Any]],
    label: str,
) -> list[dict[str, Any]]:
    expected_owners = sorted(expected)
    if _projection_target_owners(value, label) != expected_owners:
        fail(f"{label} owner set differs from its target contract")
    if not expected:
        if value != []:
            fail(f"{label} must be the exact empty projection")
        return []
    normalized = _native_cross_profile_target_parity(value, label)
    for target in normalized:
        owner = (target["project_id"], target["target"])
        contract = expected[owner]
        if (
            target["target_type"] != contract["target_type"]
            or target["artifact_relative"] != contract["artifact_relative"]
            or [source["path"] for source in target["direct_sources"]]
            != contract["direct_source_paths"]
        ):
            fail(f"{label} target type/artifact/source contract drifted: {owner}")
    return normalized


def _semantic_path_aliases(
    *,
    build_roots: dict[str, str],
    source_roots: dict[str, str],
    install_roots: dict[str, str],
) -> list[tuple[str, str]]:
    aliases = [
        (root, f"<build:{project_id}>")
        for project_id, root in build_roots.items()
    ] + [
        (root, f"<source:{project_id}>")
        for project_id, root in source_roots.items()
        if root.startswith("/qualification-output/")
    ] + [
        (root, f"<install:{project_id}>")
        for project_id, root in install_roots.items()
    ]
    if any(
        posixpath.normpath(path) != path
        or not path.startswith("/qualification-output/")
        for path, _alias in aliases
    ):
        fail("target-proof semantic alias roots are not canonical qualification paths")
    if len({path for path, _alias in aliases}) != len(aliases):
        fail("target-proof semantic alias roots overlap exactly")
    return sorted(aliases, key=lambda row: (-len(row[0]), row[0], row[1]))


def _normalize_semantic_path(
    value: Any, *, aliases: list[tuple[str, str]], label: str
) -> str:
    path = _require_text(value, label)
    if posixpath.normpath(path) != path or not path.startswith("/"):
        fail(f"{label} is not an absolute canonical path")
    normalized = path
    for prefix, alias in aliases:
        if normalized == prefix or normalized.startswith(f"{prefix}/"):
            normalized = f"{alias}{normalized[len(prefix):]}"
            break
    if "/qualification-output" in normalized:
        fail(f"{label} retains an uncontracted qualification-output path")
    return normalized


def _raw_source_path(
    normalized: str,
    *,
    build_roots: dict[str, str],
    source_roots: dict[str, str],
    install_roots: dict[str, str],
    label: str,
) -> str:
    if normalized.startswith("/workspace/"):
        return normalized
    match = re.fullmatch(r"<(build|source|install):([^>]+)>(/.*)", normalized)
    if match is None:
        fail(f"{label} has an unknown semantic path alias: {normalized}")
    kind, project_id, suffix = match.groups()
    roots = {
        "build": build_roots,
        "source": source_roots,
        "install": install_roots,
    }[kind]
    if project_id not in roots:
        fail(f"{label} has no {kind} root for {project_id}")
    return f"{roots[project_id]}{suffix}"


def _resolve_lane_dependency_path(
    logical_path: str,
    *,
    lane_root: Path,
    label: str,
) -> tuple[Path, str, list[str]]:
    """Replay one packet-native qualification-output symlink chain on the host."""

    if (
        posixpath.normpath(logical_path) != logical_path
        or not logical_path.startswith("/qualification-output/")
    ):
        fail(f"{label} is not a canonical qualification-output path")
    root = lane_root.resolve(strict=True)
    current = root / Path(logical_path).relative_to("/qualification-output")
    seen: set[Path] = set()
    chain: list[str] = []
    for _ in range(32):
        try:
            relative = current.relative_to(root)
        except ValueError:
            fail(f"{label} symlink escapes qualification output")
        chain.append(f"/qualification-output/{relative.as_posix()}")
        try:
            metadata = current.lstat()
        except OSError as error:
            fail(f"{label} is absent: {error}")
        if not stat.S_ISLNK(metadata.st_mode):
            if not stat.S_ISREG(metadata.st_mode):
                fail(f"{label} does not resolve to a regular file")
            if metadata.st_nlink != 1:
                fail(f"{label} resolves to an unqualified hardlink")
            try:
                resolved = current.resolve(strict=True)
                resolved_relative = resolved.relative_to(root)
            except (OSError, ValueError) as error:
                fail(f"{label} resolves outside qualification output: {error}")
            return (
                resolved,
                f"/qualification-output/{resolved_relative.as_posix()}",
                chain,
            )
        if current in seen:
            fail(f"{label} contains a symlink cycle")
        seen.add(current)
        try:
            target = Path(os.readlink(current))
        except OSError as error:
            fail(f"{label} symlink cannot be read: {error}")
        if target.is_absolute():
            target_text = target.as_posix()
            if not target_text.startswith("/qualification-output/"):
                fail(f"{label} absolute symlink escapes qualification output")
            current = root / target.relative_to("/qualification-output")
        else:
            current = Path(os.path.normpath(current.parent / target))
    fail(f"{label} exceeds the symlink-depth bound")


def _resolve_repository_dependency_path(
    logical_path: str,
    *,
    repository_root: Path,
    label: str,
) -> tuple[Path, str, list[str]]:
    """Replay one packet-native /workspace symlink chain on the host."""

    if (
        posixpath.normpath(logical_path) != logical_path
        or not logical_path.startswith("/workspace/")
    ):
        fail(f"{label} is not a canonical repository path")
    root = repository_root.resolve(strict=True)
    current = root / Path(logical_path).relative_to("/workspace")
    seen: set[Path] = set()
    chain: list[str] = []
    for _ in range(32):
        try:
            relative = current.relative_to(root)
        except ValueError:
            fail(f"{label} symlink escapes the repository")
        chain.append(f"/workspace/{relative.as_posix()}")
        try:
            metadata = current.lstat()
        except OSError as error:
            fail(f"{label} is absent: {error}")
        if not stat.S_ISLNK(metadata.st_mode):
            if not stat.S_ISREG(metadata.st_mode):
                fail(f"{label} does not resolve to a regular file")
            if metadata.st_nlink != 1:
                fail(f"{label} resolves to an unqualified hardlink")
            try:
                resolved = current.resolve(strict=True)
                resolved_relative = resolved.relative_to(root)
            except (OSError, ValueError) as error:
                fail(f"{label} resolves outside the repository: {error}")
            return resolved, f"/workspace/{resolved_relative.as_posix()}", chain
        if current in seen:
            fail(f"{label} contains a symlink cycle")
        seen.add(current)
        try:
            target = Path(os.readlink(current))
        except OSError as error:
            fail(f"{label} symlink cannot be read: {error}")
        if target.is_absolute():
            target_text = target.as_posix()
            if not target_text.startswith("/workspace/"):
                fail(f"{label} absolute symlink escapes the repository")
            current = root / target.relative_to("/workspace")
        else:
            current = Path(os.path.normpath(current.parent / target))
    fail(f"{label} exceeds the symlink-depth bound")


def _logical_path_is_within(path: str, root: str) -> bool:
    return path == root or path.startswith(f"{root}/")


def _command_input_kind(path: str) -> str:
    name = PurePosixPath(path).name
    if name.endswith((".o", ".obj")):
        return "object"
    if name.endswith(".a"):
        return "archive"
    if re.search(r"\.so(?:\.[0-9]+)*$", name):
        return "shared_library"
    return "authenticated_non_code_input"


def _replay_local_provenance_file(
    logical_path: str,
    *,
    lane_root: Path,
    repository_root: Path,
    label: str,
) -> tuple[Path, str, list[str]] | None:
    if logical_path.startswith("/qualification-output/"):
        return _resolve_lane_dependency_path(
            logical_path, lane_root=lane_root, label=label
        )
    if logical_path.startswith("/workspace/"):
        return _resolve_repository_dependency_path(
            logical_path, repository_root=repository_root, label=label
        )
    if logical_path.startswith(("/usr/", "/lib/", "/lib64/")):
        return None
    fail(f"{label} references an unauthenticated filesystem root")


def _validate_replayed_file_identity(
    value: dict[str, Any],
    *,
    lane_root: Path,
    repository_root: Path,
    label: str,
) -> None:
    logical_path = _require_text(value.get("path"), f"{label}.path")
    resolved_path = _require_text(
        value.get("resolved_path"), f"{label}.resolved_path"
    )
    digest = _require_text(value.get("sha256"), f"{label}.sha256")
    chain = _require_list(value.get("symlink_chain"), f"{label}.symlink_chain")
    if (
        posixpath.normpath(logical_path) != logical_path
        or not logical_path.startswith("/")
        or posixpath.normpath(resolved_path) != resolved_path
        or not resolved_path.startswith("/")
        or HEX64.fullmatch(digest) is None
    ):
        fail(f"{label} path/digest identity is malformed")
    replayed = _replay_local_provenance_file(
        logical_path,
        lane_root=lane_root,
        repository_root=repository_root,
        label=label,
    )
    if replayed is None:
        if (
            not chain
            or chain[0] != logical_path
            or any(
                not isinstance(item, str)
                or posixpath.normpath(item) != item
                or not item.startswith("/")
                for item in chain
            )
            or len(chain) != len(set(chain))
        ):
            fail(f"{label} pinned-container symlink evidence is malformed")
        return
    physical, replayed_resolved, replayed_chain = replayed
    if (
        resolved_path != replayed_resolved
        or chain != replayed_chain
        or _sha256_file(physical) != digest
    ):
        fail(f"{label} retained file identity drifted")


def _validate_command_sanitizer(
    value: Any, *, kind: str, profile: str, label: str
) -> None:
    if kind not in {"compile", "link"}:
        if value is not None:
            fail(f"{label} non-driver command has sanitizer evidence")
        return
    sanitizer = _require_object(value, f"{label}.sanitizer")
    _exact_keys(
        sanitizer,
        {
            "effective_sanitizers",
            "effective_frame_pointer",
            "disable_flags",
        },
        f"{label}.sanitizer",
    )
    effective = _require_list(
        sanitizer["effective_sanitizers"],
        f"{label}.sanitizer.effective_sanitizers",
    )
    disabled = _require_list(
        sanitizer["disable_flags"], f"{label}.sanitizer.disable_flags"
    )
    frame_pointer = sanitizer["effective_frame_pointer"]
    expected = {
        "none": [],
        "address+undefined": ["address", "undefined"],
        "thread": ["thread"],
    }
    if profile not in expected:
        fail(f"{label} uses an unknown sanitizer profile")
    if (
        effective != expected[profile]
        or disabled
        or (
            frame_pointer is not None
            and not isinstance(frame_pointer, bool)
        )
        or (profile != "none" and frame_pointer is not True)
    ):
        fail(f"{label} sanitizer projection drifted")


def _validate_native_file_magic(
    path: Path, *, expected: str, label: str
) -> None:
    try:
        with path.open("rb") as stream:
            magic = stream.read(8)
    except OSError as error:
        fail(f"{label} bytes cannot be inspected: {error}")
    if expected == "archive":
        if magic == b"!<thin>\n" or magic != b"!<arch>\n":
            fail(f"{label} is not one regular non-thin archive")
    elif expected == "elf":
        if not magic.startswith(b"\x7fELF"):
            fail(f"{label} is not an ELF object or shared image")
    else:
        fail(f"{label} requested an unknown native file format")


def _read_host_archive_members(path: Path, *, label: str) -> list[tuple[str, bytes]]:
    _validate_native_file_magic(path, expected="archive", label=label)
    listing = _run_checked(
        ["/usr/bin/ar", "t", str(path)],
        cwd=path.parent,
        label=f"{label} member listing",
    ).stdout
    names = [line for line in listing.splitlines() if line]
    if not names or len(names) != len(set(names)):
        fail(f"{label} is empty or repeats member names")
    members: list[tuple[str, bytes]] = []
    for name in names:
        extracted = _run_checked(
            ["/usr/bin/ar", "p", str(path), name],
            cwd=path.parent,
            label=f"{label} member {name}",
            text=False,
        ).stdout
        if not isinstance(extracted, bytes):
            fail(f"{label} member extraction returned text unexpectedly")
        members.append((name, extracted))
    return members


def _validate_target_proof_outer_envelope(
    proof: dict[str, Any],
    *,
    role: str,
    target_type: str,
    build_root: str,
    label: str,
) -> None:
    common = {
        "project_id",
        "build_root",
        "target",
        "role",
        "target_type",
        "artifact",
        "contract_artifact",
        "file_api_target_reply",
        "file_api_sources",
        "ninja_query",
        "ninja_commands_sha256",
        "generated_source_producers",
        "commands",
        "archives",
        "transitive_local_artifacts",
        "authenticated_output_dependencies",
    }
    if target_type == "UTILITY":
        expected = common | {
            "custom_command_output_edge",
            "generator",
            "generator_dependencies",
            "housekeeping_commands",
            "output_inventory",
            "cross_profile_output_inventory",
            "declared_byproducts",
            "consumer_relationships",
        }
    else:
        expected = common | {
            "authenticated_local_target_dependencies",
            "transitive_external_link_inputs",
            "required_generated_inputs",
        }
        requires_elf = (
            role in {"gtest", "ordinary_helper"}
            or (
                role
                in {"production", "production_dependency", "local_library"}
                and target_type
                in {"EXECUTABLE", "SHARED_LIBRARY", "MODULE_LIBRARY"}
            )
        )
        if requires_elf:
            expected.add("elf")
        if role == "gtest":
            expected |= {
                "registered_test_count",
                "registered_test_names_sha256",
                "contracted_test_count",
                "contracted_test_names_sha256",
                "additional_test_count",
            }
    _exact_keys(proof, expected, label)

    reply = _require_object(
        proof["file_api_target_reply"], f"{label}.file_api_target_reply"
    )
    _exact_keys(
        reply, {"path", "sha256"}, f"{label}.file_api_target_reply"
    )
    reply_path = _require_text(
        reply["path"], f"{label}.file_api_target_reply.path"
    )
    if (
        not _logical_path_is_within(reply_path, build_root)
        or HEX64.fullmatch(
            _require_text(
                reply["sha256"], f"{label}.file_api_target_reply.sha256"
            )
        )
        is None
        or HEX64.fullmatch(
            _require_text(
                proof["ninja_commands_sha256"],
                f"{label}.ninja_commands_sha256",
            )
        )
        is None
    ):
        fail(f"{label} File-API/Ninja identity is malformed")

    generated = [
        _require_text(value, f"{label}.generated_source_producers")
        for value in _require_list(
            proof["generated_source_producers"],
            f"{label}.generated_source_producers",
        )
    ]
    if (
        generated != sorted(set(generated))
        or any(not _logical_path_is_within(path, build_root) for path in generated)
    ):
        fail(f"{label} generated-source producer set drifted")

    query_rows = [
        _require_object(raw, f"{label}.ninja_query")
        for raw in _require_list(proof["ninja_query"], f"{label}.ninja_query")
    ]
    query_nodes: list[str] = []
    for query_index, query in enumerate(query_rows):
        query_label = f"{label}.ninja_query[{query_index}]"
        _exact_keys(query, {"node", "rule", "edges", "raw_sha256"}, query_label)
        node = _require_text(query["node"], f"{query_label}.node")
        _require_text(query["rule"], f"{query_label}.rule")
        if HEX64.fullmatch(
            _require_text(query["raw_sha256"], f"{query_label}.raw_sha256")
        ) is None:
            fail(f"{query_label} digest is malformed")
        edges = [
            _require_object(raw, f"{query_label}.edges")
            for raw in _require_list(query["edges"], f"{query_label}.edges")
        ]
        for edge in edges:
            _exact_keys(edge, {"type", "path"}, f"{query_label}.edge")
            if edge.get("type") not in {"explicit", "implicit", "order_only"}:
                fail(f"{query_label} has an unknown edge type")
            _require_text(edge.get("path"), f"{query_label}.edge.path")
        query_nodes.append(node)
    if (
        not query_rows
        or len(query_nodes) != len(set(query_nodes))
        or (
            target_type != "UTILITY"
            and query_nodes != sorted(query_nodes)
        )
    ):
        fail(f"{label} Ninja query set is empty, duplicated, or unsorted")

    for field in ("transitive_local_artifacts",):
        paths = [
            _require_text(path, f"{label}.{field}")
            for path in _require_list(proof[field], f"{label}.{field}")
        ]
        if paths != sorted(set(paths)) or any(
            not path.startswith("/qualification-output/") for path in paths
        ):
            fail(f"{label}.{field} is not an exact output-path set")


def _validate_target_command_file_evidence(
    value: Any,
    *,
    project_build_roots: dict[str, str],
    target_output_owners: Any,
    installed_output_owners: Any,
    expected_roles: frozenset[str],
    sanitizer_profile: str,
    lane_root: Path,
    repository_root: Path,
    label: str,
) -> None:
    """Authenticate exact native command rows without reparsing Ninja on the host.

    The packet-native producer remains authoritative for command discovery and argv
    decoding.  This verifier closes the independent host-checkable surface: exact
    row shapes, retained bytes, path/symlink identities, input/auxiliary joins, and
    final target-artifact ownership.  Detecting a coordinated removal from both a
    command input list and its evidence would require a second Ninja parser, which
    is deliberately outside the frozen authority split.
    """

    full_command_keys = {
        "kind",
        "directory",
        "output",
        "output_sha256",
        "inputs",
        "input_evidence",
        "auxiliary_outputs",
        "source",
        "dependency_closure",
        "argv_sha256",
        "parity_argv_sha256",
        "cross_profile_argv_sha256",
        "sanitizer",
    }
    utility_command_keys = {
        "kind",
        "output",
        "output_inventory_sha256",
        "inputs",
        "source",
        "argv_sha256",
        "parity_argv_sha256",
        "cross_profile_argv_sha256",
        "sanitizer",
    }
    proof_values = _require_list(value, label)
    output_owner_values = _require_list(
        target_output_owners, f"{label} target output owners"
    )
    eligible_output_owners: set[tuple[str, str, str, str, str, str]] = set()
    for owner_index, raw_owner_proof in enumerate(output_owner_values):
        owner_label = f"{label}[{owner_index}] output owner"
        owner_proof = _require_object(raw_owner_proof, owner_label)
        owner_artifact = _require_object(
            owner_proof.get("artifact"), f"{owner_label}.artifact"
        )
        owner_path = _require_text(
            owner_artifact.get("path"), f"{owner_label}.artifact.path"
        )
        owner_kind = _command_input_kind(owner_path)
        owner_target_type = _require_text(
            owner_proof.get("target_type"), f"{owner_label}.target_type"
        )
        if owner_kind not in {"archive", "shared_library"}:
            continue
        if (
            owner_kind == "archive"
            and owner_target_type != "STATIC_LIBRARY"
        ) or (
            owner_kind == "shared_library"
            and owner_target_type not in {"SHARED_LIBRARY", "MODULE_LIBRARY"}
        ):
            fail(f"{owner_label} artifact kind/type ownership drifted")
        eligible_output_owners.add(
            (
                "file_api_ninja_target",
                _require_text(
                    owner_proof.get("project_id"), f"{owner_label}.project_id"
                ),
                _require_text(
                    owner_proof.get("target"), f"{owner_label}.target"
                ),
                owner_path,
                _require_text(
                    owner_artifact.get("resolved_path"),
                    f"{owner_label}.artifact.resolved_path",
                ),
                _require_text(
                    owner_artifact.get("sha256"),
                    f"{owner_label}.artifact.sha256",
                ),
            )
        )
    for installed_index, raw_installed in enumerate(
        _require_list(installed_output_owners, f"{label} installed output owners")
    ):
        installed_label = f"{label} installed output owner[{installed_index}]"
        installed = _require_object(raw_installed, installed_label)
        eligible_output_owners.add(
            (
                "authenticated_install_copy",
                _require_text(
                    installed.get("producer_project"),
                    f"{installed_label}.producer_project",
                ),
                _require_text(
                    installed.get("producer_target"),
                    f"{installed_label}.producer_target",
                ),
                _require_text(installed.get("path"), f"{installed_label}.path"),
                _require_text(
                    installed.get("resolved_path"),
                    f"{installed_label}.resolved_path",
                ),
                _require_text(
                    installed.get("sha256"), f"{installed_label}.sha256"
                ),
            )
        )

    owners: set[tuple[str, str]] = set()
    owner_order: list[tuple[str, str]] = []
    for proof_index, raw_proof in enumerate(proof_values):
        proof_label = f"{label}[{proof_index}]"
        proof = _require_object(raw_proof, proof_label)
        project_id = _require_text(
            proof.get("project_id"), f"{proof_label}.project_id"
        )
        target_name = _require_text(
            proof.get("target"), f"{proof_label}.target"
        )
        owner = (project_id, target_name)
        if owner in owners:
            fail(f"{label} repeats target command owner {owner}")
        owners.add(owner)
        owner_order.append(owner)
        if owner_order != sorted(owner_order):
            fail(f"{label} target command owners are not sorted")
        build_root = _require_text(
            proof.get("build_root"), f"{proof_label}.build_root"
        )
        if (
            project_build_roots.get(project_id) != build_root
            or posixpath.normpath(build_root) != build_root
            or not build_root.startswith("/qualification-output/")
        ):
            fail(f"{proof_label} build-root ownership drifted")

        target_type = _require_text(
            proof.get("target_type"), f"{proof_label}.target_type"
        )
        role = _require_text(proof.get("role"), f"{proof_label}.role")
        if role not in expected_roles:
            fail(f"{proof_label} target role escaped its proof family")
        if target_type not in {
            "EXECUTABLE",
            "STATIC_LIBRARY",
            "SHARED_LIBRARY",
            "MODULE_LIBRARY",
            "OBJECT_LIBRARY",
            "UTILITY",
        }:
            fail(f"{proof_label} has an unsupported target type")
        if (
            role in {"gtest", "ordinary_helper"}
            and target_type != "EXECUTABLE"
        ) or (
            role == "binding_module" and target_type != "MODULE_LIBRARY"
        ) or (
            target_type == "UTILITY"
            and role
            not in {"production", "test_support", "production_dependency"}
        ):
            fail(f"{proof_label} target role/type identity drifted")
        _validate_target_proof_outer_envelope(
            proof,
            role=role,
            target_type=target_type,
            build_root=build_root,
            label=proof_label,
        )
        artifact = _require_object(
            proof.get("artifact"), f"{proof_label}.artifact"
        )
        _exact_keys(
            artifact,
            {"path", "resolved_path", "sha256"},
            f"{proof_label}.artifact",
        )
        artifact_path = _require_text(
            artifact["path"], f"{proof_label}.artifact.path"
        )
        artifact_digest = _require_text(
            artifact["sha256"], f"{proof_label}.artifact.sha256"
        )
        if not _logical_path_is_within(artifact_path, build_root):
            fail(f"{proof_label} artifact escapes its project build")
        physical_artifact, resolved_artifact, artifact_chain = (
            _resolve_lane_dependency_path(
                artifact_path,
                lane_root=lane_root,
                label=f"{proof_label} retained artifact",
            )
        )
        if (
            artifact.get("resolved_path") != resolved_artifact
            or not _logical_path_is_within(resolved_artifact, build_root)
            or any(
                not _logical_path_is_within(path, build_root)
                for path in artifact_chain
            )
            or HEX64.fullmatch(artifact_digest) is None
            or _sha256_file(physical_artifact) != artifact_digest
            or (
                role != "local_library"
                and artifact_chain != [artifact_path]
            )
            or (target_type == "EXECUTABLE" and not os.access(physical_artifact, os.X_OK))
        ):
            fail(f"{proof_label} retained artifact identity drifted")
        if target_type == "STATIC_LIBRARY":
            _validate_native_file_magic(
                physical_artifact,
                expected="archive",
                label=f"{proof_label} retained artifact",
            )
        elif target_type in {
            "EXECUTABLE",
            "SHARED_LIBRARY",
            "MODULE_LIBRARY",
            "OBJECT_LIBRARY",
        }:
            _validate_native_file_magic(
                physical_artifact,
                expected="elf",
                label=f"{proof_label} retained artifact",
            )

        commands = _require_list(proof.get("commands"), f"{proof_label}.commands")
        if target_type == "UTILITY":
            if len(commands) != 1:
                fail(f"{proof_label} UTILITY command set is not exact")
            command = _require_object(commands[0], f"{proof_label}.commands[0]")
            _exact_keys(command, utility_command_keys, f"{proof_label}.commands[0]")
            command_label = f"{proof_label}.commands[0]"
            command_output = _require_text(
                command["output"], f"{command_label}.output"
            )
            command_inputs = _require_list(
                command["inputs"], f"{command_label}.inputs"
            )
            generator = _require_object(
                proof.get("generator"), f"{proof_label}.generator"
            )
            _exact_keys(
                generator,
                {
                    "project_id",
                    "target",
                    "path",
                    "sha256",
                    "sanitizer_profile",
                },
                f"{proof_label}.generator",
            )
            inventory = _require_object(
                proof.get("output_inventory"), f"{proof_label}.output_inventory"
            )
            output_policy = inventory.get("policy")
            expected_output = (
                artifact_path
                if output_policy == "single_file"
                else inventory.get("root")
            )
            if (
                command.get("kind") != "generated"
                or posixpath.normpath(command_output) != command_output
                or not _logical_path_is_within(command_output, build_root)
                or command_output != expected_output
                or command.get("output_inventory_sha256")
                != inventory.get("entries_sha256")
                or HEX64.fullmatch(
                    _require_text(
                        command.get("output_inventory_sha256"),
                        f"{command_label}.output_inventory_sha256",
                    )
                )
                is None
                or command_inputs != [generator.get("path")]
                or command.get("source") is not None
            ):
                fail(f"{command_label} UTILITY generator join drifted")
            generator_path = _require_text(
                generator.get("path"), f"{proof_label}.generator.path"
            )
            generator_digest = _require_text(
                generator.get("sha256"), f"{proof_label}.generator.sha256"
            )
            physical_generator, resolved_generator, _generator_chain = (
                _resolve_lane_dependency_path(
                    generator_path,
                    lane_root=lane_root,
                    label=f"{proof_label} retained generator",
                )
            )
            if (
                resolved_generator != generator_path
                or HEX64.fullmatch(generator_digest) is None
                or _sha256_file(physical_generator) != generator_digest
                or not os.access(physical_generator, os.X_OK)
                or generator.get("sanitizer_profile") != sanitizer_profile
            ):
                fail(f"{proof_label} retained UTILITY generator drifted")
            for digest_key in (
                "argv_sha256",
                "parity_argv_sha256",
                "cross_profile_argv_sha256",
            ):
                if HEX64.fullmatch(
                    _require_text(command.get(digest_key), f"{command_label}.{digest_key}")
                ) is None:
                    fail(f"{command_label} command digest is malformed")
            _validate_command_sanitizer(
                command.get("sanitizer"),
                kind="generated",
                profile=sanitizer_profile,
                label=command_label,
            )
            if _require_list(
                proof.get("authenticated_output_dependencies"),
                f"{proof_label}.authenticated_output_dependencies",
            ):
                fail(f"{proof_label} UTILITY target has output dependencies")
            continue

        if not commands:
            fail(f"{proof_label} material target has no commands")
        command_outputs: list[str] = []
        command_output_identities: set[tuple[str, str, str]] = set()
        selected_build_inputs: set[tuple[str, str, str]] = set()
        qualification_command_inputs: dict[
            tuple[str, str, str], dict[str, Any]
        ] = {}
        final_kind = "archive" if target_type == "STATIC_LIBRARY" else "link"
        final_matches: list[dict[str, Any]] = []
        archive_commands: dict[str, tuple[Path, str, list[dict[str, Any]]]] = {}
        for command_index, raw_command in enumerate(commands):
            command_label = f"{proof_label}.commands[{command_index}]"
            command = _require_object(raw_command, command_label)
            _exact_keys(command, full_command_keys, command_label)
            kind = _require_text(command["kind"], f"{command_label}.kind")
            if kind not in {"compile", "link", "archive", "generated"}:
                fail(f"{command_label} kind is unsupported")
            directory = _require_text(
                command["directory"], f"{command_label}.directory"
            )
            output = _require_text(command["output"], f"{command_label}.output")
            output_digest = _require_text(
                command["output_sha256"], f"{command_label}.output_sha256"
            )
            if (
                posixpath.normpath(directory) != directory
                or not _logical_path_is_within(directory, build_root)
                or posixpath.normpath(output) != output
                or not _logical_path_is_within(output, build_root)
                or HEX64.fullmatch(output_digest) is None
            ):
                fail(f"{command_label} output/directory identity is malformed")
            _require_contained_plain_directory(
                lane_root / Path(directory).relative_to("/qualification-output"),
                lane_root,
                f"{command_label} retained command directory",
            )
            physical_output, resolved_output, output_chain = (
                _resolve_lane_dependency_path(
                    output, lane_root=lane_root, label=f"{command_label} output"
                )
            )
            if (
                _sha256_file(physical_output) != output_digest
                or not _logical_path_is_within(resolved_output, build_root)
                or any(
                    not _logical_path_is_within(chain_path, build_root)
                    for chain_path in output_chain
                )
                or (
                    len(output_chain) != 1
                    and not (
                        kind == "link"
                        and _command_input_kind(output) == "shared_library"
                    )
                )
            ):
                fail(f"{command_label} retained output identity drifted")
            command_outputs.append(output)
            command_output_identities.add(
                (output, resolved_output, output_digest)
            )

            inputs = [
                _require_text(item, f"{command_label}.inputs")
                for item in _require_list(
                    command["inputs"], f"{command_label}.inputs"
                )
            ]
            input_evidence: list[dict[str, Any]] = []
            for input_index, raw_input in enumerate(
                _require_list(
                    command["input_evidence"], f"{command_label}.input_evidence"
                )
            ):
                input_label = (
                    f"{command_label}.input_evidence[{input_index}]"
                )
                input_row = _require_object(raw_input, input_label)
                _exact_keys(
                    input_row,
                    {
                        "path",
                        "resolved_path",
                        "symlink_chain",
                        "sha256",
                        "scope",
                        "kind",
                    },
                    input_label,
                )
                input_path = _require_text(input_row["path"], f"{input_label}.path")
                expected_scope: str
                if _logical_path_is_within(input_path, build_root):
                    expected_scope = "selected_build"
                elif input_path.startswith("/qualification-output/"):
                    expected_scope = "qualification_output"
                elif input_path.startswith("/workspace/"):
                    expected_scope = "repository_input"
                elif input_path.startswith(
                    (
                        "/lib/",
                        "/lib64/",
                        "/usr/lib/",
                        "/usr/lib64/",
                        "/usr/local/lib/",
                        "/usr/local/lib64/",
                    )
                ):
                    expected_scope = "pinned_container"
                else:
                    fail(f"{input_label} path escaped qualified command roots")
                if (
                    input_row.get("scope") != expected_scope
                    or input_row.get("kind") != _command_input_kind(input_path)
                ):
                    fail(f"{input_label} scope/kind identity drifted")
                _validate_replayed_file_identity(
                    input_row,
                    lane_root=lane_root,
                    repository_root=repository_root,
                    label=input_label,
                )
                if expected_scope == "selected_build" and (
                    not _logical_path_is_within(
                        _require_text(
                            input_row["resolved_path"],
                            f"{input_label}.resolved_path",
                        ),
                        build_root,
                    )
                    or any(
                        not isinstance(chain_path, str)
                        or not _logical_path_is_within(chain_path, build_root)
                        for chain_path in _require_list(
                            input_row["symlink_chain"],
                            f"{input_label}.symlink_chain",
                        )
                    )
                ):
                    fail(f"{input_label} selected-build symlink escaped its owner")
                if expected_scope == "qualification_output":
                    input_identity = (
                        input_path,
                        _require_text(
                            input_row["resolved_path"],
                            f"{input_label}.resolved_path",
                        ),
                        _require_text(
                            input_row["sha256"], f"{input_label}.sha256"
                        ),
                    )
                    qualification_command_inputs[input_identity] = input_row
                elif expected_scope == "selected_build":
                    selected_build_inputs.add(
                        (
                            input_path,
                            _require_text(
                                input_row["resolved_path"],
                                f"{input_label}.resolved_path",
                            ),
                            _require_text(
                                input_row["sha256"], f"{input_label}.sha256"
                            ),
                        )
                    )
                input_evidence.append(input_row)
            if (
                inputs != sorted(inputs)
                or inputs != [row["path"] for row in input_evidence]
                or input_evidence
                != sorted(
                    input_evidence,
                    key=lambda row: (row["path"], row["sha256"]),
                )
                or output in inputs
            ):
                fail(f"{command_label} input/evidence join drifted")

            if kind in {"link", "archive"}:
                for input_row in input_evidence:
                    replayed_input = _replay_local_provenance_file(
                        input_row["path"],
                        lane_root=lane_root,
                        repository_root=repository_root,
                        label=f"{command_label} native input",
                    )
                    if replayed_input is None:
                        # Exact-image replay validates pinned-container magic after
                        # all stopped-lane claims have been collected in one batch.
                        continue
                    physical_input = replayed_input[0]
                    _validate_native_file_magic(
                        physical_input,
                        expected=(
                            "archive"
                            if input_row["kind"] == "archive"
                            else "elf"
                        ),
                        label=f"{command_label} input {input_row['path']}",
                    )
            if kind == "archive":
                archive_commands[output] = (
                    physical_output,
                    output_digest,
                    input_evidence,
                )

            auxiliary_outputs: list[dict[str, Any]] = []
            for auxiliary_index, raw_auxiliary in enumerate(
                _require_list(
                    command["auxiliary_outputs"],
                    f"{command_label}.auxiliary_outputs",
                )
            ):
                auxiliary_label = (
                    f"{command_label}.auxiliary_outputs[{auxiliary_index}]"
                )
                auxiliary = _require_object(raw_auxiliary, auxiliary_label)
                _exact_keys(auxiliary, {"path", "sha256"}, auxiliary_label)
                auxiliary_path = _require_text(
                    auxiliary["path"], f"{auxiliary_label}.path"
                )
                auxiliary_digest = _require_text(
                    auxiliary["sha256"], f"{auxiliary_label}.sha256"
                )
                if (
                    not _logical_path_is_within(auxiliary_path, build_root)
                    or HEX64.fullmatch(auxiliary_digest) is None
                ):
                    fail(f"{auxiliary_label} identity escaped its build")
                physical_auxiliary, _resolved_auxiliary, auxiliary_chain = (
                    _resolve_lane_dependency_path(
                        auxiliary_path,
                        lane_root=lane_root,
                        label=auxiliary_label,
                    )
                )
                if (
                    auxiliary_chain != [auxiliary_path]
                    or _sha256_file(physical_auxiliary) != auxiliary_digest
                ):
                    fail(f"{auxiliary_label} retained bytes drifted")
                auxiliary_outputs.append(auxiliary)
            if (
                auxiliary_outputs
                != sorted(auxiliary_outputs, key=lambda row: row["path"])
                or len({row["path"] for row in auxiliary_outputs})
                != len(auxiliary_outputs)
                or (kind != "link" and auxiliary_outputs)
            ):
                fail(f"{command_label} auxiliary-output set is not exact/sorted")

            if kind == "compile" and (inputs or input_evidence):
                fail(f"{command_label} compile command has linker inputs")
            if kind == "archive":
                member_names = [PurePosixPath(path).name for path in inputs]
                if (
                    not input_evidence
                    or any(row["kind"] != "object" for row in input_evidence)
                    or any(not name for name in member_names)
                    or len(member_names) != len(set(member_names))
                ):
                    fail(
                        f"{command_label} archive command has an invalid member set"
                    )
            if kind == "generated":
                if (
                    _command_input_kind(output)
                    != "authenticated_non_code_input"
                    or len(input_evidence) != 1
                    or input_evidence[0]["scope"] != "selected_build"
                    or input_evidence[0]["kind"]
                    != "authenticated_non_code_input"
                ):
                    fail(f"{command_label} generated command owner is not exact")
                generated_owner, _resolved_owner, _owner_chain = (
                    _resolve_lane_dependency_path(
                        input_evidence[0]["path"],
                        lane_root=lane_root,
                        label=f"{command_label} executable generator",
                    )
                )
                if not os.access(generated_owner, os.X_OK):
                    fail(f"{command_label} generated command owner is not executable")

            source_value = command["source"]
            closure_value = command["dependency_closure"]
            if kind == "compile":
                source = _require_object(source_value, f"{command_label}.source")
                _exact_keys(source, {"path", "sha256"}, f"{command_label}.source")
                closure = _require_object(
                    closure_value, f"{command_label}.dependency_closure"
                )
                _exact_keys(
                    closure,
                    {"object", "declared_dependency_count", "raw_sha256", "files"},
                    f"{command_label}.dependency_closure",
                )
                closure_files = _require_list(
                    closure["files"], f"{command_label}.dependency_closure.files"
                )
                dependency_identities: dict[str, str] = {}
                for dependency_index, raw_dependency in enumerate(closure_files):
                    dependency_label = (
                        f"{command_label}.dependency_closure.files"
                        f"[{dependency_index}]"
                    )
                    dependency = _require_object(raw_dependency, dependency_label)
                    _exact_keys(
                        dependency,
                        {
                            "path",
                            "resolved_path",
                            "symlink_chain",
                            "sha256",
                            "cross_profile_sha256",
                            "scope",
                            "producer",
                            "source_identity",
                        },
                        dependency_label,
                    )
                    _validate_replayed_file_identity(
                        dependency,
                        lane_root=lane_root,
                        repository_root=repository_root,
                        label=dependency_label,
                    )
                    dependency_path = _require_text(
                        dependency["path"], f"{dependency_label}.path"
                    )
                    dependency_digest = _require_text(
                        dependency["sha256"], f"{dependency_label}.sha256"
                    )
                    cross_digest = _require_text(
                        dependency["cross_profile_sha256"],
                        f"{dependency_label}.cross_profile_sha256",
                    )
                    if (
                        HEX64.fullmatch(cross_digest) is None
                        or dependency_path in dependency_identities
                    ):
                        fail(f"{dependency_label} identity is duplicated/malformed")
                    dependency_identities[dependency_path] = dependency_digest
                source_path = _require_text(
                    source["path"], f"{command_label}.source.path"
                )
                source_digest = _require_text(
                    source["sha256"], f"{command_label}.source.sha256"
                )
                declared_count = closure["declared_dependency_count"]
                if (
                    PurePosixPath(source_path).suffix
                    not in {".c", ".cc", ".cpp", ".cxx", ".C"}
                    or _command_input_kind(output) != "object"
                    or closure.get("object") != output
                    or not isinstance(declared_count, int)
                    or isinstance(declared_count, bool)
                    or declared_count != len(closure_files)
                    or not closure_files
                    or HEX64.fullmatch(
                        _require_text(
                            closure.get("raw_sha256"),
                            f"{command_label}.dependency_closure.raw_sha256",
                        )
                    )
                    is None
                    or list(dependency_identities) != sorted(dependency_identities)
                    or dependency_identities.get(source_path) != source_digest
                ):
                    fail(f"{command_label} compile closure/source join drifted")
            elif source_value is not None or closure_value is not None:
                fail(f"{command_label} non-compile source/closure shape drifted")

            for digest_key in (
                "argv_sha256",
                "parity_argv_sha256",
                "cross_profile_argv_sha256",
            ):
                if HEX64.fullmatch(
                    _require_text(command[digest_key], f"{command_label}.{digest_key}")
                ) is None:
                    fail(f"{command_label} command digest is malformed")
            _validate_command_sanitizer(
                command["sanitizer"],
                kind=kind,
                profile=sanitizer_profile,
                label=command_label,
            )
            if (
                kind == final_kind
                and output == artifact_path
                and output_digest == artifact_digest
            ):
                final_matches.append(command)

        archive_rows = [
            _require_object(raw, f"{proof_label}.archives")
            for raw in _require_list(
                proof.get("archives"), f"{proof_label}.archives"
            )
        ]
        if (
            [row.get("path") for row in archive_rows]
            != sorted(archive_commands)
            or len(archive_rows) != len(archive_commands)
        ):
            fail(f"{proof_label} archive evidence owner set/order drifted")
        for archive_index, archive in enumerate(archive_rows):
            archive_label = f"{proof_label}.archives[{archive_index}]"
            _exact_keys(archive, {"path", "sha256", "members"}, archive_label)
            archive_path = _require_text(
                archive["path"], f"{archive_label}.path"
            )
            physical_archive, archive_digest, archive_inputs = archive_commands[
                archive_path
            ]
            if archive.get("sha256") != archive_digest:
                fail(f"{archive_label} digest differs from its command output")
            actual_members = _read_host_archive_members(
                physical_archive, label=archive_label
            )
            claimed_members = [
                _require_object(raw, f"{archive_label}.members")
                for raw in _require_list(
                    archive["members"], f"{archive_label}.members"
                )
            ]
            input_by_path = {row["path"]: row for row in archive_inputs}
            if len(input_by_path) != len(archive_inputs) or len(claimed_members) != len(
                actual_members
            ):
                fail(f"{archive_label} member/input multiplicity drifted")
            claimed_objects: list[str] = []
            for member_index, (member_name, member_bytes) in enumerate(
                actual_members
            ):
                member_label = f"{archive_label}.members[{member_index}]"
                member = claimed_members[member_index]
                _exact_keys(
                    member,
                    {"name", "sha256", "producer_object"},
                    member_label,
                )
                producer_object = _require_text(
                    member["producer_object"], f"{member_label}.producer_object"
                )
                producer = input_by_path.get(producer_object)
                member_digest = hashlib.sha256(member_bytes).hexdigest()
                if (
                    producer is None
                    or member.get("name") != member_name
                    or PurePosixPath(producer_object).name != member_name
                    or member.get("sha256") != member_digest
                    or producer.get("sha256") != member_digest
                ):
                    fail(f"{member_label} differs from archive/object bytes")
                claimed_objects.append(producer_object)
            if set(claimed_objects) != set(input_by_path):
                fail(f"{archive_label} member producer set drifted")
        authenticated_output_dependencies: dict[
            tuple[str, str, str], dict[str, Any]
        ] = {}
        authenticated_dependency_order: list[tuple[str, str, str]] = []
        for dependency_index, raw_dependency in enumerate(
            _require_list(
                proof.get("authenticated_output_dependencies"),
                f"{proof_label}.authenticated_output_dependencies",
            )
        ):
            dependency_label = (
                f"{proof_label}.authenticated_output_dependencies"
                f"[{dependency_index}]"
            )
            dependency = _require_object(raw_dependency, dependency_label)
            _exact_keys(
                dependency,
                {"path", "resolved_path", "sha256", "owner"},
                dependency_label,
            )
            owner = _require_object(
                dependency["owner"], f"{dependency_label}.owner"
            )
            _exact_keys(
                owner,
                {
                    "kind",
                    "project_id",
                    "target",
                    "path",
                    "resolved_path",
                    "sha256",
                },
                f"{dependency_label}.owner",
            )
            owner_identity = (
                _require_text(
                    owner["kind"], f"{dependency_label}.owner.kind"
                ),
                _require_text(
                    owner["project_id"],
                    f"{dependency_label}.owner.project_id",
                ),
                _require_text(
                    owner["target"], f"{dependency_label}.owner.target"
                ),
                _require_text(
                    owner["path"], f"{dependency_label}.owner.path"
                ),
                _require_text(
                    owner["resolved_path"],
                    f"{dependency_label}.owner.resolved_path",
                ),
                _require_text(
                    owner["sha256"], f"{dependency_label}.owner.sha256"
                ),
            )
            identity = (
                _require_text(dependency["path"], f"{dependency_label}.path"),
                _require_text(
                    dependency["resolved_path"],
                    f"{dependency_label}.resolved_path",
                ),
                _require_text(
                    dependency["sha256"], f"{dependency_label}.sha256"
                ),
            )
            if identity in authenticated_output_dependencies:
                fail(f"{dependency_label} repeats an output dependency")
            matching_owners = {
                candidate
                for candidate in eligible_output_owners
                if candidate[5] == identity[2]
                and (
                    candidate[3] == identity[0]
                    or candidate[4] == identity[1]
                )
            }
            if (
                matching_owners != {owner_identity}
                or owner_identity[1:3] == (project_id, target_name)
                or identity[1] != owner_identity[4]
                or identity[2] != owner_identity[5]
            ):
                fail(f"{dependency_label} output dependency owner join drifted")
            authenticated_output_dependencies[identity] = dependency
            authenticated_dependency_order.append(
                (identity[0], owner_identity[1], owner_identity[2])
            )
        if set(authenticated_output_dependencies) != set(
            qualification_command_inputs
        ) or authenticated_dependency_order != sorted(
            authenticated_dependency_order
        ):
            fail(f"{proof_label} command/output-dependency join drifted")
        if not selected_build_inputs.issubset(command_output_identities):
            fail(f"{proof_label} selected-build input has no command owner")
        if (
            command_outputs != sorted(command_outputs)
            or len(command_outputs) != len(set(command_outputs))
            or len(final_matches) != 1
        ):
            fail(f"{proof_label} command set/final artifact ownership drifted")


def _configure_generated_dependency_specs(
    build_contract: dict[str, Any],
    *,
    label: str,
) -> dict[str, dict[str, Any]]:
    specs: dict[str, dict[str, Any]] = {}
    for index, raw in enumerate(
        _require_list(
            build_contract["configure_generated_input_contracts"],
            f"{label} configure-generated contracts",
        )
    ):
        row_label = f"{label} configure-generated contract[{index}]"
        row = _require_object(raw, row_label)
        output_name = _require_text(row.get("output_name"), f"{row_label}.output")
        if output_name in specs:
            fail(f"{label} repeats configure-generated output {output_name}")
        owner_project = _require_text(
            row.get("owner_project_id"), f"{row_label}.owner"
        )
        allowed_paths = [
            _safe_relative_path(
                _require_text(path, f"{row_label}.allowed path"),
                f"{row_label}.allowed path",
            ).as_posix()
            for path in _require_list(
                row.get("allowed_build_relative_paths"),
                f"{row_label}.allowed paths",
            )
        ]
        required_paths = sorted(
            {
                f"/workspace/{owner_project}/CMakeLists.txt",
                f"/workspace/{_require_text(row.get('template_repository_path'), f'{row_label}.template')}",
                *(
                    f"/workspace/{_require_text(path, f'{row_label}.semantic input')}"
                    for path in _require_list(
                        row.get("required_semantic_repository_paths"),
                        f"{row_label}.semantic inputs",
                    )
                ),
            }
        )
        if (
            not allowed_paths
            or allowed_paths != sorted(allowed_paths)
            or len(allowed_paths) != len(set(allowed_paths))
            or not required_paths
            or row.get("semantic_input_policy")
            != "all_authenticated_cmake_inputs_and_exact_cache"
        ):
            fail(f"{row_label} route/input contract drifted")
        specs[output_name] = {
            "owner_project_id": owner_project,
            "allowed_build_relative_paths": allowed_paths,
            "required_input_paths": required_paths,
        }
    if not specs:
        fail(f"{label} configure-generated contract is empty")
    return specs


def _qualified_install_owner_index(value: Any, *, label: str) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for index, raw in enumerate(_require_list(value, label)):
        row = _require_object(raw, f"{label}[{index}]")
        path = _require_text(row.get("path"), f"{label}[{index}].path")
        if path in result:
            fail(f"{label} repeats owner path {path}")
        result[path] = row
    return result


def _validate_target_proofs_against_projection(
    proofs_value: Any,
    projection_value: Any,
    *,
    expected: dict[tuple[str, str], dict[str, Any]],
    build_roots: dict[str, str],
    source_roots: dict[str, str],
    install_roots: dict[str, str],
    configure_generated_specs: dict[str, dict[str, Any]],
    qualified_install_owners: dict[str, dict[str, Any]],
    project_provenance: dict[str, dict[str, Any]],
    repository_root: Path,
    lane_root: Path,
    allow_unexpected_proofs: bool,
    label: str,
) -> dict[tuple[str, str], dict[str, Any]]:
    normalized_projection = _validate_exact_target_projection(
        projection_value, expected=expected, label=f"{label} projection"
    )
    projection_rows = {
        (
            _require_text(row.get("project_id"), f"{label} projection project"),
            _require_text(row.get("target"), f"{label} projection target"),
        ): row
        for row in _require_list(projection_value, f"{label} projection")
    }
    normalized_rows = {
        (row["project_id"], row["target"]): row for row in normalized_projection
    }
    aliases = _semantic_path_aliases(
        build_roots=build_roots,
        source_roots=source_roots,
        install_roots=install_roots,
    )
    proofs: dict[tuple[str, str], dict[str, Any]] = {}
    unexpected: list[tuple[str, str]] = []
    executable_producer_claims: list[
        tuple[tuple[str, str], str, dict[str, Any], str]
    ] = []
    for index, raw_proof in enumerate(
        _require_list(proofs_value, f"{label} target proofs")
    ):
        proof_label = f"{label} target proof[{index}]"
        proof = _require_object(raw_proof, proof_label)
        owner = (
            _require_text(proof.get("project_id"), f"{proof_label}.project_id"),
            _require_text(proof.get("target"), f"{proof_label}.target"),
        )
        if owner not in expected:
            unexpected.append(owner)
            continue
        if owner in proofs:
            fail(f"{label} repeats target proof {owner}")
        contract = expected[owner]
        artifact = _require_object(proof.get("artifact"), f"{proof_label}.artifact")
        _exact_keys(
            artifact,
            {"path", "resolved_path", "sha256"},
            f"{proof_label}.artifact",
        )
        expected_artifact = (
            f"{build_roots[owner[0]]}/{contract['artifact_relative']}"
        )
        artifact_digest = _require_text(
            artifact.get("sha256"), f"{proof_label}.artifact.sha256"
        )
        if proof.get("role") == "local_library":
            (
                physical_artifact,
                replayed_artifact,
                artifact_chain,
            ) = _resolve_lane_dependency_path(
                expected_artifact,
                lane_root=lane_root,
                label=f"{proof_label} contracted local-library artifact",
            )
            if (
                not _logical_path_is_within(
                    replayed_artifact, build_roots[owner[0]]
                )
                or any(
                    not _logical_path_is_within(path, build_roots[owner[0]])
                    for path in artifact_chain
                )
            ):
                fail(f"{proof_label} local-library symlink escaped its build")
        else:
            physical_artifact = _require_contained_regular_file(
                lane_root
                / Path(expected_artifact).relative_to("/qualification-output"),
                lane_root,
                f"{proof_label} contracted artifact",
                require_single_link=True,
            )
            replayed_artifact = expected_artifact
        if (
            proof.get("target_type") != contract["target_type"]
            or proof.get("contract_artifact") != contract["artifact_relative"]
            or artifact.get("path") != expected_artifact
            or artifact.get("resolved_path") != replayed_artifact
            or HEX64.fullmatch(artifact_digest) is None
            or _sha256_file(physical_artifact) != artifact_digest
            or projection_rows[owner].get("artifact_sha256") != artifact_digest
        ):
            fail(f"{proof_label} artifact/type identity differs from its projection")

        if contract["target_type"] == "UTILITY":
            raw_inventory = _require_object(
                proof.get("output_inventory"), f"{proof_label}.output_inventory"
            )
            inventory_root = _require_text(
                raw_inventory.get("root"), f"{proof_label}.output_inventory.root"
            )
            if (
                posixpath.normpath(inventory_root) != inventory_root
                or not inventory_root.startswith("/qualification-output/")
            ):
                fail(f"{proof_label} utility output root is not canonical")
            cross_inventory = _require_object(
                proof.get("cross_profile_output_inventory"),
                f"{proof_label}.cross_profile_output_inventory",
            )
            _exact_keys(
                cross_inventory,
                {"entries", "entries_sha256"},
                f"{proof_label}.cross_profile_output_inventory",
            )
            expected_cross_entries: list[dict[str, str]] = []
            for entry_index, raw_entry in enumerate(
                _require_list(
                    raw_inventory.get("entries"),
                    f"{proof_label}.output_inventory.entries",
                )
            ):
                entry_label = f"{proof_label}.output_inventory.entries[{entry_index}]"
                entry = _require_object(raw_entry, entry_label)
                relative = _safe_relative_path(
                    _require_text(entry.get("relative_path"), f"{entry_label}.path"),
                    f"{entry_label}.path",
                ).as_posix()
                kind = _require_text(entry.get("kind"), f"{entry_label}.kind")
                projected_entry: dict[str, str] = {
                    "relative_path": relative,
                    "kind": kind,
                }
                if kind == "file":
                    projected_entry["mode"] = _require_text(
                        entry.get("mode"), f"{entry_label}.mode"
                    )
                    physical_output = _require_contained_regular_file(
                        lane_root
                        / Path(inventory_root).relative_to("/qualification-output")
                        / Path(relative),
                        lane_root,
                        f"{entry_label} retained utility output",
                        require_single_link=True,
                    )
                    try:
                        projected_entry["cross_profile_sha256"] = (
                            generated_output_cross_profile_sha256(physical_output)
                        )
                    except NativeBuildProvenanceError as error:
                        fail(
                            f"{entry_label} utility-output normalization failed: "
                            f"{error}"
                        )
                elif kind != "directory":
                    fail(f"{entry_label} has an unknown utility-output kind")
                expected_cross_entries.append(projected_entry)
            expected_cross_digest = _canonical_json_sha256(expected_cross_entries)
            if (
                cross_inventory["entries"] != expected_cross_entries
                or cross_inventory["entries_sha256"] != expected_cross_digest
                or projection_rows[owner].get(
                    "output_inventory_cross_profile_sha256"
                )
                != expected_cross_digest
            ):
                fail(
                    f"{proof_label} utility output projection is not derived from "
                    "its retained bytes"
                )
        elif "cross_profile_output_inventory" in proof:
            fail(f"{proof_label} non-UTILITY target has a utility output projection")

        source_rows: list[dict[str, str]] = []
        for source_index, raw_source in enumerate(
            _require_list(proof.get("file_api_sources"), f"{proof_label}.sources")
        ):
            source_label = f"{proof_label}.sources[{source_index}]"
            source = _require_object(raw_source, source_label)
            _exact_keys(
                source,
                {"path", "sha256", "cross_profile_sha256"},
                source_label,
            )
            raw_path = _require_text(source["path"], f"{source_label}.path")
            raw_digest = _require_text(source["sha256"], f"{source_label}.sha256")
            cross_digest = _require_text(
                source["cross_profile_sha256"],
                f"{source_label}.cross_profile_sha256",
            )
            normalized_path = _normalize_semantic_path(
                raw_path, aliases=aliases, label=f"{source_label}.path"
            )
            if (
                HEX64.fullmatch(raw_digest) is None
                or HEX64.fullmatch(cross_digest) is None
            ):
                fail(f"{source_label} digest semantics drifted")
            if raw_path.startswith(f"{build_roots[owner[0]]}/"):
                physical_source = _require_contained_regular_file(
                    lane_root / Path(raw_path).relative_to("/qualification-output"),
                    lane_root,
                    f"{source_label} generated source",
                    require_single_link=True,
                )
                try:
                    recomputed_cross_digest = generated_output_cross_profile_sha256(
                        physical_source
                    )
                except NativeBuildProvenanceError as error:
                    fail(f"{source_label} generated-source normalization failed: {error}")
                if cross_digest != recomputed_cross_digest:
                    fail(f"{source_label} generated-source normalization drifted")
            elif raw_digest != cross_digest:
                fail(f"{source_label} non-generated source changed parity digest")
            source_rows.append(
                {
                    "raw_path": raw_path,
                    "path": normalized_path,
                    "sha256": cross_digest,
                }
            )
        expected_raw_paths = [
            _raw_source_path(
                path,
                build_roots=build_roots,
                source_roots=source_roots,
                install_roots=install_roots,
                label=f"{proof_label}.expected source",
            )
            for path in contract["direct_source_paths"]
        ]
        if (
            [row["raw_path"] for row in source_rows] != sorted(expected_raw_paths)
            or len({row["raw_path"] for row in source_rows}) != len(source_rows)
            or len({row["path"] for row in source_rows}) != len(source_rows)
        ):
            fail(f"{proof_label} raw File-API source set drifted")
        projected_direct_sources = normalized_rows[owner]["direct_sources"]
        if [
            {"path": row["path"], "sha256": row["sha256"]}
            for row in sorted(source_rows, key=lambda row: row["path"])
        ] != projected_direct_sources:
            fail(f"{proof_label} direct-source projection is not derived from its proof")

        raw_command_semantics: list[dict[str, str]] = []
        command_semantics: list[dict[str, str]] = []
        closure_sources: dict[str, str] = {}
        compile_source_counts: Counter[str] = Counter()
        compile_dependencies: dict[str, dict[str, Any]] = {}
        for command_index, raw_command in enumerate(
            _require_list(proof.get("commands"), f"{proof_label}.commands")
        ):
            command_label = f"{proof_label}.commands[{command_index}]"
            command = _require_object(raw_command, command_label)
            kind = _require_text(command.get("kind"), f"{command_label}.kind")
            raw_command_output = _require_text(
                command.get("output"), f"{command_label}.output"
            )
            output = _normalize_semantic_path(
                raw_command_output,
                aliases=aliases,
                label=f"{command_label}.output",
            )
            parity_digest = _require_text(
                command.get("parity_argv_sha256"),
                f"{command_label}.parity_argv_sha256",
            )
            cross_digest = _require_text(
                command.get("cross_profile_argv_sha256"),
                f"{command_label}.cross_profile_argv_sha256",
            )
            if (
                HEX64.fullmatch(parity_digest) is None
                or HEX64.fullmatch(cross_digest) is None
            ):
                fail(f"{command_label} parity command digest is malformed")
            raw_command_semantics.append(
                {
                    "kind": kind,
                    "output": output,
                    "argv_sha256": parity_digest,
                    "cross_profile_argv_sha256": cross_digest,
                }
            )
            command_semantics.append(
                {
                    "kind": kind,
                    "output": output,
                    "cross_profile_argv_sha256": cross_digest,
                }
            )
            dependency_closure = command.get("dependency_closure")
            closure_dependency_identities: dict[str, str] = {}
            if dependency_closure is not None:
                closure = _require_object(
                    dependency_closure, f"{command_label}.dependency_closure"
                )
                _exact_keys(
                    closure,
                    {"object", "declared_dependency_count", "raw_sha256", "files"},
                    f"{command_label}.dependency_closure",
                )
                closure_files = _require_list(
                    closure.get("files"),
                    f"{command_label}.dependency_closure.files",
                )
                declared_dependency_count = closure.get(
                    "declared_dependency_count"
                )
                if (
                    kind != "compile"
                    or closure.get("object") != raw_command_output
                    or not isinstance(declared_dependency_count, int)
                    or isinstance(declared_dependency_count, bool)
                    or declared_dependency_count != len(closure_files)
                    or not closure_files
                    or HEX64.fullmatch(
                        _require_text(
                            closure.get("raw_sha256"),
                            f"{command_label}.dependency_closure.raw_sha256",
                        )
                    )
                    is None
                ):
                    fail(f"{command_label} dependency-closure envelope drifted")
                for dependency_index, raw_dependency in enumerate(
                    closure_files
                ):
                    dependency_label = (
                        f"{command_label}.dependency_closure.files[{dependency_index}]"
                    )
                    dependency = _require_object(raw_dependency, dependency_label)
                    _exact_keys(
                        dependency,
                        {
                            "path",
                            "resolved_path",
                            "symlink_chain",
                            "sha256",
                            "cross_profile_sha256",
                            "scope",
                            "producer",
                            "source_identity",
                        },
                        dependency_label,
                    )
                    raw_dependency_path = _require_text(
                        dependency.get("path"), f"{dependency_label}.path"
                    )
                    dependency_path = _normalize_semantic_path(
                        raw_dependency_path,
                        aliases=aliases,
                        label=f"{dependency_label}.path",
                    )
                    raw_dependency_digest = _require_text(
                        dependency.get("sha256"), f"{dependency_label}.sha256"
                    )
                    previous_raw_dependency = closure_dependency_identities.setdefault(
                        raw_dependency_path, raw_dependency_digest
                    )
                    if previous_raw_dependency != raw_dependency_digest:
                        fail(f"{command_label} dependency identities conflict")
                    dependency_cross_digest = _require_text(
                        dependency.get("cross_profile_sha256"),
                        f"{dependency_label}.cross_profile_sha256",
                    )
                    scope = _require_text(
                        dependency.get("scope"), f"{dependency_label}.scope"
                    )
                    if (
                        HEX64.fullmatch(raw_dependency_digest) is None
                        or HEX64.fullmatch(dependency_cross_digest) is None
                    ):
                        fail(f"{dependency_label} digest is malformed")
                    resolved_path = _require_text(
                        dependency.get("resolved_path"),
                        f"{dependency_label}.resolved_path",
                    )
                    physical_lane_dependency: Path | None = None
                    physical_repository_dependency: Path | None = None
                    if raw_dependency_path.startswith("/qualification-output/"):
                        (
                            physical_lane_dependency,
                            replayed_resolved_path,
                            replayed_symlink_chain,
                        ) = _resolve_lane_dependency_path(
                            raw_dependency_path,
                            lane_root=lane_root,
                            label=f"{dependency_label} lane-local input",
                        )
                        if (
                            resolved_path != replayed_resolved_path
                            or dependency.get("symlink_chain")
                            != replayed_symlink_chain
                            or _sha256_file(physical_lane_dependency)
                            != raw_dependency_digest
                        ):
                            fail(
                                f"{dependency_label} raw/resolved lane-input "
                                "identity drifted"
                            )
                    elif raw_dependency_path.startswith("/workspace/"):
                        if (
                            resolved_path != raw_dependency_path
                            or dependency.get("symlink_chain")
                            != [raw_dependency_path]
                        ):
                            fail(
                                f"{dependency_label} repository input path/chain "
                                "drifted"
                            )
                        physical_repository_dependency = (
                            _require_contained_regular_file(
                                repository_root
                                / Path(raw_dependency_path).relative_to("/workspace"),
                                repository_root,
                                f"{dependency_label} repository input",
                                require_single_link=True,
                            )
                        )
                        if (
                            _sha256_file(physical_repository_dependency)
                            != raw_dependency_digest
                        ):
                            fail(f"{dependency_label} repository input bytes drifted")
                    if scope in {
                        "configure_generated_input",
                        "generated_build_input",
                    }:
                        if physical_lane_dependency is None:
                            fail(
                                f"{dependency_label} generated input escaped the lane"
                            )
                        try:
                            recomputed_dependency_digest = (
                                compile_dependency_cross_profile_sha256(
                                    physical_lane_dependency, scope
                                )
                            )
                        except NativeBuildProvenanceError as error:
                            fail(
                                f"{dependency_label} normalization failed: {error}"
                            )
                        if dependency_cross_digest != recomputed_dependency_digest:
                            fail(f"{dependency_label} normalization drifted")
                    elif scope in {
                        "repository_source",
                        "qualified_external_source",
                        "qualified_install_input",
                        "pinned_container",
                    } and dependency_cross_digest != raw_dependency_digest:
                        fail(f"{dependency_label} changed a non-generated parity digest")
                    elif scope not in {
                        "repository_source",
                        "qualified_external_source",
                        "qualified_install_input",
                        "pinned_container",
                    }:
                        fail(f"{dependency_label} has an unknown dependency scope")

                    producer_value = dependency.get("producer")
                    producer_kind: str | None = None
                    producer_project: str | None = None
                    producer_target: str | None = None
                    producer_inputs_sha256: str | None = None
                    if producer_value is not None:
                        producer = _require_object(
                            producer_value, f"{dependency_label}.producer"
                        )
                        producer_kind = _require_text(
                            producer.get("kind"), f"{dependency_label}.producer.kind"
                        )
                        if producer_kind == "executable_build_generator":
                            _exact_keys(
                                producer,
                                {
                                    "kind",
                                    "project_id",
                                    "target",
                                    "artifact",
                                    "artifact_sha256",
                                    "argv_sha256",
                                    "parity_argv_sha256",
                                },
                                f"{dependency_label}.producer",
                            )
                            producer_project = _require_text(
                                producer.get("project_id"),
                                f"{dependency_label}.producer.project_id",
                            )
                            producer_target = _require_text(
                                producer.get("target"),
                                f"{dependency_label}.producer.target",
                            )
                            executable_producer_claims.append(
                                (
                                    owner,
                                    _require_text(
                                        dependency.get("path"),
                                        f"{dependency_label}.path",
                                    ),
                                    producer,
                                    dependency_label,
                                )
                            )
                        elif producer_kind == "cmake_configure_file":
                            _exact_keys(
                                producer,
                                {
                                    "kind",
                                    "owner_project_id",
                                    "output",
                                    "output_sha256",
                                    "cmake",
                                    "cmake_cache",
                                    "semantic_input_policy",
                                    "required_inputs",
                                    "all_configure_inputs_sha256",
                                },
                                f"{dependency_label}.producer",
                            )
                            producer_project = _require_text(
                                producer.get("owner_project_id"),
                                f"{dependency_label}.producer.owner_project_id",
                            )
                            configure_spec = configure_generated_specs.get(
                                PurePosixPath(raw_dependency_path).name
                            )
                            configure_project = _require_object(
                                project_provenance.get(owner[0]),
                                f"{dependency_label} consuming project provenance",
                            )
                            configure_inputs = _require_list(
                                configure_project.get("configure_inputs"),
                                f"{dependency_label} project configure inputs",
                            )
                            configure_input_index: dict[str, dict[str, Any]] = {}
                            for raw_configure_input in configure_inputs:
                                configure_input = _require_object(
                                    raw_configure_input,
                                    f"{dependency_label} project configure input",
                                )
                                configure_input_path = _require_text(
                                    configure_input.get("path"),
                                    f"{dependency_label} project configure input path",
                                )
                                if configure_input_path in configure_input_index:
                                    fail(
                                        f"{dependency_label} project configure inputs "
                                        "repeat a path"
                                    )
                                configure_input_index[configure_input_path] = (
                                    configure_input
                                )
                            try:
                                configure_output_relative = PurePosixPath(
                                    raw_dependency_path
                                ).relative_to(PurePosixPath(build_roots[owner[0]]))
                            except ValueError:
                                fail(
                                    f"{dependency_label} configure output escaped its "
                                    "consumer build"
                                )
                            required_inputs = []
                            for input_index, raw_input in enumerate(
                                _require_list(
                                    producer.get("required_inputs"),
                                    f"{dependency_label}.producer.required_inputs",
                                )
                            ):
                                input_label = (
                                    f"{dependency_label}.producer.required_inputs"
                                    f"[{input_index}]"
                                )
                                input_row = _require_object(raw_input, input_label)
                                _exact_keys(
                                    input_row,
                                    {
                                        "path",
                                        "sha256",
                                        "is_generated",
                                        "is_external",
                                        "is_cmake",
                                        "identity",
                                    },
                                    input_label,
                                )
                                identity = _require_object(
                                    input_row.get("identity"),
                                    f"{input_label}.identity",
                                )
                                _exact_keys(
                                    identity,
                                    {"kind", "git_root", "path", "mode", "blob"},
                                    f"{input_label}.identity",
                                )
                                input_path = _require_text(
                                    input_row.get("path"), f"{input_label}.path"
                                )
                                input_digest = _require_text(
                                    input_row.get("sha256"), f"{input_label}.sha256"
                                )
                                input_identity_root = _require_text(
                                    identity.get("git_root"),
                                    f"{input_label}.identity.git_root",
                                )
                                input_identity_relative = _safe_relative_path(
                                    _require_text(
                                        identity.get("path"),
                                        f"{input_label}.identity.path",
                                    ),
                                    f"{input_label}.identity.path",
                                ).as_posix()
                                if not input_path.startswith("/workspace/"):
                                    fail(
                                        f"{input_label} configure input escaped "
                                        "the repository"
                                    )
                                physical_configure_input = (
                                    _require_contained_regular_file(
                                        repository_root
                                        / Path(input_path).relative_to("/workspace"),
                                        repository_root,
                                        input_label,
                                        require_single_link=True,
                                    )
                                )
                                identity_blob = _require_text(
                                    identity.get("blob"),
                                    f"{input_label}.identity.blob",
                                )
                                identity_mode = identity.get("mode")
                                configure_input_bytes = (
                                    physical_configure_input.read_bytes()
                                )
                                actual_input_blob = hashlib.sha1(
                                    f"blob {len(configure_input_bytes)}\0".encode(
                                        "ascii"
                                    )
                                    + configure_input_bytes,
                                ).hexdigest()
                                if (
                                    HEX64.fullmatch(input_digest) is None
                                    or _sha256_file(physical_configure_input)
                                    != input_digest
                                    or input_row.get("is_generated") is not False
                                    or type(input_row.get("is_external")) is not bool
                                    or input_row.get("is_cmake") is not False
                                    or identity.get("kind") != "git_tracked_source"
                                    or input_identity_root != "/workspace"
                                    or posixpath.normpath(input_identity_root)
                                    != input_identity_root
                                    or input_path
                                    != f"{input_identity_root}/{input_identity_relative}"
                                    or identity_mode not in {"100644", "100755"}
                                    or HEX40.fullmatch(identity_blob) is None
                                    or actual_input_blob != identity_blob
                                    or bool(
                                        physical_configure_input.stat().st_mode
                                        & stat.S_IXUSR
                                    )
                                    != (identity_mode == "100755")
                                ):
                                    fail(f"{input_label} tracked-input shape drifted")
                                required_inputs.append(
                                    {
                                        "path": input_path,
                                        "sha256": input_digest,
                                        "identity": identity,
                                    }
                                )
                            if (
                                not required_inputs
                                or required_inputs
                                != sorted(required_inputs, key=lambda row: row["path"])
                                or len({row["path"] for row in required_inputs})
                                != len(required_inputs)
                                or producer.get("output") != raw_dependency_path
                                or producer.get("output_sha256")
                                != raw_dependency_digest
                                or producer.get("semantic_input_policy")
                                != "all_authenticated_cmake_inputs_and_exact_cache"
                                or configure_spec is None
                                or producer_project
                                != configure_spec["owner_project_id"]
                                or configure_output_relative.as_posix()
                                not in configure_spec[
                                    "allowed_build_relative_paths"
                                ]
                                or [row["path"] for row in required_inputs]
                                != configure_spec["required_input_paths"]
                                or any(
                                    configure_input_index.get(row["path"])
                                    != raw_required
                                    for row, raw_required in zip(
                                        required_inputs,
                                        _require_list(
                                            producer.get("required_inputs"),
                                            f"{dependency_label}.producer.required_inputs",
                                        ),
                                        strict=True,
                                    )
                                )
                                or producer.get("cmake")
                                != configure_project.get("cmake")
                                or producer.get("cmake_cache")
                                != configure_project.get("cmake_cache")
                                or producer.get("all_configure_inputs_sha256")
                                != _canonical_json_sha256(configure_inputs)
                            ):
                                fail(
                                    f"{dependency_label} configure producer identity "
                                    "drifted"
                                )
                            producer_inputs_sha256 = _canonical_json_sha256(
                                required_inputs
                            )
                        elif producer_kind == "qualified_install_input":
                            _exact_keys(
                                producer,
                                {"kind", "owner"},
                                f"{dependency_label}.producer",
                            )
                            installed_owner = _require_object(
                                producer.get("owner"),
                                f"{dependency_label}.producer.owner",
                            )
                            _exact_keys(
                                installed_owner,
                                {
                                    "path",
                                    "resolved_path",
                                    "symlink_chain",
                                    "sha256",
                                    "project_id",
                                    "install_relative_path",
                                    "install_manifest",
                                    "producer",
                                },
                                f"{dependency_label}.producer.owner",
                            )
                            installed_producer = _require_object(
                                installed_owner.get("producer"),
                                f"{dependency_label}.producer.owner.producer",
                            )
                            producer_project = _require_text(
                                installed_owner.get("project_id"),
                                f"{dependency_label}.producer.owner.project_id",
                            )
                            producer_inputs_sha256 = _canonical_json_sha256(
                                {
                                    "installed_sha256": _require_text(
                                        installed_owner.get("sha256"),
                                        f"{dependency_label}.installed sha256",
                                    ),
                                    "producer_kind": _require_text(
                                        installed_producer.get("kind"),
                                        f"{dependency_label}.installed producer kind",
                                    ),
                                    "producer_sha256": _require_text(
                                        installed_producer.get("sha256"),
                                        f"{dependency_label}.installed producer sha256",
                                    ),
                                }
                            )
                            install_relative = _safe_relative_path(
                                _require_text(
                                    installed_owner.get("install_relative_path"),
                                    f"{dependency_label}.installed relative path",
                                ),
                                f"{dependency_label}.installed relative path",
                            ).as_posix()
                            install_root = install_roots.get(producer_project)
                            installed_producer_digest = _require_text(
                                installed_producer.get("sha256"),
                                f"{dependency_label}.installed producer sha256",
                            )
                            if (
                                install_root is None
                                or installed_owner.get("path")
                                != raw_dependency_path
                                or installed_owner.get("resolved_path")
                                != resolved_path
                                or installed_owner.get("sha256")
                                != raw_dependency_digest
                                or raw_dependency_path
                                != f"{install_root}/{install_relative}"
                                or installed_owner.get("symlink_chain") != []
                                or installed_producer.get("sha256")
                                != raw_dependency_digest
                                or HEX64.fullmatch(installed_producer_digest) is None
                            ):
                                fail(
                                    f"{dependency_label} qualified install owner "
                                    "join drifted"
                                )
                            if qualified_install_owners.get(raw_dependency_path) != (
                                installed_owner
                            ):
                                fail(
                                    f"{dependency_label} embedded install owner "
                                    "differs from the authenticated inventory"
                                )
                        else:
                            fail(f"{dependency_label} has an unknown producer kind")
                    source_identity_value = dependency.get("source_identity")
                    expected_producer_kind = {
                        "repository_source": None,
                        "qualified_external_source": None,
                        "pinned_container": None,
                        "qualified_install_input": "qualified_install_input",
                        "configure_generated_input": "cmake_configure_file",
                        "generated_build_input": "executable_build_generator",
                    }[scope]
                    expects_source_identity = scope in {
                        "repository_source",
                        "qualified_external_source",
                    }
                    if producer_kind != expected_producer_kind:
                        fail(f"{dependency_label} scope/producer ownership drifted")
                    if (
                        scope == "generated_build_input"
                        and producer_project != owner[0]
                    ):
                        fail(f"{dependency_label} generated producer project drifted")
                    if expects_source_identity:
                        source_identity = _require_object(
                            source_identity_value,
                            f"{dependency_label}.source_identity",
                        )
                        _exact_keys(
                            source_identity,
                            {"git_root", "path", "mode", "blob"},
                            f"{dependency_label}.source_identity",
                        )
                        identity_root = _require_text(
                            source_identity.get("git_root"),
                            f"{dependency_label}.source_identity.git_root",
                        )
                        identity_relative = _safe_relative_path(
                            _require_text(
                                source_identity.get("path"),
                                f"{dependency_label}.source_identity.path",
                            ),
                            f"{dependency_label}.source_identity.path",
                        ).as_posix()
                        identity_mode = _require_text(
                            source_identity.get("mode"),
                            f"{dependency_label}.source_identity.mode",
                        )
                        identity_blob = _require_text(
                            source_identity.get("blob"),
                            f"{dependency_label}.source_identity.blob",
                        )
                        source_project = _require_object(
                            project_provenance.get(owner[0]),
                            f"{dependency_label} source project provenance",
                        )
                        source_generation = _require_object(
                            source_project.get("source_git_generation"),
                            f"{dependency_label} source Git generation",
                        )
                        expected_identity_root = (
                            "/workspace"
                            if scope == "repository_source"
                            else source_generation.get("git_root")
                        )
                        if (
                            posixpath.normpath(identity_root) != identity_root
                            or not identity_root.startswith("/")
                            or raw_dependency_path
                            != f"{identity_root}/{identity_relative}"
                            or identity_root
                            != expected_identity_root
                            or identity_mode not in {"100644", "100755"}
                            or HEX40.fullmatch(identity_blob) is None
                        ):
                            fail(f"{dependency_label} tracked-source identity drifted")
                        physical_identity_file = (
                            physical_lane_dependency
                            if physical_lane_dependency is not None
                            else physical_repository_dependency
                        )
                        if physical_identity_file is not None:
                            raw_identity_bytes = physical_identity_file.read_bytes()
                            actual_blob = hashlib.sha1(
                                f"blob {len(raw_identity_bytes)}\0".encode("ascii")
                                + raw_identity_bytes,
                            ).hexdigest()
                            executable = bool(
                                physical_identity_file.stat().st_mode
                                & stat.S_IXUSR
                            )
                            if (
                                actual_blob != identity_blob
                                or executable != (identity_mode == "100755")
                            ):
                                fail(
                                    f"{dependency_label} tracked-source bytes/mode "
                                    "drifted"
                                )
                    elif source_identity_value is not None:
                        fail(f"{dependency_label} scope/source identity drifted")
                    if (
                        scope == "repository_source"
                        and not raw_dependency_path.startswith("/workspace/")
                    ):
                        fail(f"{dependency_label} repository scope escaped workspace")
                    if scope == "qualified_external_source":
                        source_root = source_roots.get(owner[0])
                        if (
                            source_root is None
                            or not source_root.startswith("/qualification-output/")
                            or not raw_dependency_path.startswith(f"{source_root}/")
                            or resolved_path != raw_dependency_path
                            or dependency.get("symlink_chain")
                            != [raw_dependency_path]
                        ):
                            fail(
                                f"{dependency_label} qualified external-source "
                                "route drifted"
                            )
                    if scope == "qualified_install_input" and not any(
                        raw_dependency_path.startswith(f"{root}/")
                        for root in install_roots.values()
                    ):
                        fail(f"{dependency_label} qualified install route drifted")
                    if scope in {
                        "configure_generated_input",
                        "generated_build_input",
                    } and not raw_dependency_path.startswith(
                        f"{build_roots[owner[0]]}/"
                    ):
                        fail(f"{dependency_label} generated-input build route drifted")
                    if (
                        scope == "generated_build_input"
                        and PurePosixPath(raw_dependency_path).name
                        in configure_generated_specs
                    ):
                        fail(
                            f"{dependency_label} reserved configure-generated output "
                            "was relabelled as an executable-generated input"
                        )
                    if (
                        scope in {
                            "qualified_external_source",
                            "qualified_install_input",
                            "configure_generated_input",
                            "generated_build_input",
                        }
                        and physical_lane_dependency is None
                    ):
                        fail(f"{dependency_label} lane-local scope escaped its lane")
                    if scope == "pinned_container" and (
                        not raw_dependency_path.startswith(("/usr/", "/lib/"))
                        or not resolved_path.startswith(("/usr/", "/lib/"))
                    ):
                        fail(f"{dependency_label} pinned scope escaped system roots")
                    projected_dependency = {
                        "path": dependency_path,
                        "sha256": dependency_cross_digest,
                        "scope": scope,
                        "producer_kind": producer_kind,
                        "producer_project": producer_project,
                        "producer_target": producer_target,
                        "producer_inputs_sha256": producer_inputs_sha256,
                    }
                    previous_dependency = compile_dependencies.setdefault(
                        dependency_path, projected_dependency
                    )
                    if previous_dependency != projected_dependency:
                        fail(f"{proof_label} dependency has conflicting identities")
                if list(closure_dependency_identities) != sorted(
                    closure_dependency_identities
                ) or len(closure_dependency_identities) != len(closure_files):
                    fail(f"{command_label} dependency closure is not exact/sorted")
            source_value = command.get("source")
            if source_value is not None:
                source = _require_object(source_value, f"{command_label}.source")
                _exact_keys(
                    source,
                    {"path", "sha256"},
                    f"{command_label}.source",
                )
                raw_source_path = _require_text(
                    source.get("path"), f"{command_label}.source.path"
                )
                source_path = _normalize_semantic_path(
                    raw_source_path,
                    aliases=aliases,
                    label=f"{command_label}.source.path",
                )
                source_digest = _require_text(
                    source.get("sha256"), f"{command_label}.source.sha256"
                )
                if HEX64.fullmatch(source_digest) is None:
                    fail(f"{command_label}.source digest is malformed")
                if closure_dependency_identities.get(raw_source_path) != source_digest:
                    fail(f"{command_label} source is absent from its dependency closure")
                previous = closure_sources.setdefault(source_path, source_digest)
                if previous != source_digest:
                    fail(f"{proof_label} source closure has conflicting digests")
                compile_source_counts[source_path] += 1
            if (kind == "compile") != (
                dependency_closure is not None and source_value is not None
            ):
                fail(f"{command_label} compile source/closure shape drifted")
        if sorted(
            raw_command_semantics,
            key=lambda row: (row["kind"], row["output"], row["argv_sha256"]),
        ) != projection_rows[owner]["command_semantics"]:
            fail(f"{proof_label} raw command projection is not derived from its proof")
        if sorted(
            command_semantics, key=lambda row: (row["output"], row["kind"])
        ) != normalized_rows[owner]["command_semantics"]:
            fail(f"{proof_label} command projection is not derived from its proof")
        if [
            {"path": path, "sha256": closure_sources[path]}
            for path in sorted(closure_sources)
        ] != normalized_rows[owner]["closure_sources"]:
            fail(f"{proof_label} source-closure projection is not derived from its proof")
        compiled_contract_paths = contract.get("compiled_source_paths")
        dependency_only_contract_paths = contract.get(
            "dependency_only_source_paths"
        )
        if (compiled_contract_paths is None) != (
            dependency_only_contract_paths is None
        ):
            fail(f"{proof_label} source-ownership contract is partial")
        if compiled_contract_paths is not None:
            if sorted(
                [*compiled_contract_paths, *dependency_only_contract_paths]
            ) != contract["direct_source_paths"]:
                fail(f"{proof_label} source-ownership contract drifted")
            if any(
                compile_source_counts[path] != 1
                for path in compiled_contract_paths
            ):
                fail(
                    f"{proof_label} direct source has no exact compile-command owner"
                )
            if not set(dependency_only_contract_paths).issubset(
                compile_dependencies
            ):
                fail(
                    f"{proof_label} dependency-only direct source has no "
                    "compiler-dependency owner"
                )
        if [compile_dependencies[path] for path in sorted(compile_dependencies)] != (
            normalized_rows[owner]["compile_dependencies"]
        ):
            fail(f"{proof_label} dependency projection is not derived from its proof")
        proofs[owner] = proof
    for consumer_owner, generated_path, producer, producer_label in (
        executable_producer_claims
    ):
        producer_owner = (
            _require_text(producer["project_id"], f"{producer_label}.project_id"),
            _require_text(producer["target"], f"{producer_label}.target"),
        )
        producer_proof = proofs.get(producer_owner)
        if producer_proof is None or producer_proof.get("target_type") != "EXECUTABLE":
            fail(f"{producer_label} has no proved executable owner")
        producer_artifact = _require_object(
            producer_proof.get("artifact"), f"{producer_label} target artifact"
        )
        physical_producer_artifact = _require_contained_regular_file(
            lane_root
            / Path(
                _require_text(
                    producer_artifact.get("resolved_path"),
                    f"{producer_label} target artifact resolved path",
                )
            ).relative_to("/qualification-output"),
            lane_root,
            f"{producer_label} executable target artifact",
            require_single_link=True,
        )
        if not os.access(physical_producer_artifact, os.X_OK):
            fail(f"{producer_label} proved generator artifact is not executable")
        generated_commands = [
            command
            for raw_command in _require_list(
                proofs[consumer_owner].get("commands"),
                f"{producer_label} consumer commands",
            )
            if (
                command := _require_object(raw_command, f"{producer_label} command")
            ).get("kind")
            == "generated"
            and command.get("output") == generated_path
        ]
        if (
            producer.get("artifact") != producer_artifact.get("path")
            or producer.get("artifact_sha256") != producer_artifact.get("sha256")
            or len(generated_commands) != 1
            or producer.get("argv_sha256")
            != generated_commands[0].get("argv_sha256")
            or producer.get("parity_argv_sha256")
            != generated_commands[0].get("parity_argv_sha256")
        ):
            fail(f"{producer_label} generated-command/executable owner join drifted")
        generated_command = generated_commands[0]
        command_inputs = _require_list(
            generated_command.get("inputs"),
            f"{producer_label} generated command inputs",
        )
        input_evidence_rows = _require_list(
            generated_command.get("input_evidence"),
            f"{producer_label} generated command input evidence",
        )
        if len(input_evidence_rows) != 1:
            fail(f"{producer_label} generated command input evidence is not exact")
        input_evidence = _require_object(
            input_evidence_rows[0], f"{producer_label} generator input evidence"
        )
        _exact_keys(
            input_evidence,
            {
                "path",
                "resolved_path",
                "symlink_chain",
                "sha256",
                "scope",
                "kind",
            },
            f"{producer_label} generator input evidence",
        )
        if (
            command_inputs != [producer_artifact.get("path")]
            or input_evidence
            != {
                "path": producer_artifact.get("path"),
                "resolved_path": producer_artifact.get("resolved_path"),
                "symlink_chain": [producer_artifact.get("path")],
                "sha256": producer_artifact.get("sha256"),
                "scope": "selected_build",
                "kind": "authenticated_non_code_input",
            }
        ):
            fail(f"{producer_label} generated command input owner join drifted")
    if set(proofs) != set(expected):
        fail(f"{label} target proof owner set drifted")
    if unexpected and not allow_unexpected_proofs:
        fail(f"{label} contains unexpected target proofs: {sorted(unexpected)}")
    return proofs


def _validate_native_profile_parity(
    build_contract: dict[str, Any],
    proven: dict[str, dict[str, Any]],
) -> None:
    parity_contract = _require_object(
        build_contract["cross_profile_parity_contract"],
        "cross-profile parity contract",
    )
    for index, raw_comparison in enumerate(
        _require_list(
            parity_contract["comparisons"], "cross-profile parity comparisons"
        )
    ):
        label = f"cross-profile parity comparisons[{index}]"
        comparison = _require_object(raw_comparison, label)
        kind = comparison.get("kind")
        if kind not in {"native_cross_profile", "native_same_profile_exact"}:
            continue
        lane_ids = [
            _require_text(value, f"{label}.lane_ids")
            for value in _require_list(comparison.get("lane_ids"), f"{label}.lane_ids")
        ]
        if not set(lane_ids).issubset(proven):
            continue
        expected_owners = _comparison_target_owners(
            comparison.get("target_owners"), f"{label}.target_owners"
        )
        field = (
            "cross_profile_parity"
            if kind == "native_cross_profile"
            else "production_parity"
        )
        raw_values: list[Any] = []
        normalized_values: list[list[dict[str, Any]]] = []
        for lane_id in lane_ids:
            value = proven[lane_id].get(field)
            if _projection_target_owners(value, f"{lane_id}.{field}") != expected_owners:
                fail(f"{lane_id} {field} owner set differs from {label}")
            raw_values.append(value)
            normalized_values.append(
                _native_cross_profile_target_parity(value, f"{lane_id}.{field}")
            )
        if kind == "native_cross_profile":
            if any(value != normalized_values[0] for value in normalized_values[1:]):
                fail(f"{label} authenticated target semantics differ across profiles")
        elif any(value != raw_values[0] for value in raw_values[1:]):
            fail(f"{label} same-profile products are not byte/recipe identical")


def _validate_grouped_results(
    manifest: dict[str, Any],
    artifact_root: Path,
    log_root: Path,
    successful_lanes: set[str],
    repository_root: Path,
    expected_manyforge_generation: dict[str, Any],
) -> tuple[list[dict[str, Any]], dict[str, set[str]]]:
    contract = manifest["test_inventory_contract"]
    build_contract = _load_build_provenance_contract(
        contract, manifest, repository_root
    )
    groups, _ = _load_baseline_test_inventory(
        contract, manifest, repository_root
    )
    target_groups, ordinary_sources = _load_native_target_contract(
        contract, manifest, repository_root
    )
    lane_ordinals = {
        lane["id"]: lane["ordinal"] for lane in manifest["qualification_lanes"]
    }
    summaries: list[dict[str, Any]] = []
    passed_by_lane: dict[str, set[str]] = {}
    proven_native: dict[str, dict[str, Any]] = {}
    for row in contract["native_execution_lanes"]:
        lane_id = row["lane_id"]
        if lane_id not in successful_lanes:
            continue
        lane_root = _lane_artifact_root(artifact_root, lane_id)
        inventory_path = _require_output_file(
            lane_root,
            row["inventory_output"],
            f"{lane_id} native CTest inventory",
        )
        registered = _ctest_inventory_names(
            inventory_path, f"{lane_id} native CTest inventory"
        )
        required = groups[row["required_group"]]
        if not required.issubset(registered):
            fail(f"{lane_id} deleted required sanitizer tests")
        if not row["allow_additional_tests"] and registered != required:
            fail(f"{lane_id} sanitizer registry differs from its exact frozen set")
        target_mapping = _validate_native_target_mapping(
            inventory_path,
            lane_id=lane_id,
            build_root=row["build_root"],
            required_group=row["required_group"],
            required_names=required,
            allow_additional_tests=row["allow_additional_tests"],
            target_groups=target_groups,
            ordinary_sources=ordinary_sources,
        )
        provenance_path = _require_output_file(
            lane_root,
            row["provenance_output"],
            f"{lane_id} native build provenance",
        )
        provenance, proven_gtest_artifacts, authenticated_provenance_files = (
            _validate_native_build_provenance_artifact(
                provenance_path,
                lane_root=lane_root,
                repository_root=repository_root,
                row=row,
                target_mapping=target_mapping,
                build_contract=build_contract,
                expected_manyforge_generation=expected_manyforge_generation,
            )
        )
        proven_native[lane_id] = provenance
        excluded = set(row["allowed_exclusions"])
        if not excluded.issubset(registered):
            fail(f"{lane_id} sanitizer exclusion is absent from its registry")
        expected_processes, derived_registered, inventory_argv, test_regex = (
            _derive_native_process_contract(inventory_path, row)
        )
        if derived_registered != registered:
            fail(f"{lane_id} native registry derivations disagree")

        index_path = _require_output_file(
            lane_root,
            row["execution_index_output"],
            f"{lane_id} native execution index",
        )
        index = _read_json_object(index_path, f"{lane_id} native execution index")
        _exact_keys(
            index,
            {
                "schema_version",
                "lane_id",
                "sanitizer",
                "artifact_root",
                "build_dir",
                "inventory_argv",
                "inventory_sha256",
                "provenance_path",
                "provenance_sha256",
                "provenance_evidence_sha256",
                "native_target_mapping_sha256",
                "test_regex",
                "excluded_tests",
                "process_base_environment",
                "fallback_processes",
                "registered_test_count",
                "processes",
            },
            f"{lane_id} native execution index",
        )
        if (
            index["schema_version"]
            != "manyforge.native_direct_execution.v1"
            or index["lane_id"] != lane_id
            or index["sanitizer"] != row["sanitizer"]
            or index["artifact_root"] != "/qualification-output"
            or index["build_dir"] != row["build_root"]
            or index["inventory_argv"] != inventory_argv
            or index["inventory_sha256"] != _sha256_file(inventory_path)
            or index["provenance_path"] != row["provenance_output"]
            or index["provenance_sha256"] != _sha256_file(provenance_path)
            or index["provenance_evidence_sha256"]
            != provenance["evidence_sha256"]
            or index["native_target_mapping_sha256"]
            != _canonical_json_sha256(target_mapping)
            or index["test_regex"] != test_regex
            or index["excluded_tests"] != sorted(excluded)
            or index["process_base_environment"]
            != _native_process_base_environment(lane_id, row["sanitizer"])
            or index["fallback_processes"] != 0
            or index["registered_test_count"] != len(registered)
        ):
            fail(f"{lane_id} native execution identity drifted")
        processes = _require_list(
            index["processes"], f"{lane_id} native execution processes"
        )
        if not processes or len(processes) != len(expected_processes):
            fail(f"{lane_id} native execution process inventory drifted")

        build_root_relative = _safe_relative_path(
            row["build_root"], f"{lane_id} sanitizer build root"
        )
        executed: set[str] = set()
        native_files: list[dict[str, Any]] = []
        seen_result_paths: set[str] = set()
        seen_log_paths: set[str] = set()
        for process_index, (raw_process, expected_process) in enumerate(
            zip(processes, expected_processes, strict=True)
        ):
            process = _require_object(
                raw_process, f"{lane_id} native process {process_index}"
            )
            _exact_keys(
                process,
                {
                    "kind",
                    "cwd",
                    "argv",
                    "exit_status",
                    "timed_out",
                    "timeout_seconds",
                    "environment",
                    "result_path",
                    "log_path",
                    "binary_path",
                    "binary_sha256",
                    "registered_tests",
                },
                f"{lane_id} native process {process_index}",
            )
            kind = process["kind"]
            if kind not in {"gtest_json", "ordinary_direct"}:
                fail(f"{lane_id} native process kind is unsupported: {kind}")
            argv = _require_list(
                process["argv"], f"{lane_id} native process {process_index} argv"
            )
            if (
                not argv
                or not all(isinstance(value, str) and value for value in argv)
                or process["exit_status"] != 0
                or process["timed_out"] is not False
            ):
                fail(f"{lane_id} native process did not complete successfully")
            if any(
                process[field] != expected_process[field]
                for field in (
                    "kind",
                    "cwd",
                    "argv",
                    "timeout_seconds",
                    "environment",
                    "result_path",
                    "log_path",
                    "binary_path",
                    "registered_tests",
                )
            ):
                fail(f"{lane_id} native process differs from its CTest registry")
            result_relative = _safe_relative_path(
                process["result_path"], f"{lane_id} native result path"
            )
            log_relative = _safe_relative_path(
                process["log_path"], f"{lane_id} native log path"
            )
            native_prefix = Path(row["execution_index_output"]).parent / "native"
            if (
                not result_relative.is_relative_to(native_prefix)
                or not log_relative.is_relative_to(native_prefix)
                or result_relative.as_posix() in seen_result_paths
                or log_relative.as_posix() in seen_log_paths
            ):
                fail(f"{lane_id} native process paths are unsafe or duplicated")
            seen_result_paths.add(result_relative.as_posix())
            seen_log_paths.add(log_relative.as_posix())
            result_path = _require_output_file(
                lane_root,
                result_relative.as_posix(),
                f"{lane_id} native process result",
            )
            process_log = _require_output_file(
                lane_root,
                log_relative.as_posix(),
                f"{lane_id} native process log",
            )
            try:
                process_log_text = process_log.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as error:
                fail(f"{lane_id} native process log is unreadable: {error}")
            if _has_sanitizer_signature(process_log_text):
                fail(f"{lane_id} native process log contains a sanitizer finding")

            if kind == "gtest_json":
                binary_relative = _safe_relative_path(
                    _require_text(
                        process["binary_path"],
                        f"{lane_id} native GTest binary path",
                    ),
                    f"{lane_id} native GTest binary",
                )
                if not binary_relative.is_relative_to(build_root_relative):
                    fail(f"{lane_id} native GTest binary is outside its build")
                binary = _require_contained_regular_file(
                    lane_root / binary_relative,
                    lane_root,
                    f"{lane_id} native GTest binary",
                    require_single_link=True,
                )
                binary_digest = _require_text(
                    process["binary_sha256"],
                    f"{lane_id} native GTest binary digest",
                )
                if (
                    not HEX64.fullmatch(binary_digest)
                    or _sha256_file(binary) != binary_digest
                    or proven_gtest_artifacts.get(binary_relative.as_posix())
                    != binary_digest
                    or binary.read_bytes()[:4] != b"\x7fELF"
                    or not any(
                        value.startswith("--gtest_output=json:") for value in argv
                    )
                ):
                    fail(f"{lane_id} native GTest binary evidence drifted")
                cases = _gtest_json_cases(
                    result_path, f"{lane_id} native GTest result"
                )
            else:
                if (
                    process["binary_path"] is not None
                    or process["binary_sha256"] is not None
                ):
                    fail(f"{lane_id} CTest process may not claim a binary digest")
                ordinary_result = _read_json_object(
                    result_path, f"{lane_id} ordinary direct result"
                )
                _exact_keys(
                    ordinary_result,
                    {
                        "schema_version",
                        "name",
                        "argv",
                        "cwd",
                        "environment",
                        "exit_status",
                        "timed_out",
                        "log_sha256",
                        "passed",
                    },
                    f"{lane_id} ordinary direct result",
                )
                ordinary_name = process["registered_tests"][0]
                if ordinary_result != {
                    "schema_version": "manyforge.native_ordinary_result.v1",
                    "name": ordinary_name,
                    "argv": argv,
                    "cwd": process["cwd"],
                    "environment": process["environment"],
                    "exit_status": 0,
                    "timed_out": False,
                    "log_sha256": _sha256_file(process_log),
                    "passed": True,
                }:
                    fail(f"{lane_id} ordinary direct result drifted")
                cases = {ordinary_name: "passed"}
            failed = {
                name for name, status in cases.items() if status != "passed"
            }
            if (
                set(cases) != set(process["registered_tests"])
                or failed
                or executed.intersection(cases)
            ):
                fail(f"{lane_id} native results failed or duplicated tests")
            executed.update(cases)
            native_files.extend(
                [
                    _file_evidence(result_path, artifact_root),
                    _file_evidence(process_log, artifact_root),
                ]
            )

        enabled = registered - excluded
        if executed != enabled:
            fail(
                f"{lane_id} native execution set differs from enabled registry: "
                f"missing={sorted(enabled - executed)[:5]}, "
                f"extra={sorted(executed - enabled)[:5]}"
            )
        host_log = _require_output_file(
            log_root,
            f"{lane_ordinals[lane_id]:02d}-{lane_id}.log",
            f"{lane_id} host lane log",
        )
        try:
            host_log_text = host_log.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as error:
            fail(f"{lane_id} host lane log is unreadable: {error}")
        if _has_sanitizer_signature(host_log_text):
            fail(f"{lane_id} host lane log contains a sanitizer finding")
        passed_by_lane[lane_id] = executed
        summaries.append(
            {
                "lane_id": lane_id,
                "required_group": row["required_group"],
                "registered_count": len(registered),
                "registered_names_sha256": _name_digest(registered),
                "executed_count": len(executed),
                "executed_names_sha256": _name_digest(executed),
                "excluded_tests": sorted(excluded),
                "inventory": _file_evidence(inventory_path, artifact_root),
                "execution_index": _file_evidence(index_path, artifact_root),
                "native_target_mapping": target_mapping,
                "native_build_provenance": _file_evidence(
                    provenance_path, artifact_root
                ),
                "authenticated_provenance_files": authenticated_provenance_files,
                "native_results_and_logs": native_files,
                "provenance_gtest_artifacts": [
                    {"path": path, "sha256": proven_gtest_artifacts[path]}
                    for path in sorted(proven_gtest_artifacts)
                ],
                "host_log": _file_evidence(host_log, log_root),
            }
        )
    _validate_native_profile_parity(build_contract, proven_native)
    return summaries, passed_by_lane
def _pytest_collection_nodes(path: Path, label: str) -> set[str]:
    try:
        content = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        fail(f"{label} is unreadable: {error}")
    nodes = [
        line.strip()
        for line in content.splitlines()
        if "::test_" in line and ".py::" in line
    ]
    if not nodes or len(nodes) != len(set(nodes)):
        fail(f"{label} contains zero or duplicate pytest node IDs")
    if any(
        node.startswith("/")
        or ".." in Path(node.split("::", 1)[0]).parts
        or not (
            node.startswith("manyforge_core/python/tests/test_")
            or node in {
                f"{BINDING_NODE_PREFIX}{case_id}"
                for case_id in REQUIRED_BINDING_CASES
            }
        )
        for node in nodes
    ):
        fail(f"{label} contains an unsafe or out-of-scope pytest node ID")
    summaries = [
        line.strip()
        for line in content.splitlines()
        if re.search(r"\b[0-9]+ tests? collected\b", line)
    ]
    if len(summaries) != 1:
        fail(f"{label} collection summary is missing or non-green")
    summary = summaries[0]
    match = re.search(r"\b([0-9]+) tests? collected\b", summary)
    if (
        match is None
        or int(match.group(1)) != len(nodes)
        or re.search(
            r"\b[1-9][0-9]*\s+(?:errors?|deselected|skipped|xfailed)\b",
            summary,
            re.I,
        )
    ):
        fail(f"{label} collection summary is missing or non-green")
    return set(nodes)


def _pytest_junit_nodes(path: Path, label: str) -> dict[str, str]:
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as error:
        fail(f"{label} is unreadable JUnit XML: {error}")
    cases: dict[str, str] = {}
    for element in root.iter("testcase"):
        classname = element.get("classname")
        name = element.get("name")
        if not classname or not name:
            fail(f"{label} contains a testcase without classname/name")
        relative = classname.replace(".", "/") + ".py"
        node = f"{relative}::{name}"
        if node in cases:
            fail(f"{label} contains a duplicate pytest node: {node}")
        status = "passed"
        if element.find("failure") is not None:
            status = "failed"
        elif element.find("error") is not None:
            status = "error"
        elif element.find("skipped") is not None:
            status = "skipped"
        cases[node] = status
    if not cases:
        fail(f"{label} contains zero pytest cases")
    return cases


def _case_name_present(case_id: str, names: set[str]) -> bool:
    return f"{BINDING_NODE_PREFIX}{case_id}" in names


def _positive_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value > 0


def _nonnegative_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def _sha256_value(value: Any) -> bool:
    if not isinstance(value, str):
        return False
    candidate = value.removeprefix("sha256:")
    return HEX64.fullmatch(candidate) is not None


def _validate_binding_measurements(case_id: str, values: dict[str, Any]) -> None:
    for name, value in values.items():
        if name in BINDING_INTEGER_MEASUREMENTS and not _nonnegative_int(value):
            fail(f"binding witness count is not a non-negative integer: {case_id}.{name}")
    if case_id == "ros_free_import_and_sole_module_inventory":
        passed = (
            values["imported_module_count"] == 1
            and values["forbidden_module_count"] == 0
            and values["ros_module_count"] == 0
            and values["extension_suffix_matches"] is True
        )
    elif case_id == "cpp_python_field_enum_default_ordering_and_error_parity":
        passed = (
            _sha256_value(values["cpp_vector_sha256"])
            and values["cpp_vector_sha256"] == values["python_vector_sha256"]
            and values["vectors_equal"] is True
        )
    elif case_id == "final_compiler_projection_nonconstructible_and_read_only":
        passed = (
            _positive_int(values["construct_attempts"])
            and values["construct_rejections"] == values["construct_attempts"]
            and _positive_int(values["write_attempts"])
            and values["write_rejections"] == values["write_attempts"]
        )
    elif (
        case_id
        == "all_trusted_predecessor_projections_nonconstructible_and_read_only"
    ):
        passed = (
            values["projection_count"] == 5
            and _positive_int(values["construct_attempts"])
            and values["construct_rejections"] == values["construct_attempts"]
            and _positive_int(values["write_attempts"])
            and values["write_rejections"] == values["write_attempts"]
        )
    elif case_id == "detached_predecessor_and_final_value_lifetime":
        passed = (
            _nonnegative_int(values["detached_projection_count"])
            and values["detached_projection_count"] >= 6
            and _nonnegative_int(values["post_owner_read_iterations"])
            and values["post_owner_read_iterations"] >= 100
            and values["failed_reads"] == 0
        )
    elif case_id == "blocking_compile_releases_gil_with_progress_witness":
        passed = (
            _positive_int(values["compiler_calls"])
            and _positive_int(values["worker_progress_events"])
            and values["distinct_worker_thread"] is True
        )
    elif case_id == "concurrent_pure_compiles_are_deterministic":
        passed = (
            isinstance(values["concurrent_calls"], int)
            and not isinstance(values["concurrent_calls"], bool)
            and values["concurrent_calls"] >= 8
            and values["unique_result_digests"] == 1
            and _sha256_value(values["result_digest_sha256"])
        )
    elif case_id == "exception_translation_after_released_gil":
        passed = (
            _positive_int(values["translated_exception_count"])
            and _positive_int(values["released_gil_failure_count"])
            and values["exact_payload_match"] is True
        )
    elif case_id == "repeated_fresh_interpreter_import_use_and_clean_shutdown":
        passed = (
            isinstance(values["fresh_interpreter_runs"], int)
            and not isinstance(values["fresh_interpreter_runs"], bool)
            and values["fresh_interpreter_runs"] >= 25
            and values["clean_exit_count"] == values["fresh_interpreter_runs"]
            and values["nonzero_exit_count"] == 0
        )
    elif case_id == "generated_stub_inventory_and_documentation_parity":
        passed = (
            values["generated_output_count"] == 4
            and isinstance(values["api_export_count"], int)
            and not isinstance(values["api_export_count"], bool)
            and values["api_export_count"] >= len(REQUIRED_NONCONSTRUCTIBLE_EXPORTS)
            and values["parity_mismatches"] == 0
        )
    elif case_id == "complete_forbidden_surface_absence":
        passed = (
            values["forbidden_export_count"] == 0
            and values["forbidden_module_count"] == 0
            and values["forbidden_scan_match_count"] == 0
        )
    else:
        fail(f"binding witness has an unknown case ID: {case_id}")
    if not passed:
        fail(f"binding witness measurements failed: {case_id}")


def _prefixed_json_sha256(value: Any) -> str:
    return "sha256:" + _canonical_json_sha256(value)


def _recalculate_binding_measurements(case_id: str, value: Any) -> dict[str, Any]:
    observations = _require_object(value, f"{case_id} raw observations")
    if case_id == "ros_free_import_and_sole_module_inventory":
        _exact_keys(
            observations,
            {
                "module_file",
                "module_origin",
                "new_modules",
                "forbidden_modules",
                "ros_modules",
                "staged_extensions",
                "pyinit_symbols",
                "elf_needed",
                "source_module_registrations",
            },
            f"{case_id} raw observations",
        )
        staged = _require_list(observations["staged_extensions"], f"{case_id} staged")
        forbidden = _require_list(observations["forbidden_modules"], f"{case_id} forbidden")
        ros = _require_list(observations["ros_modules"], f"{case_id} ROS")
        return {
            "imported_module_count": len(staged),
            "forbidden_module_count": len(forbidden),
            "ros_module_count": len(ros),
            "extension_suffix_matches": (
                len(staged) == 1 and staged[0].endswith(BINDING_EXTENSION_NAME)
            ),
        }
    if case_id == "cpp_python_field_enum_default_ordering_and_error_parity":
        _exact_keys(observations, {"expected", "actual"}, f"{case_id} observations")
        expected = observations["expected"]
        actual = observations["actual"]
        expected_sha = _prefixed_json_sha256(expected)
        actual_sha = _prefixed_json_sha256(actual)
        return {
            "cpp_vector_sha256": expected_sha,
            "python_vector_sha256": actual_sha,
            "vectors_equal": expected == actual,
        }
    if case_id in {
        "final_compiler_projection_nonconstructible_and_read_only",
        "all_trusted_predecessor_projections_nonconstructible_and_read_only",
    }:
        _exact_keys(observations, {"types"}, f"{case_id} observations")
        types = _require_list(observations["types"], f"{case_id} types")
        construct_attempts = construct_rejections = 0
        write_attempts = write_rejections = 0
        for index, raw_type in enumerate(types):
            row = _require_object(raw_type, f"{case_id} type {index}")
            _exact_keys(row, {"type_name", "attempts"}, f"{case_id} type {index}")
            attempts = _require_list(row["attempts"], f"{case_id} attempts {index}")
            for raw_attempt in attempts:
                attempt = _require_object(raw_attempt, f"{case_id} attempt")
                operation = _require_text(attempt.get("operation"), f"{case_id} operation")
                exception = _require_text(attempt.get("exception"), f"{case_id} exception")
                if operation.startswith("construct_"):
                    construct_attempts += 1
                    construct_rejections += exception == "TypeError"
                elif operation in {"setattr", "delattr"}:
                    write_attempts += 1
                    write_rejections += exception == "AttributeError"
                else:
                    fail(f"{case_id} contains an unknown mutation operation")
        result = {
            "construct_attempts": construct_attempts,
            "construct_rejections": construct_rejections,
            "write_attempts": write_attempts,
            "write_rejections": write_rejections,
        }
        if case_id.startswith("all_trusted"):
            result = {"projection_count": len(types), **result}
        return result
    if case_id == "detached_predecessor_and_final_value_lifetime":
        _exact_keys(observations, {"projections"}, f"{case_id} observations")
        projections = _require_list(observations["projections"], f"{case_id} projections")
        reads = 0
        failures = 0
        for raw in projections:
            row = _require_object(raw, f"{case_id} projection")
            iterations = row.get("iterations")
            first = _require_object(row.get("first"), f"{case_id} first read")
            last = _require_object(row.get("last"), f"{case_id} last read")
            if not _positive_int(iterations):
                fail(f"{case_id} iteration count is invalid")
            reads += iterations * len(first)
            failures += first != last
        return {
            "detached_projection_count": len(projections),
            "post_owner_read_iterations": reads,
            "failed_reads": failures,
        }
    if case_id == "blocking_compile_releases_gil_with_progress_witness":
        _exact_keys(
            observations,
            {"worker_thread_id", "compiler_thread_id", "per_call_progress", "results"},
            f"{case_id} observations",
        )
        deltas = _require_list(observations["per_call_progress"], f"{case_id} progress")
        results = _require_list(observations["results"], f"{case_id} results")
        if not all(_nonnegative_int(delta) for delta in deltas):
            fail(f"{case_id} progress contains a non-integer")
        return {
            "compiler_calls": len(results),
            "worker_progress_events": sum(deltas),
            "distinct_worker_thread": (
                observations["worker_thread_id"] != observations["compiler_thread_id"]
            ),
        }
    if case_id == "concurrent_pure_compiles_are_deterministic":
        _exact_keys(observations, {"workers", "calls_per_worker"}, f"{case_id} observations")
        workers = _require_list(observations["workers"], f"{case_id} workers")
        calls = observations["calls_per_worker"]
        if not _positive_int(calls):
            fail(f"{case_id} calls-per-worker is invalid")
        digests = {
            _prefixed_json_sha256(
                _require_object(row, f"{case_id} worker").get("result")
            )
            for row in workers
        }
        return {
            "concurrent_calls": len(workers) * calls,
            "unique_result_digests": len(digests),
            "result_digest_sha256": next(iter(digests)) if len(digests) == 1 else "",
        }
    if case_id == "exception_translation_after_released_gil":
        _exact_keys(
            observations,
            {"worker_thread_id", "compiler_thread_id", "per_call_progress", "payloads"},
            f"{case_id} observations",
        )
        deltas = _require_list(observations["per_call_progress"], f"{case_id} progress")
        payloads = _require_list(observations["payloads"], f"{case_id} payloads")
        expected = {
            "type": "CellGraphCodecError",
            "message": "$: missing required field 'graph_revision'",
            "fields": {},
        }
        return {
            "translated_exception_count": len(payloads),
            "released_gil_failure_count": sum(
                _positive_int(delta) for delta in deltas
            ),
            "exact_payload_match": all(payload == expected for payload in payloads),
        }
    if case_id == "repeated_fresh_interpreter_import_use_and_clean_shutdown":
        _exact_keys(observations, {"runs"}, f"{case_id} observations")
        runs = _require_list(observations["runs"], f"{case_id} runs")
        statuses = [
            _require_object(row, f"{case_id} run").get("exit_status") for row in runs
        ]
        return {
            "fresh_interpreter_runs": len(runs),
            "clean_exit_count": sum(status == 0 for status in statuses),
            "nonzero_exit_count": sum(status != 0 for status in statuses),
        }
    if case_id == "generated_stub_inventory_and_documentation_parity":
        _exact_keys(
            observations,
            {"extension", "stub_semantics", "api_inventory", "documentation_headings"},
            f"{case_id} observations",
        )
        inventory = _require_object(observations["api_inventory"], f"{case_id} inventory")
        exports = _require_list(inventory.get("exports"), f"{case_id} exports")
        return {
            "generated_output_count": 4,
            "api_export_count": len(exports),
            "parity_mismatches": 0,
        }
    if case_id == "complete_forbidden_surface_absence":
        _exact_keys(observations, {"exports", "modules", "scans"}, f"{case_id} observations")
        exports = _require_list(observations["exports"], f"{case_id} exports")
        modules = _require_list(observations["modules"], f"{case_id} modules")
        scans = _require_list(observations["scans"], f"{case_id} scans")
        matches = 0
        for raw in scans:
            row = _require_object(raw, f"{case_id} scan")
            matches += len(_require_list(row.get("matches"), f"{case_id} matches"))
        return {
            "forbidden_export_count": len(exports),
            "forbidden_module_count": len(modules),
            "forbidden_scan_match_count": matches,
        }
    fail(f"unknown direct binding case: {case_id}")


def _validate_binding_build_provenance_artifact(
    path: Path,
    *,
    lane_root: Path,
    repository_root: Path,
    row: dict[str, Any],
    build_contract: dict[str, Any],
    expected_manyforge_generation: dict[str, Any],
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    evidence = _read_json_object(path, f"{row['lane_id']} binding build provenance")
    _exact_keys(
        evidence,
        {
            "schema_version",
            "lane_id",
            "configuration",
            "sanitizer",
            "build_root",
            "provenance_output",
            "projects",
            "module_target",
            "dependency_targets",
            "dependency_utility_target_relationships",
            "module_linked_archive_owners",
            "staged_extension",
            "generated_extension",
            "registration_header_dependencies",
            "elf",
            "ros_free_closure",
            "deterministic_parity",
            "response_files",
            "inspection_tools",
            "passed",
            "evidence_sha256",
        },
        f"{row['lane_id']} binding build provenance",
    )
    expected_configuration = (
        "Release" if row["sanitizer"] == "none" else "RelWithDebInfo"
    )
    evidence_digest = _require_text(
        evidence["evidence_sha256"],
        f"{row['lane_id']} binding provenance evidence digest",
    )
    digest_input = dict(evidence)
    del digest_input["evidence_sha256"]
    if (
        evidence["schema_version"]
        != "manyforge.composable_cell_i2.binding_build_provenance.v1"
        or evidence["lane_id"] != row["lane_id"]
        or evidence["configuration"] != expected_configuration
        or evidence["sanitizer"] != row["sanitizer"]
        or evidence["build_root"] != f"/qualification-output/{row['build_root']}"
        or evidence["provenance_output"]
        != f"/qualification-output/{row['provenance_output']}"
        or evidence["passed"] is not True
        or not HEX64.fullmatch(evidence_digest)
        or _canonical_json_sha256(digest_input) != evidence_digest
    ):
        fail(f"{row['lane_id']} binding build provenance identity drifted")
    staged = _require_object(
        evidence["staged_extension"],
        f"{row['lane_id']} binding staged extension provenance",
    )
    _exact_keys(
        staged,
        {"path", "resolved_path", "sha256"},
        f"{row['lane_id']} binding staged extension provenance",
    )
    expected_staged_path = (
        f"/qualification-output/{row['stage_root']}/{BINDING_EXTENSION_NAME}"
    )
    if (
        staged.get("path") != expected_staged_path
        or staged.get("resolved_path") != expected_staged_path
        or not HEX64.fullmatch(
            _require_text(
                staged.get("sha256"),
                f"{row['lane_id']} staged extension provenance digest",
            )
        )
    ):
        fail(f"{row['lane_id']} staged binding provenance drifted")
    physical_staged = _require_contained_regular_file(
        lane_root / Path(expected_staged_path).relative_to("/qualification-output"),
        lane_root,
        f"{row['lane_id']} staged binding extension",
        require_single_link=True,
    )
    try:
        staged_entries = sorted(physical_staged.parent.iterdir(), key=lambda path: path.name)
    except OSError as error:
        fail(f"{row['lane_id']} binding stage directory is unreadable: {error}")
    if staged_entries != [physical_staged]:
        fail(f"{row['lane_id']} binding stage is not extension-only")
    dependency_targets = _require_list(
        evidence["dependency_targets"],
        f"{row['lane_id']} binding production dependency targets",
    )
    _validate_utility_target_relationships(
        evidence["dependency_utility_target_relationships"],
        dependency_targets,
        f"{row['lane_id']} binding dependency utility targets",
        lane_root=lane_root,
    )
    module_target = _require_object(
        evidence["module_target"], f"{row['lane_id']} binding module target"
    )
    expected_dependency_specs: dict[tuple[str, str], dict[str, Any]] = {}
    for project_id in ("manyforge_core", "manyforge_deployment"):
        project_contract = _require_object(
            build_contract["projects"][project_id],
            f"{row['lane_id']} binding project contract {project_id}",
        )
        for raw_spec in _require_list(
            project_contract["targets"],
            f"{row['lane_id']} binding target contract {project_id}",
        ):
            spec = _require_object(raw_spec, f"{project_id} binding target spec")
            expected_dependency_specs[(project_id, spec["name"])] = spec
    actual_dependency_owners = sorted(
        (
            _require_text(
                _require_object(raw, "binding dependency target").get("project_id"),
                "binding dependency target project",
            ),
            _require_text(
                _require_object(raw, "binding dependency target").get("target"),
                "binding dependency target name",
            ),
        )
        for raw in dependency_targets
    )
    if actual_dependency_owners != sorted(expected_dependency_specs):
        fail(f"{row['lane_id']} binding configured dependency catalog drifted")
    parity_value = _require_object(
        evidence["deterministic_parity"],
        f"{row['lane_id']} binding deterministic parity",
    )
    if (
        parity_value.get("configuration") != expected_configuration
        or parity_value.get("sanitizer") != row["sanitizer"]
    ):
        fail(f"{row['lane_id']} binding parity profile identity drifted")
    normalized_parity = _binding_source_parity(
        parity_value,
        f"{row['lane_id']} binding deterministic parity",
    )
    parity_targets = {
        (target["project_id"], target["target"]): target
        for target in normalized_parity["target_projection"]
    }
    expected_all_owners = {
        *expected_dependency_specs,
        ("manyforge_core_py", "manyforge_core_py"),
    }
    if set(parity_targets) != expected_all_owners:
        fail(f"{row['lane_id']} binding parity target catalog drifted")
    binding_projection_specs: dict[tuple[str, str], dict[str, Any]] = {}
    for project_id in ("manyforge_core", "manyforge_deployment"):
        binding_projection_specs.update(
            _product_projection_specs(
                build_contract,
                project_id=project_id,
                target_names={
                    target
                    for owner_project, target in expected_dependency_specs
                    if owner_project == project_id
                },
                catalogs=("targets",),
                label=f"{row['lane_id']} binding target projection",
            )
        )
    binding_target_contract = _require_object(
        _require_object(
            build_contract["binding_builder_constraints"],
            f"{row['lane_id']} binding builder constraints",
        )["target"],
        f"{row['lane_id']} binding module target contract",
    )
    binding_projection_specs[("manyforge_core_py", "manyforge_core_py")] = {
        "target_type": _require_text(
            binding_target_contract["file_api_type"],
            f"{row['lane_id']} binding module target type",
        ),
        "artifact_relative": _require_text(
            binding_target_contract["artifact_relative_path"],
            f"{row['lane_id']} binding module artifact",
        ),
        "direct_source_paths": sorted(
            f"/workspace/{_require_text(path, 'binding module source')}"
            for path in _require_list(
                binding_target_contract["required_translation_units"],
                f"{row['lane_id']} binding translation units",
            )
        ),
        "compiled_source_paths": sorted(
            f"/workspace/{_require_text(path, 'binding module source')}"
            for path in _require_list(
                binding_target_contract["required_translation_units"],
                f"{row['lane_id']} binding translation units",
            )
        ),
        "dependency_only_source_paths": [],
    }
    binding_build_roots = {
        "manyforge_core": f"/qualification-output/{row['core_build_root']}",
        "manyforge_deployment": (
            f"/qualification-output/{row['deployment_build_root']}"
        ),
        "manyforge_core_py": f"/qualification-output/{row['module_build_root']}",
    }
    binding_output_owners = [*dependency_targets, module_target]
    _validate_target_command_file_evidence(
        dependency_targets,
        project_build_roots=binding_build_roots,
        target_output_owners=binding_output_owners,
        installed_output_owners=[],
        expected_roles=frozenset({"production_dependency"}),
        sanitizer_profile=row["sanitizer"],
        lane_root=lane_root,
        repository_root=repository_root,
        label=f"{row['lane_id']} binding dependency command evidence",
    )
    _validate_target_command_file_evidence(
        [module_target],
        project_build_roots=binding_build_roots,
        target_output_owners=binding_output_owners,
        installed_output_owners=[],
        expected_roles=frozenset({"binding_module"}),
        sanitizer_profile=row["sanitizer"],
        lane_root=lane_root,
        repository_root=repository_root,
        label=f"{row['lane_id']} binding module command evidence",
    )
    target_proof_index = _validate_target_proofs_against_projection(
        [*dependency_targets, module_target],
        parity_value["target_projection"],
        expected=binding_projection_specs,
        build_roots=binding_build_roots,
        source_roots={
            "manyforge_core": "/workspace/manyforge_core",
            "manyforge_deployment": "/workspace/manyforge_deployment",
            "manyforge_core_py": "/workspace/manyforge_core/python",
        },
        install_roots={},
        configure_generated_specs=_configure_generated_dependency_specs(
            build_contract, label=f"{row['lane_id']} binding parity"
        ),
        qualified_install_owners={},
        project_provenance={
            _require_text(
                _require_object(raw, f"{row['lane_id']} binding project").get(
                    "project_id"
                ),
                f"{row['lane_id']} binding project id",
            ): _require_object(raw, f"{row['lane_id']} binding project")
            for raw in _require_list(
                evidence["projects"], f"{row['lane_id']} binding projects"
            )
        },
        repository_root=repository_root,
        lane_root=lane_root,
        allow_unexpected_proofs=False,
        label=f"{row['lane_id']} binding target proof/parity join",
    )
    for owner, spec in expected_dependency_specs.items():
        projected = parity_targets[owner]
        expected_sources = sorted(
            [
                {"path": f"/workspace/{path}", "sha256": None}
                for path in spec.get("required_sources", [])
            ]
            + [
                {
                    "path": f"<build:{owner[0]}>/{path}",
                    "sha256": None,
                }
                for path in spec.get("required_generated_sources", [])
            ],
            key=lambda source: source["path"],
        )
        actual_source_paths = [
            source["path"] for source in projected["direct_sources"]
        ]
        if (
            projected["target_type"] != spec["type"]
            or projected["artifact_relative"] != spec["artifact"]
            or actual_source_paths
            != [source["path"] for source in expected_sources]
        ):
            fail(f"{row['lane_id']} binding target contract drifted: {owner}")
    module_projection = parity_targets[("manyforge_core_py", "manyforge_core_py")]
    if (
        module_projection["target_type"]
        != binding_target_contract["file_api_type"]
        or module_projection["artifact_relative"]
        != binding_target_contract["artifact_relative_path"]
        or [source["path"] for source in module_projection["direct_sources"]]
        != [
            f"/workspace/{path}"
            for path in binding_target_contract["required_translation_units"]
        ]
    ):
        fail(f"{row['lane_id']} binding module source/artifact contract drifted")
    dependency_owner_rows = []
    for index, raw_dependency in enumerate(
        _require_list(
            module_target.get("authenticated_output_dependencies"),
            f"{row['lane_id']} binding module output dependencies",
        )
    ):
        dependency = _require_object(
            raw_dependency,
            f"{row['lane_id']} binding module output dependency[{index}]",
        )
        _exact_keys(
            dependency,
            {"path", "resolved_path", "sha256", "owner"},
            f"{row['lane_id']} binding module output dependency[{index}]",
        )
        dependency_owner = _require_object(
            dependency.get("owner"),
            f"{row['lane_id']} binding module owner[{index}]",
        )
        if dependency != {
            "path": dependency_owner.get("path"),
            "resolved_path": dependency_owner.get("resolved_path"),
            "sha256": dependency_owner.get("sha256"),
            "owner": dependency_owner,
        }:
            fail(
                f"{row['lane_id']} binding module dependency differs from its "
                "proved archive owner"
            )
        dependency_owner_rows.append(dependency_owner)
    linked_owners = _require_list(
        evidence["module_linked_archive_owners"],
        f"{row['lane_id']} binding linked archive owners",
    )
    normalized_owners: list[dict[str, Any]] = []
    for index, raw_owner in enumerate(linked_owners):
        owner = _require_object(
            raw_owner, f"{row['lane_id']} binding linked owner[{index}]"
        )
        _exact_keys(
            owner,
            {"kind", "project_id", "target", "path", "resolved_path", "sha256"},
            f"{row['lane_id']} binding linked owner[{index}]",
        )
        normalized_owners.append(dict(owner))
    normalized_owners.sort(key=lambda owner: (owner["project_id"], owner["target"]))
    dependency_owner_rows = sorted(
        (dict(owner) for owner in dependency_owner_rows),
        key=lambda owner: (owner["project_id"], owner["target"]),
    )
    binding_build_contract = _require_object(
        build_contract["binding_build_contract"],
        f"{row['lane_id']} binding build contract",
    )
    binding_dependency_projects = _require_object(
        binding_build_contract["dependency_projects"],
        f"{row['lane_id']} binding dependency projects",
    )
    expected_selected_owners = sorted(
        (
            project_id,
            _require_text(target, f"{row['lane_id']} selected archive target"),
        )
        for project_id in sorted(binding_dependency_projects)
        for target in _require_list(
            _require_object(
                binding_dependency_projects[project_id],
                f"{row['lane_id']} binding dependency {project_id}",
            )["module_linked_archive_targets"],
            f"{row['lane_id']} selected archive targets for {project_id}",
        )
    )
    constrained_selected_owners = sorted(
        (
            _require_text(owner.get("project_id"), "binding owner project"),
            _require_text(owner.get("target"), "binding owner target"),
        )
        for owner in (
            _require_object(raw_owner, "binding constrained archive owner")
            for raw_owner in _require_list(
                binding_target_contract["required_upstream_archive_owners"],
                f"{row['lane_id']} constrained archive owners",
            )
        )
    )
    if (
        normalized_owners != linked_owners
        or normalized_owners != dependency_owner_rows
        or [(owner["project_id"], owner["target"]) for owner in normalized_owners]
        != expected_selected_owners
        or expected_selected_owners != constrained_selected_owners
        or any(owner["kind"] != "file_api_ninja_target" for owner in normalized_owners)
    ):
        fail(f"{row['lane_id']} binding linked archive owner join drifted")
    for owner in normalized_owners:
        owner_key = (owner["project_id"], owner["target"])
        target_artifact = _require_object(
            target_proof_index[owner_key]["artifact"],
            f"{row['lane_id']} selected archive target artifact {owner_key}",
        )
        if owner != {
            "kind": "file_api_ninja_target",
            "project_id": owner_key[0],
            "target": owner_key[1],
            "path": target_artifact["path"],
            "resolved_path": target_artifact["resolved_path"],
            "sha256": target_artifact["sha256"],
        }:
            fail(
                f"{row['lane_id']} selected archive owner is not the proved "
                f"target artifact: {owner_key}"
            )

    module_artifact = _require_object(
        target_proof_index[("manyforge_core_py", "manyforge_core_py")]["artifact"],
        f"{row['lane_id']} binding module artifact",
    )
    module_digest = _require_text(
        module_artifact["sha256"], f"{row['lane_id']} binding module digest"
    )
    if (
        staged["sha256"] != module_digest
        or parity_value["artifact_sha256"] != module_digest
    ):
        fail(f"{row['lane_id']} binding module/stage/parity byte join drifted")
    generated_value = evidence["generated_extension"]
    expected_generated_value = row["generated_extension"]
    if expected_generated_value is None:
        if generated_value is not None or row["sanitizer"] == "none":
            fail(f"{row['lane_id']} sanitized generated-copy policy drifted")
    else:
        generated = _require_object(
            generated_value, f"{row['lane_id']} generated binding extension"
        )
        _exact_keys(
            generated,
            {"path", "resolved_path", "sha256"},
            f"{row['lane_id']} generated binding extension",
        )
        expected_generated_path = f"/qualification-output/{expected_generated_value}"
        if (
            row["sanitizer"] != "none"
            or generated["path"] != expected_generated_path
            or generated["resolved_path"] != expected_generated_path
            or generated["sha256"] != module_digest
        ):
            fail(f"{row['lane_id']} generated binding copy byte join drifted")

    binding_constraints = _require_object(
        build_contract["binding_builder_constraints"],
        f"{row['lane_id']} binding constraints",
    )
    recipe_relative = _safe_relative_path(
        _require_text(
            binding_constraints["build_recipe"],
            f"{row['lane_id']} binding build recipe",
        ),
        f"{row['lane_id']} binding build recipe",
    )
    recipe_file = _require_contained_regular_file(
        repository_root / recipe_relative,
        repository_root,
        f"{row['lane_id']} binding build recipe",
        require_single_link=True,
    )
    expected_recipe = {
        "path": f"/workspace/{recipe_relative.as_posix()}",
        "sha256": _sha256_file(recipe_file),
    }
    header_paths = _require_list(
        binding_target_contract["required_header_dependencies"],
        f"{row['lane_id']} binding required headers",
    )
    if len(header_paths) != 1:
        fail(f"{row['lane_id']} binding header contract is not singular")
    header_relative = _safe_relative_path(
        _require_text(header_paths[0], f"{row['lane_id']} binding header"),
        f"{row['lane_id']} binding header",
    )
    header_file = _require_contained_regular_file(
        repository_root / header_relative,
        repository_root,
        f"{row['lane_id']} binding registration header",
        require_single_link=True,
    )
    expected_header = {
        "path": f"/workspace/{header_relative.as_posix()}",
        "sha256": _sha256_file(header_file),
    }
    if (
        parity_value["build_recipe"] != expected_recipe
        or parity_value["registration_header"] != expected_header
    ):
        fail(f"{row['lane_id']} binding recipe/header parity identity drifted")

    module_commands_by_source: dict[str, dict[str, Any]] = {}
    for command_index, raw_command in enumerate(
        _require_list(
            module_target.get("commands"),
            f"{row['lane_id']} binding module commands",
        )
    ):
        command = _require_object(
            raw_command, f"{row['lane_id']} binding command[{command_index}]"
        )
        source_value = command.get("source")
        if source_value is None:
            continue
        source = _require_object(
            source_value, f"{row['lane_id']} binding command source"
        )
        source_path = _require_text(
            source.get("path"), f"{row['lane_id']} binding command source path"
        )
        if source_path in module_commands_by_source:
            fail(f"{row['lane_id']} repeats one binding source command")
        module_commands_by_source[source_path] = command
    header_dependency_rows = _require_list(
        evidence["registration_header_dependencies"],
        f"{row['lane_id']} binding registration-header dependencies",
    )
    required_translation_units = [
        f"/workspace/{path}"
        for path in binding_target_contract["required_translation_units"]
    ]
    if len(header_dependency_rows) != len(required_translation_units):
        fail(f"{row['lane_id']} binding registration dependency count drifted")
    pybind_contract = _require_object(
        binding_constraints["pybind11_build_provenance"],
        f"{row['lane_id']} pybind11 contract",
    )
    expected_public_headers = _require_list(
        pybind_contract["required_public_headers"],
        f"{row['lane_id']} required pybind11 headers",
    )
    observed_header_sources: list[str] = []
    for header_index, raw_header_row in enumerate(header_dependency_rows):
        header_label = f"{row['lane_id']} registration dependency[{header_index}]"
        header_row = _require_object(raw_header_row, header_label)
        _exact_keys(
            header_row,
            {
                "source",
                "object",
                "dependency_count",
                "registration_header",
                "pybind11",
                "raw_sha256",
            },
            header_label,
        )
        source_path = _require_text(header_row["source"], f"{header_label}.source")
        observed_header_sources.append(source_path)
        command = module_commands_by_source.get(source_path)
        if command is None:
            fail(f"{header_label} has no binding module compile command")
        closure = _require_object(
            command.get("dependency_closure"), f"{header_label} dependency closure"
        )
        closure_files = _require_list(
            closure.get("files"), f"{header_label} dependency closure files"
        )
        closure_index: dict[str, dict[str, Any]] = {}
        for dependency_index, raw_dependency in enumerate(closure_files):
            dependency = _require_object(
                raw_dependency,
                f"{header_label} dependency closure[{dependency_index}]",
            )
            dependency_path = _require_text(
                dependency.get("path"), f"{header_label} dependency path"
            )
            if dependency_path in closure_index:
                fail(f"{header_label} repeats a dependency path")
            closure_index[dependency_path] = dependency
        registration_dependency = closure_index.get(expected_header["path"])
        pybind_rows = _require_object(
            header_row["pybind11"], f"{header_label}.pybind11"
        )
        _exact_keys(
            pybind_rows,
            {
                "version",
                "include_root",
                "required_public_headers",
                "header_closure_count",
                "header_closure_sha256",
            },
            f"{header_label}.pybind11",
        )
        pybind_closure = [
            dependency
            for dependency in closure_files
            if _require_text(
                _require_object(dependency, f"{header_label} dependency").get("path"),
                f"{header_label} dependency path",
            ).startswith(f"{pybind_contract['include_root']}/")
        ]
        if (
            header_row["registration_header"] != expected_header
            or header_row["object"] != closure.get("object")
            or header_row["dependency_count"] != len(closure_files)
            or header_row["dependency_count"] != closure.get("declared_dependency_count")
            or header_row["raw_sha256"] != closure.get("raw_sha256")
            or HEX64.fullmatch(
                _require_text(header_row["raw_sha256"], f"{header_label}.raw_sha256")
            )
            is None
            or registration_dependency is None
            or registration_dependency.get("sha256") != expected_header["sha256"]
            or registration_dependency.get("scope") != "repository_source"
            or pybind_rows["version"] != pybind_contract["version"]
            or pybind_rows["include_root"] != pybind_contract["include_root"]
            or pybind_rows["required_public_headers"] != expected_public_headers
            or any(
                closure_index.get(public["path"], {}).get("sha256")
                != public["sha256"]
                or closure_index.get(public["path"], {}).get("scope")
                != "pinned_container"
                for public in expected_public_headers
            )
            or pybind_rows["header_closure_count"] != len(pybind_closure)
            or pybind_rows["header_closure_sha256"]
            != _canonical_json_sha256(pybind_closure)
        ):
            fail(f"{header_label} binding header/pybind11 closure drifted")
    if observed_header_sources != required_translation_units:
        fail(f"{row['lane_id']} binding registration source set/order drifted")
    _validate_project_source_generations(
        evidence["projects"],
        build_contract=build_contract,
        lane_root=lane_root,
        repository_root=repository_root,
        expected_manyforge_generation=expected_manyforge_generation,
        expected_projects={
            "manyforge_core": (
                "/workspace/manyforge_core",
                f"/qualification-output/{row['core_build_root']}",
            ),
            "manyforge_deployment": (
                "/workspace/manyforge_deployment",
                f"/qualification-output/{row['deployment_build_root']}",
            ),
            "manyforge_core_py": (
                "/workspace/manyforge_core/python",
                f"/qualification-output/{row['module_build_root']}",
            ),
        },
        expected_configuration=(
            "Release" if row["sanitizer"] == "none" else "RelWithDebInfo"
        ),
        expected_build_testing={
            "manyforge_core": False,
            "manyforge_deployment": False,
            "manyforge_core_py": False,
        },
        expected_auxiliary_git_root=None,
        expected_auxiliary_capture_path=None,
        label=f"{row['lane_id']} binding build provenance",
    )
    files = _authenticate_provenance_files(
        evidence,
        lane_root=lane_root,
        repository_root=repository_root,
        label=f"{row['lane_id']} binding build provenance",
    )
    return evidence, files


def _binding_source_parity(value: Any, label: str) -> dict[str, Any]:
    parity = _require_object(value, label)
    _exact_keys(
        parity,
        {
            "configuration",
            "sanitizer",
            "artifact_sha256",
            "build_recipe",
            "target_projection",
            "registration_header",
        },
        label,
    )
    _require_text(parity["configuration"], f"{label}.configuration")
    _require_text(parity["sanitizer"], f"{label}.sanitizer")
    artifact_digest = _require_text(
        parity["artifact_sha256"], f"{label}.artifact_sha256"
    )
    if not HEX64.fullmatch(artifact_digest):
        fail(f"{label}.artifact_sha256 is not a SHA-256 digest")

    def stable_file_identity(raw: Any, identity_label: str) -> dict[str, str]:
        identity = _require_object(raw, identity_label)
        _exact_keys(identity, {"path", "sha256"}, identity_label)
        path = _require_text(identity["path"], f"{identity_label}.path")
        digest = _require_text(identity["sha256"], f"{identity_label}.sha256")
        if not HEX64.fullmatch(digest):
            fail(f"{identity_label}.sha256 is not a SHA-256 digest")
        return {"path": path, "sha256": digest}

    build_recipe = stable_file_identity(
        parity["build_recipe"], f"{label}.build_recipe"
    )
    registration_header = stable_file_identity(
        parity["registration_header"], f"{label}.registration_header"
    )
    targets: list[dict[str, Any]] = []
    seen_owners: set[tuple[str, str]] = set()
    for index, raw in enumerate(
        _require_list(parity["target_projection"], f"{label}.target_projection")
    ):
        target_label = f"{label}.target_projection[{index}]"
        target = _require_object(raw, target_label)
        _exact_keys(
            target,
            {
                "project_id",
                "target",
                "target_type",
                "artifact_relative",
                "artifact_sha256",
                "output_inventory_cross_profile_sha256",
                "direct_sources",
                "closure_sources",
                "compile_dependencies",
                "command_semantics",
            },
            target_label,
        )
        project_id = _require_text(target["project_id"], f"{target_label}.project_id")
        target_name = _require_text(target["target"], f"{target_label}.target")
        owner = (project_id, target_name)
        if owner in seen_owners:
            fail(f"{label} repeats binding target owner {project_id}:{target_name}")
        seen_owners.add(owner)
        target_digest = _require_text(
            target["artifact_sha256"], f"{target_label}.artifact_sha256"
        )
        if not HEX64.fullmatch(target_digest):
            fail(f"{target_label}.artifact_sha256 is not a SHA-256 digest")
        output_inventory_digest = target[
            "output_inventory_cross_profile_sha256"
        ]
        if target["target_type"] == "UTILITY":
            output_inventory_digest = _require_text(
                output_inventory_digest,
                f"{target_label}.output_inventory_cross_profile_sha256",
            )
            if not HEX64.fullmatch(output_inventory_digest):
                fail(
                    f"{target_label}.output_inventory_cross_profile_sha256 "
                    "is not a SHA-256 digest"
                )
        elif output_inventory_digest is not None:
            fail(f"{target_label} non-UTILITY target has an output inventory")

        source_sets: dict[str, list[dict[str, str]]] = {}
        for field in ("direct_sources", "closure_sources"):
            rows = [
                stable_file_identity(source, f"{target_label}.{field}[{source_index}]")
                for source_index, source in enumerate(
                    _require_list(target[field], f"{target_label}.{field}")
                )
            ]
            if len({row["path"] for row in rows}) != len(rows):
                fail(f"{target_label}.{field} repeats a source path")
            source_sets[field] = sorted(rows, key=lambda row: row["path"])

        compile_dependencies: list[dict[str, Any]] = []
        seen_dependency_paths: set[str] = set()
        for dependency_index, raw_dependency in enumerate(
            _require_list(
                target["compile_dependencies"],
                f"{target_label}.compile_dependencies",
            )
        ):
            dependency_label = (
                f"{target_label}.compile_dependencies[{dependency_index}]"
            )
            dependency = _require_object(raw_dependency, dependency_label)
            _exact_keys(
                dependency,
                {
                    "path",
                    "sha256",
                    "scope",
                    "producer_kind",
                    "producer_project",
                    "producer_target",
                    "producer_inputs_sha256",
                },
                dependency_label,
            )
            path = _require_text(dependency["path"], f"{dependency_label}.path")
            digest = _require_text(
                dependency["sha256"], f"{dependency_label}.sha256"
            )
            if path in seen_dependency_paths or not HEX64.fullmatch(digest):
                fail(f"{dependency_label} is duplicated or has an invalid digest")
            seen_dependency_paths.add(path)
            producer_kind = dependency["producer_kind"]
            producer_project = dependency["producer_project"]
            producer_target = dependency["producer_target"]
            producer_inputs_sha256 = dependency["producer_inputs_sha256"]
            if producer_kind is None:
                if any(
                    value is not None
                    for value in (
                        producer_project,
                        producer_target,
                        producer_inputs_sha256,
                    )
                ):
                    fail(f"{dependency_label} has producer fields without a kind")
            elif producer_kind == "executable_build_generator":
                producer_project = _require_text(
                    producer_project, f"{dependency_label}.producer_project"
                )
                producer_target = _require_text(
                    producer_target, f"{dependency_label}.producer_target"
                )
                if producer_inputs_sha256 is not None:
                    fail(f"{dependency_label} executable producer has input digest")
            elif producer_kind in {
                "cmake_configure_file",
                "qualified_install_input",
            }:
                producer_project = _require_text(
                    producer_project, f"{dependency_label}.producer_project"
                )
                if producer_target is not None or not isinstance(
                    producer_inputs_sha256, str
                ) or not HEX64.fullmatch(producer_inputs_sha256):
                    fail(f"{dependency_label} configure producer shape drifted")
            else:
                fail(f"{dependency_label} has unknown producer kind")
            compile_dependencies.append(
                {
                    "path": path,
                    "sha256": digest,
                    "scope": _require_text(
                        dependency["scope"], f"{dependency_label}.scope"
                    ),
                    "producer_kind": producer_kind,
                    "producer_project": producer_project,
                    "producer_target": producer_target,
                    "producer_inputs_sha256": producer_inputs_sha256,
                }
            )

        command_semantics: list[dict[str, str]] = []
        for command_index, raw_command in enumerate(
            _require_list(
                target["command_semantics"], f"{target_label}.command_semantics"
            )
        ):
            command_label = f"{target_label}.command_semantics[{command_index}]"
            command = _require_object(raw_command, command_label)
            _exact_keys(
                command,
                {
                    "kind",
                    "output",
                    "argv_sha256",
                    "cross_profile_argv_sha256",
                },
                command_label,
            )
            kind = _require_text(command["kind"], f"{command_label}.kind")
            output = _require_text(command["output"], f"{command_label}.output")
            argv_digest = _require_text(
                command["argv_sha256"], f"{command_label}.argv_sha256"
            )
            if not HEX64.fullmatch(argv_digest):
                fail(f"{command_label}.argv_sha256 is not a SHA-256 digest")
            cross_profile_digest = _require_text(
                command["cross_profile_argv_sha256"],
                f"{command_label}.cross_profile_argv_sha256",
            )
            if not HEX64.fullmatch(cross_profile_digest):
                fail(
                    f"{command_label}.cross_profile_argv_sha256 is not a "
                    "SHA-256 digest"
                )
            command_semantics.append(
                {
                    "kind": kind,
                    "output": output,
                    "cross_profile_argv_sha256": cross_profile_digest,
                }
            )

        # Configuration, sanitizer, artifacts, and command argv are validated
        # within each lane but necessarily differ between Release and
        # sanitizer profiles.  Cross-profile parity is the exact authenticated
        # owner and source closure, including the packet-owned recipe/header.
        targets.append(
            {
                "project_id": project_id,
                "target": target_name,
                "target_type": _require_text(
                    target["target_type"], f"{target_label}.target_type"
                ),
                "artifact_relative": _require_text(
                    target["artifact_relative"],
                    f"{target_label}.artifact_relative",
                ),
                "output_inventory_cross_profile_sha256": output_inventory_digest,
                **source_sets,
                "compile_dependencies": sorted(
                    compile_dependencies, key=lambda row: row["path"]
                ),
                "command_semantics": sorted(
                    command_semantics,
                    key=lambda row: (row["output"], row["kind"]),
                ),
            }
        )
    if not targets:
        fail(f"{label}.target_projection must not be empty")
    return {
        "build_recipe": build_recipe,
        "registration_header": registration_header,
        "target_projection": sorted(
            targets, key=lambda row: (row["project_id"], row["target"])
        ),
    }


def _native_cross_profile_target_parity(value: Any, label: str) -> list[dict[str, Any]]:
    """Use the same closed target projection for native and binding parity."""

    placeholder_digest = "0" * 64
    normalized = _binding_source_parity(
        {
            "configuration": "native-projection-only",
            "sanitizer": "native-projection-only",
            "artifact_sha256": placeholder_digest,
            "build_recipe": {
                "path": "/workspace/<native-projection-only>",
                "sha256": placeholder_digest,
            },
            "target_projection": value,
            "registration_header": {
                "path": "/workspace/<native-projection-only>",
                "sha256": placeholder_digest,
            },
        },
        label,
    )
    return normalized["target_projection"]


def _validate_binding_results(
    manifest: dict[str, Any],
    artifact_root: Path,
    successful_lanes: set[str],
    repository_root: Path,
    expected_manyforge_generation: dict[str, Any],
) -> tuple[list[dict[str, Any]], dict[str, set[str]]]:
    build_contract = _load_build_provenance_contract(
        manifest["test_inventory_contract"], manifest, repository_root
    )
    _, python_floor = _load_baseline_test_inventory(
        manifest["test_inventory_contract"], manifest, repository_root
    )
    summaries: list[dict[str, Any]] = []
    passed_by_lane: dict[str, set[str]] = {}
    seen_typed_evidence_paths: set[str] = set()
    seen_typed_evidence_inodes: set[tuple[int, int]] = set()
    build_rows = {
        row["lane_id"]: row
        for row in manifest["test_inventory_contract"]["binding_build_lanes"]
    }
    proven_builds: dict[str, dict[str, Any]] = {}
    proven_build_files: dict[str, list[dict[str, Any]]] = {}
    proven_build_paths: dict[str, Path] = {}
    for row in manifest["test_inventory_contract"]["binding_result_lanes"]:
        lane_id = row["lane_id"]
        if lane_id not in successful_lanes:
            continue
        lane_root = _lane_artifact_root(artifact_root, lane_id)
        collection_path = _require_output_file(
            lane_root,
            row["collection_output"],
            f"{lane_id} native pytest collection",
        )
        junit_path = _require_output_file(
            lane_root,
            row["result_output"],
            f"{lane_id} native pytest JUnit",
        )
        witness_path = _require_output_file(
            lane_root,
            row["witness_output"],
            f"{lane_id} typed binding witness",
        )
        producer_lane_id = row["producer_lane_id"]
        if producer_lane_id not in successful_lanes:
            fail(f"{lane_id} lacks its required binding producer lane")
        producer_row = build_rows[producer_lane_id]
        producer_root = _lane_artifact_root(artifact_root, producer_lane_id)
        build_provenance_path = _require_output_file(
            producer_root,
            producer_row["provenance_output"],
            f"{lane_id} producer binding build provenance",
        )
        if producer_lane_id not in proven_builds:
            build_provenance, authenticated_build_files = (
                _validate_binding_build_provenance_artifact(
                    build_provenance_path,
                    lane_root=producer_root,
                    repository_root=repository_root,
                    row=producer_row,
                    build_contract=build_contract,
                    expected_manyforge_generation=expected_manyforge_generation,
                )
            )
            proven_builds[producer_lane_id] = build_provenance
            proven_build_files[producer_lane_id] = authenticated_build_files
            proven_build_paths[producer_lane_id] = build_provenance_path
        else:
            build_provenance = proven_builds[producer_lane_id]
            if proven_build_paths[producer_lane_id] != build_provenance_path:
                fail(f"{lane_id} binding producer provenance path drifted")
        collected = _pytest_collection_nodes(
            collection_path, f"{lane_id} native pytest collection"
        )
        junit = _pytest_junit_nodes(
            junit_path, f"{lane_id} native pytest JUnit"
        )
        failed = {name for name, status in junit.items() if status != "passed"}
        if failed or set(junit) != collected:
            fail(f"{lane_id} pytest execution differs from collection or is non-green")

        required = set(row["required_case_ids"])
        exact_case_nodes = {
            f"{BINDING_NODE_PREFIX}{case_id}" for case_id in required
        }
        collected_packet_nodes = {
            node for node in collected if node.startswith(BINDING_NODE_PREFIX)
        }
        if collected_packet_nodes != exact_case_nodes:
            fail(f"{lane_id} native pytest result omits required binding cases")
        baseline_case_count = 0
        if row["require_python_baseline"]:
            for prefix, minimum in python_floor.items():
                matches = {
                    node
                    for node in collected
                    if node == prefix or node.startswith(f"{prefix}[")
                }
                if len(matches) < minimum:
                    fail(
                        f"{lane_id} deleted or reduced Python binding baseline: "
                        f"{prefix}: expected>={minimum}, got={len(matches)}"
                    )
                baseline_case_count += len(matches)
            if baseline_case_count < PYTHON_BINDING_MINIMUM_CASE_COUNT:
                fail(f"{lane_id} Python binding baseline collection is incomplete")
        elif collected != exact_case_nodes:
            fail(f"{lane_id} specialized collection contains non-packet tests")

        witness = _read_json_object(
            witness_path, f"{lane_id} typed binding witness"
        )
        _exact_keys(
            witness,
            {
                "schema_version",
                "lane_id",
                "framework",
                "preparation_argv",
                "preparation_exit_status",
                "collection_argv",
                "collection_sha256",
                "junit_sha256",
                "child_argv",
                "child_exit_status",
                "extension_path",
                "extension_sha256",
                "producer_lane_id",
                "build_provenance_path",
                "build_provenance_sha256",
                "build_provenance_evidence_sha256",
                "case_witnesses",
            },
            f"{lane_id} typed binding witness",
        )
        child_argv = _require_list(
            witness["child_argv"], f"{lane_id} binding child argv"
        )
        collection_argv = _require_list(
            witness["collection_argv"], f"{lane_id} binding collection argv"
        )
        preparation_argv = _require_list(
            witness["preparation_argv"], f"{lane_id} binding preparation argv"
        )
        extension_path = _require_contained_regular_file(
            _binding_extension_host_path(artifact_root, lane_id),
            artifact_root,
            f"{lane_id} exact binding extension",
            require_single_link=True,
        )
        expected_extension_path = (
            f"{BINDING_LANE_STAGE_ROOTS[lane_id]}/{BINDING_EXTENSION_NAME}"
        )
        if (
            witness["schema_version"]
            != "manyforge.composable_cell_i2.binding_witness.v1"
            or witness["lane_id"] != lane_id
            or witness["framework"] != "packet-owned-pytest-junit"
            or preparation_argv != BINDING_LANE_PREPARATION.get(lane_id, [])
            or witness["preparation_exit_status"] != 0
            or collection_argv != _binding_collection_argv(row)
            or witness["collection_sha256"] != _sha256_file(collection_path)
            or witness["junit_sha256"] != _sha256_file(junit_path)
            or child_argv != _binding_run_argv(row)
            or witness["child_exit_status"] != 0
            or witness["extension_path"] != expected_extension_path
            or witness["extension_sha256"] != _sha256_file(extension_path)
            or witness["producer_lane_id"] != producer_lane_id
            or witness["build_provenance_path"]
            != producer_row["provenance_output"]
            or witness["build_provenance_sha256"]
            != _sha256_file(build_provenance_path)
            or witness["build_provenance_evidence_sha256"]
            != build_provenance["evidence_sha256"]
            or witness["extension_sha256"]
            != build_provenance["staged_extension"]["sha256"]
        ):
            fail(f"{lane_id} typed binding witness identity drifted")

        raw_case_witnesses = _require_list(
            witness["case_witnesses"], f"{lane_id} binding case witnesses"
        )
        case_witnesses: dict[str, dict[str, Any]] = {}
        witness_file_evidence: list[dict[str, Any]] = []
        for index, raw_case in enumerate(raw_case_witnesses):
            case = _require_object(
                raw_case, f"{lane_id} binding case witness {index}"
            )
            _exact_keys(
                case,
                {"case_id", "node_ids", "measurements", "evidence_files"},
                f"{lane_id} binding case witness {index}",
            )
            case_id = _require_text(
                case["case_id"], f"{lane_id} binding case witness ID"
            )
            if case_id in case_witnesses or case_id not in required:
                fail(f"{lane_id} binding case witness is unknown or duplicated")
            node_ids = _unique_text_set(
                case["node_ids"], f"{lane_id} {case_id} node IDs"
            )
            if (
                not node_ids
                or not node_ids.issubset(collected)
                or not _case_name_present(case_id, node_ids)
            ):
                fail(f"{lane_id} binding case witness is not tied to native tests")
            measurements = _require_object(
                case["measurements"], f"{lane_id} {case_id} measurements"
            )
            if set(measurements) != set(
                REQUIRED_BINDING_WITNESS_MEASUREMENTS[case_id]
            ):
                fail(f"{lane_id} binding case measurement keys drifted: {case_id}")
            _validate_binding_measurements(case_id, measurements)

            evidence_rows = _require_list(
                case["evidence_files"], f"{lane_id} {case_id} evidence files"
            )
            evidence_roles: list[str] = []
            evidence_paths: set[str] = set()
            for evidence_index, raw_evidence in enumerate(evidence_rows):
                evidence = _require_object(
                    raw_evidence,
                    f"{lane_id} {case_id} evidence file {evidence_index}",
                )
                _exact_keys(
                    evidence,
                    {"role", "path", "sha256", "size"},
                    f"{lane_id} {case_id} evidence file {evidence_index}",
                )
                role = _require_text(
                    evidence["role"],
                    f"{lane_id} {case_id} evidence role",
                )
                relative = _safe_relative_path(
                    evidence["path"], f"{lane_id} {case_id} evidence path"
                )
                qualified_evidence_path = f"{lane_id}/{relative.as_posix()}"
                if (
                    relative.as_posix() in evidence_paths
                    or qualified_evidence_path in seen_typed_evidence_paths
                    or relative.as_posix()
                    in {
                        row["collection_output"],
                        row["result_output"],
                        row["witness_output"],
                    }
                ):
                    fail(f"{lane_id} binding evidence path is duplicated or circular")
                evidence_paths.add(relative.as_posix())
                seen_typed_evidence_paths.add(qualified_evidence_path)
                path = _require_output_file(
                    lane_root,
                    relative.as_posix(),
                    f"{lane_id} {case_id} typed evidence",
                )
                inode = (path.stat().st_dev, path.stat().st_ino)
                if inode in seen_typed_evidence_inodes:
                    fail(f"{lane_id} binding evidence inode is reused")
                seen_typed_evidence_inodes.add(inode)
                if (
                    not _nonnegative_int(evidence["size"])
                    or evidence["size"] == 0
                    or evidence["sha256"] != _sha256_file(path)
                    or evidence["size"] != path.stat().st_size
                ):
                    fail(f"{lane_id} binding typed evidence digest drifted")
                direct = _read_json_object(
                    path, f"{lane_id} {case_id} direct binding evidence"
                )
                _exact_keys(
                    direct,
                    {
                        "schema_version",
                        "case_id",
                        "node_id",
                        "extension_sha256",
                        "measurements",
                        "observations",
                    },
                    f"{lane_id} {case_id} direct binding evidence",
                )
                recalculated = _recalculate_binding_measurements(
                    case_id, direct["observations"]
                )
                if (
                    role != "direct_case_evidence"
                    or direct["schema_version"]
                    != "manyforge.composable_cell_i2.direct_binding_case.v1"
                    or direct["case_id"] != case_id
                    or direct["node_id"] != f"{BINDING_NODE_PREFIX}{case_id}"
                    or direct["extension_sha256"]
                    != witness["extension_sha256"]
                    or direct["measurements"] != recalculated
                    or measurements != recalculated
                ):
                    fail(f"{lane_id} direct binding evidence drifted: {case_id}")
                _validate_binding_measurements(case_id, recalculated)
                evidence_roles.append(role)
                witness_file_evidence.append(
                    {"case_id": case_id, "role": role, **_file_evidence(path, artifact_root)}
                )
            if sorted(evidence_roles) != sorted(
                REQUIRED_BINDING_WITNESS_EVIDENCE_ROLES[case_id]
            ):
                fail(f"{lane_id} binding typed evidence roles drifted: {case_id}")
            case_witnesses[case_id] = case
        if set(case_witnesses) != required:
            fail(f"{lane_id} binding witness case set is incomplete")
        passed_by_lane[lane_id] = required
        summaries.append(
            {
                "lane_id": lane_id,
                "collected_count": len(collected),
                "collected_names_sha256": _name_digest(collected),
                "python_baseline_case_count": baseline_case_count,
                "required_case_ids": sorted(required),
                "extension": _file_evidence(extension_path, artifact_root),
                "collection": _file_evidence(collection_path, artifact_root),
                "junit": _file_evidence(junit_path, artifact_root),
                "witness": _file_evidence(witness_path, artifact_root),
                "producer_lane_id": producer_lane_id,
                "binding_build_provenance": _file_evidence(
                    build_provenance_path, artifact_root
                ),
                "authenticated_build_provenance_files": proven_build_files[
                    producer_lane_id
                ],
                "typed_evidence_files": witness_file_evidence,
            }
        )
    if {"bindings_selector", "bindings_release"}.issubset(proven_builds):
        if (
            proven_builds["bindings_selector"]["deterministic_parity"]
            != proven_builds["bindings_release"]["deterministic_parity"]
        ):
            fail("selector and Release binding builds are not byte deterministic")
    parity_contract = _require_object(
        build_contract["cross_profile_parity_contract"],
        "binding cross-profile parity contract",
    )
    binding_comparisons = [
        _require_object(raw, "binding cross-profile comparison")
        for raw in _require_list(
            parity_contract["comparisons"], "binding cross-profile comparisons"
        )
        if _require_object(raw, "binding cross-profile comparison").get("kind")
        == "binding_cross_profile"
    ]
    if len(binding_comparisons) != 1:
        fail("binding cross-profile comparison contract is not singular")
    comparison = binding_comparisons[0]
    lane_ids = [
        _require_text(value, "binding cross-profile lane")
        for value in _require_list(
            comparison.get("lane_ids"), "binding cross-profile lanes"
        )
    ]
    if set(lane_ids).issubset(proven_builds):
        expected_owners = _comparison_target_owners(
            comparison.get("target_owners"), "binding cross-profile target owners"
        )
        normalized: list[dict[str, Any]] = []
        for lane_id in lane_ids:
            parity = _require_object(
                proven_builds[lane_id]["deterministic_parity"],
                f"{lane_id} deterministic parity",
            )
            if _projection_target_owners(
                parity.get("target_projection"),
                f"{lane_id} deterministic target projection",
            ) != expected_owners:
                fail(f"{lane_id} binding parity owner set drifted")
            normalized.append(
                _binding_source_parity(parity, f"{lane_id} deterministic parity")
            )
        if any(value != normalized[0] for value in normalized[1:]):
            fail("binding authenticated target semantics differ across profiles")
    return summaries, passed_by_lane
def _validate_determinism_outputs(
    manifest: dict[str, Any],
    run_root: Path,
    successful_lanes: set[str],
    expected_manyforge_identity: dict[str, Any],
) -> tuple[list[dict[str, Any]], dict[str, Any] | None]:
    if "deterministic_rebuild" not in successful_lanes:
        return [], None
    comparison_lane_root = _lane_artifact_root(run_root, "deterministic_rebuild")
    rotating = manifest["rotating_identities"]
    artifact_maps: dict[str, dict[str, dict[str, Any]]] = {}
    artifact_inodes: dict[str, dict[str, tuple[int, int]]] = {}
    root_inodes: set[tuple[int, int]] = set()
    evidence: list[dict[str, Any]] = []
    for root_value in rotating["determinism_artifact_roots"]:
        root_relative = _safe_relative_path(root_value, "determinism artifact root")
        producer_lane = DETERMINISM_ROOT_LANES.get(root_value)
        if producer_lane is None:
            fail(f"determinism artifact root has no isolated producer: {root_value}")
        artifact_root = (
            _lane_artifact_root(run_root, producer_lane) / root_relative
        )
        if not artifact_root.is_dir() or artifact_root.is_symlink():
            fail(f"determinism artifact root is absent or unsafe: {root_value}")
        current = _lane_artifact_root(run_root, producer_lane)
        for part in root_relative.parts:
            current = current / part
            metadata = current.lstat()
            if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
                fail(f"determinism root contains an unsafe component: {current}")
        root_inode = (artifact_root.stat().st_dev, artifact_root.stat().st_ino)
        if root_inode in root_inodes:
            fail("determinism builds reuse one artifact-root inode")
        root_inodes.add(root_inode)
        artifacts: dict[str, dict[str, Any]] = {}
        inodes: dict[str, tuple[int, int]] = {}
        for pattern in rotating["regression_products"]:
            matches = sorted(candidate for candidate in artifact_root.glob(pattern) if candidate.is_file())
            if not matches:
                fail(f"determinism artifact pattern has no matches in {root_value}: {pattern}")
            for candidate in matches:
                path = _require_contained_regular_file(
                    candidate,
                    artifact_root,
                    f"determinism artifact {candidate}",
                    require_single_link=True,
                )
                relative = path.relative_to(artifact_root).as_posix()
                if relative in artifacts:
                    continue
                row = {
                    "path": relative,
                    "sha256": _sha256_file(path),
                    "size": path.stat().st_size,
                }
                if row["size"] == 0:
                    fail(f"determinism artifact is empty: {root_value}/{relative}")
                artifacts[relative] = row
                inodes[relative] = (path.stat().st_dev, path.stat().st_ino)
                evidence.append({"artifact_root": root_value, **row})
        artifact_maps[root_value] = artifacts
        artifact_inodes[root_value] = inodes
    roots = rotating["determinism_artifact_roots"]
    first = artifact_maps[roots[0]]
    second = artifact_maps[roots[1]]
    if first != second:
        fail("two clean deterministic builds produced different artifact paths or bytes")
    for relative in first:
        if artifact_inodes[roots[0]][relative] == artifact_inodes[roots[1]][relative]:
            fail(f"two deterministic builds reuse one artifact inode: {relative}")
    required_fixture_paths = set(rotating["fixture_ir_outputs"].values())
    actual_fixture_paths = {
        path for path in first if path.startswith("fixture-ir/")
    }
    if actual_fixture_paths != required_fixture_paths:
        fail("determinism fixture-IR artifact set drifted")
    fixture_by_id = {
        fixture["id"]: fixture for fixture in manifest["fixture_matrix"]
    }
    for fixture_id, relative in rotating["fixture_ir_outputs"].items():
        fixture = fixture_by_id[fixture_id]
        expected_native_groups = sorted(
            {row["group_id"] for row in fixture["native_test_witnesses"]}
        )
        expected_binding_cases = sorted(
            {
                case_id
                for row in fixture["binding_case_witnesses"]
                for case_id in row["case_ids"]
            }
        )
        for root_value in roots:
            producer_lane = DETERMINISM_ROOT_LANES[root_value]
            physical_root = (
                _lane_artifact_root(run_root, producer_lane) / root_value
            )
            path = _require_output_file(
                physical_root,
                relative,
                f"{root_value} fixture-IR witness {fixture_id}",
            )
            witness = _read_json_object(
                path, f"{root_value} fixture-IR witness {fixture_id}"
            )
            _exact_keys(
                witness,
                {
                    "schema_version",
                    "fixture_id",
                    "producer",
                    "producer_source_commit",
                    "producer_source_tree",
                    "native_witness_groups",
                    "binding_case_ids",
                    "raw_artifacts",
                    "canonical_input_sha256",
                    "compiler_ir_sha256",
                    "canonical_projection_sha256",
                },
                f"{root_value} fixture-IR witness {fixture_id}",
            )
            digest_fields = (
                "canonical_input_sha256",
                "compiler_ir_sha256",
                "canonical_projection_sha256",
            )
            if (
                witness["schema_version"] != FIXTURE_IR_WITNESS_SCHEMA
                or witness["fixture_id"] != fixture_id
                or witness["producer"]
                != "manyforge-i2-packet-determinism-executor"
                or witness["producer_source_commit"]
                != expected_manyforge_identity["head_commit"]
                or witness["producer_source_tree"]
                != expected_manyforge_identity["head_tree"]
                or witness["native_witness_groups"] != expected_native_groups
                or witness["binding_case_ids"] != expected_binding_cases
                or any(
                    not _sha256_value(witness[field])
                    or witness[field].removeprefix("sha256:") == "0" * 64
                    for field in digest_fields
                )
            ):
                fail(f"{root_value} fixture-IR witness contract drifted: {fixture_id}")
            raw_artifacts = _require_object(
                witness["raw_artifacts"],
                f"{root_value} fixture raw artifact paths {fixture_id}",
            )
            expected_raw_paths = _determinism_raw_paths(fixture_id)
            if raw_artifacts != expected_raw_paths:
                fail(f"{root_value} fixture raw paths drifted: {fixture_id}")
            raw_digests: dict[str, str] = {}
            for role, raw_relative in expected_raw_paths.items():
                raw_path = _require_output_file(
                    physical_root,
                    raw_relative,
                    f"{root_value} {fixture_id} raw {role}",
                )
                _strict_json_file(
                    raw_path, f"{root_value} {fixture_id} raw {role}"
                )
                raw_digests[role] = "sha256:" + _sha256_file(raw_path)
            if (
                witness["canonical_input_sha256"]
                != raw_digests["canonical_input"]
                or witness["compiler_ir_sha256"] != raw_digests["compiler_ir"]
                or witness["canonical_projection_sha256"]
                != raw_digests["canonical_projection"]
            ):
                fail(f"{root_value} fixture witness does not hash its raw files")

    derived_candidates: list[dict[str, str]] = []
    derived_identity_evidence: list[dict[str, Any]] = []
    for root_value in roots:
        producer_lane = DETERMINISM_ROOT_LANES[root_value]
        physical_root = (
            _lane_artifact_root(run_root, producer_lane) / root_value
        )
        path = _require_output_file(
            physical_root,
            rotating["identity_artifact"]["path"],
            f"{root_value} identity artifact",
        )
        derived = _recompute_package_identities(
            physical_root / "canonical_empty_product_package", root_value
        )
        for identity_name, field_name in rotating["identity_artifact"]["field_map"].items():
            derived_identity_evidence.append(
                {
                    "artifact_root": root_value,
                    "path": rotating["identity_artifact"]["path"],
                    "json_field": field_name,
                    "identity_name": identity_name,
                    "value": derived[identity_name],
                    "file_sha256": _sha256_file(path),
                    "file_size": path.stat().st_size,
                }
            )
        derived_candidates.append(derived)
    if derived_candidates[0] != derived_candidates[1]:
        fail("byte-identical identity artifacts did not derive one identity set")
    derived_candidate = derived_candidates[0]

    comparison_path = _require_output_file(
        comparison_lane_root,
        "determinism/comparison.json",
        "determinism comparison",
    )
    comparison = _read_json_object(comparison_path, "determinism comparison")
    _exact_keys(
        comparison,
        {
            "schema_version",
            "artifact_roots",
            "artifacts_byte_identical",
            "identity_baseline",
            "identity_candidate",
            "identity_relations",
            "passed",
        },
        "determinism comparison",
    )
    if (
        comparison["schema_version"]
        != "manyforge.composable_cell_i2.determinism_comparison.v1"
        or comparison["artifact_roots"] != roots
        or comparison["artifacts_byte_identical"] is not True
        or comparison["passed"] is not True
        or comparison["identity_baseline"] != rotating["baseline"]
        or comparison["identity_relations"] != rotating["candidate_expectations"]
    ):
        fail("determinism comparison contract or result drifted")
    candidate = _require_object(
        comparison["identity_candidate"], "determinism identity candidate"
    )
    if candidate != derived_candidate or set(candidate) != set(rotating["baseline"]):
        fail("producer identity candidate differs from verifier-derived artifacts")
    for key, baseline in rotating["baseline"].items():
        value = _require_text(candidate[key], f"determinism candidate identity {key}")
        if not value.startswith("sha256:") or not HEX64.fullmatch(value[7:]):
            fail(f"determinism candidate identity is malformed: {key}")
        relation = rotating["candidate_expectations"][key]
        if (relation == "changed") != (value != baseline):
            fail(f"determinism identity relation failed for {key}: expected {relation}")
    return evidence, {
        "comparison": comparison,
        "comparison_file": _file_evidence(comparison_path, run_root),
        "derived_identity_evidence": derived_identity_evidence,
    }


def _validate_scan_output(
    manifest: dict[str, Any],
    artifact_root: Path,
    successful_lanes: set[str],
    repository_root: Path,
) -> Any:
    if "forbidden_surface_scan" not in successful_lanes:
        return None
    lane_root = _lane_artifact_root(artifact_root, "forbidden_surface_scan")
    path = _require_output_file(
        lane_root,
        FORBIDDEN_SCAN_OUTPUT,
        "forbidden-surface scan result",
    )
    try:
        decoded = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_object_without_duplicates,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"forbidden-surface scan result is unreadable: {error}")
    rows = _require_list(decoded, "forbidden-surface scan result")
    if len(rows) != len(REQUIRED_SCAN_IDS):
        fail("forbidden-surface scan result count drifted")
    seen: set[str] = set()
    for index, raw in enumerate(rows):
        row = _require_object(raw, f"forbidden scan result {index}")
        scan_id = row.get("id")
        if scan_id in seen or scan_id not in REQUIRED_SCAN_IDS:
            fail(f"forbidden scan result is unknown or duplicated: {scan_id}")
        seen.add(scan_id)
        if (
            row.get("matches") != 0
            or row.get("expected_matches") != 0
            or row.get("passed") is not True
        ):
            fail(f"forbidden scan did not pass: {scan_id}")
    if seen != REQUIRED_SCAN_IDS:
        fail("forbidden scan result inventory is incomplete")
    recalculated = [
        evaluate_scan(scan, repository_root) for scan in manifest["forbidden_scans"]
    ]
    if rows != recalculated:
        fail("forbidden scan output differs from the exact candidate source inventory")
    return {"results": rows, "result_file": _file_evidence(path, artifact_root)}


def _validate_generated_api_inventory(
    manifest: dict[str, Any], path: Path
) -> set[str]:
    inventory = _read_json_object(path, "generated binding API inventory")
    _exact_keys(
        inventory,
        {"schema_version", "module", "framework", "python_abi", "exports"},
        "generated binding API inventory",
    )
    contract = manifest["binding_contract"]["generated_api_contract"]
    if (
        inventory["schema_version"] != contract["schema_version"]
        or inventory["module"] != "manyforge_core_py"
        or inventory["framework"] != "pybind11"
    ):
        fail("generated binding API identity drifted")
    python = manifest["qualification_environment"]["python"]
    if inventory["python_abi"] != {
        "implementation": python["implementation"],
        "version": python["version"],
        "soabi": python["soabi"],
        "extension_suffix": python["extension_suffix"],
        "gil_disabled": python["gil_disabled"],
    }:
        fail("generated binding API ABI metadata drifted")
    exports = _require_list(inventory["exports"], "generated binding API exports")
    if not exports:
        fail("generated binding API export inventory is empty")
    by_name: dict[str, dict[str, Any]] = {}
    for index, raw in enumerate(exports):
        row = _require_object(raw, f"generated binding export {index}")
        _exact_keys(
            row,
            {
                "name",
                "kind",
                "constructible",
                "writable_attributes",
                "trusted_projection",
            },
            f"generated binding export {index}",
        )
        name = _require_text(row["name"], f"generated binding export {index} name")
        if name in by_name or row["kind"] not in {"class", "enum", "function", "constant"}:
            fail(f"generated binding export is duplicated or has an unknown kind: {name}")
        if not (
            isinstance(row["constructible"], bool)
            or row["constructible"] is None
        ):
            fail(f"generated binding export has invalid constructibility: {name}")
        writable = _require_list(
            row["writable_attributes"], f"generated binding export {name} writable attributes"
        )
        if not all(isinstance(value, str) and value for value in writable) or len(writable) != len(
            set(writable)
        ):
            fail(f"generated binding export has invalid writable attributes: {name}")
        if not isinstance(row["trusted_projection"], bool):
            fail(f"generated binding export has invalid trust flag: {name}")
        if row["trusted_projection"] and (
            row["kind"] != "class" or row["constructible"] is not False or writable
        ):
            fail(f"trusted binding projection is constructible or writable: {name}")
        by_name[name] = row
    required = set(contract["required_read_only_nonconstructible_exports"])
    forbidden = set(contract["forbidden_exports"])
    if not required.issubset(by_name) or forbidden.intersection(by_name):
        fail("generated binding API required/forbidden export set failed")
    for name in required:
        row = by_name[name]
        if (
            row["kind"] != "class"
            or row["constructible"] is not False
            or row["writable_attributes"]
            or row["trusted_projection"] is not True
        ):
            fail(f"required trusted binding projection is not read-only: {name}")
    return set(by_name)


def _validate_fixture_witnesses(
    manifest: dict[str, Any],
    successful_lanes: set[str],
    repository_root: Path,
    native_passed: dict[str, set[str]],
    binding_passed: dict[str, set[str]],
    binding_results: list[dict[str, Any]],
    regression_products: list[dict[str, Any]],
    scan_results: dict[str, Any] | None,
    generated_outputs: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    groups, _ = _load_baseline_test_inventory(
        manifest["test_inventory_contract"], manifest, repository_root
    )
    native_contracts = {
        row["lane_id"]: row
        for row in manifest["test_inventory_contract"]["native_execution_lanes"]
    }
    binding_summary_by_lane = {row["lane_id"]: row for row in binding_results}
    generated_by_role = {row["role"]: row for row in generated_outputs}
    scan_ids = (
        {row["id"] for row in scan_results["results"]}
        if scan_results is not None
        else set()
    )
    regression_by_key = {
        (row["artifact_root"], row["path"]): row
        for row in regression_products
    }
    roots = manifest["rotating_identities"]["determinism_artifact_roots"]
    fixture_evidence: list[dict[str, Any]] = []
    for fixture in manifest["fixture_matrix"]:
        if not set(fixture["evidence_lanes"]).issubset(successful_lanes):
            continue
        native_rows: list[dict[str, Any]] = []
        for witness in fixture["native_test_witnesses"]:
            lane_id = witness["lane_id"]
            group_id = witness["group_id"]
            contract = native_contracts[lane_id]
            exclusions = set(contract.get("allowed_exclusions", []))
            required = groups[group_id] - exclusions
            passed = native_passed.get(lane_id, set())
            if not required.issubset(passed):
                fail(
                    f"fixture {fixture['id']} lacks native witnesses in {lane_id}: "
                    f"{sorted(required - passed)[:5]}"
                )
            native_rows.append(
                {
                    "lane_id": lane_id,
                    "group_id": group_id,
                    "required_count": len(required),
                    "required_names_sha256": _name_digest(required),
                    "excluded_names": sorted(groups[group_id] & exclusions),
                }
            )

        binding_rows: list[dict[str, Any]] = []
        for witness in fixture["binding_case_witnesses"]:
            lane_id = witness["lane_id"]
            required = set(witness["case_ids"])
            passed = binding_passed.get(lane_id, set())
            if not required.issubset(passed):
                fail(
                    f"fixture {fixture['id']} lacks binding witnesses in {lane_id}"
                )
            binding_rows.append(
                {
                    "lane_id": lane_id,
                    "case_ids": sorted(required),
                    "case_ids_sha256": _name_digest(required),
                }
            )

        floor_rows: list[dict[str, Any]] = []
        for lane_id in fixture["python_binding_floor_lanes"]:
            summary = binding_summary_by_lane.get(lane_id)
            if (
                summary is None
                or summary["python_baseline_case_count"]
                < PYTHON_BINDING_MINIMUM_CASE_COUNT
            ):
                fail(f"fixture {fixture['id']} lacks the Python binding floor")
            floor_rows.append(
                {
                    "lane_id": lane_id,
                    "function_count": PYTHON_BINDING_FUNCTION_COUNT,
                    "minimum_case_count": PYTHON_BINDING_MINIMUM_CASE_COUNT,
                    "observed_case_count": summary["python_baseline_case_count"],
                    "function_floor_sha256": PYTHON_BINDING_FUNCTION_DIGEST,
                }
            )

        required_scans = set(fixture["scan_ids"])
        if not required_scans.issubset(scan_ids):
            fail(f"fixture {fixture['id']} lacks exact forbidden-scan witnesses")

        determinism_rows: list[dict[str, Any]] = []
        for relative in fixture["determinism_artifacts"]:
            rows: list[dict[str, Any]] = []
            for root in roots:
                row = regression_by_key.get((root, relative))
                if row is None:
                    fail(
                        f"fixture {fixture['id']} lacks determinism artifact "
                        f"{root}/{relative}"
                    )
                rows.append(row)
            if len({(row["sha256"], row["size"]) for row in rows}) != 1:
                fail(f"fixture {fixture['id']} determinism artifacts differ")
            determinism_rows.append(
                {
                    "path": relative,
                    "roots": roots,
                    "sha256": rows[0]["sha256"],
                    "size": rows[0]["size"],
                }
            )

        generated_rows: list[dict[str, Any]] = []
        for role in fixture["generated_output_roles"]:
            row = generated_by_role.get(role)
            if row is None:
                fail(f"fixture {fixture['id']} lacks generated output {role}")
            generated_rows.append(row)

        fixture_evidence.append(
            {
                "id": fixture["id"],
                "evidence_lanes": fixture["evidence_lanes"],
                "native_test_witnesses": native_rows,
                "binding_case_witnesses": binding_rows,
                "python_binding_floor_witnesses": floor_rows,
                "scan_ids": sorted(required_scans),
                "scan_ids_sha256": _name_digest(required_scans),
                "determinism_artifacts": determinism_rows,
                "generated_outputs": generated_rows,
            }
        )
    if successful_lanes == REQUIRED_LANES and {
        row["id"] for row in fixture_evidence
    } != REQUIRED_FIXTURE_IDS:
        fail("complete qualification did not prove every exact fixture witness")
    return fixture_evidence


def _canonical_absolute_path(value: Any, label: str) -> str:
    path = _require_text(value, label)
    if (
        not path.startswith("/")
        or path.startswith("//")
        or "\\" in path
        or posixpath.normpath(path) != path
    ):
        fail(f"{label} is not one canonical absolute POSIX path")
    return path


def _path_is_within_any(path: str, roots: tuple[str, ...]) -> bool:
    return any(_logical_path_is_within(path, root) for root in roots)


def _pinned_container_claim(
    value: Any,
    *,
    claim_kind: str,
    label: str,
) -> dict[str, Any]:
    row = _require_object(value, label)
    if claim_kind not in PINNED_CONTAINER_CLAIM_KINDS:
        fail(f"{label} uses an unknown pinned-container claim kind")
    path = _canonical_absolute_path(row.get("path"), f"{label}.path")
    resolved_path = _canonical_absolute_path(
        row.get("resolved_path"), f"{label}.resolved_path"
    )
    digest = _require_text(row.get("sha256"), f"{label}.sha256")
    if HEX64.fullmatch(digest) is None:
        fail(f"{label}.sha256 is not one SHA-256 digest")

    if claim_kind in {"build_tool", "inspection_tool"}:
        catalog = (
            PINNED_CONTAINER_BUILD_TOOLS
            if claim_kind == "build_tool"
            else PINNED_CONTAINER_INSPECTION_TOOLS
        )
        if path not in catalog or not _path_is_within_any(
            resolved_path, ("/usr",)
        ):
            fail(f"{label} is not one exact {claim_kind.replace('_', '-')} route")
        chain = list(dict.fromkeys((path, resolved_path)))
    else:
        if row.get("scope") != "pinned_container":
            fail(f"{label} is not a pinned-container claim")
        chain = [
            _canonical_absolute_path(item, f"{label}.symlink_chain")
            for item in _require_list(
                row.get("symlink_chain"), f"{label}.symlink_chain"
            )
        ]
        if not chain or chain[0] != path or len(chain) != len(set(chain)):
            fail(f"{label} symlink chain is not exact, unique, and path-rooted")
        if claim_kind == "compile_dependency":
            expected_chain = list(dict.fromkeys((path, resolved_path)))
            if (
                not _path_is_within_any(path, ("/usr", "/lib"))
                or not _path_is_within_any(resolved_path, ("/usr", "/lib"))
                or chain != expected_chain
            ):
                fail(
                    f"{label} does not use endpoint-only pinned compiler semantics"
                )
        elif claim_kind in PINNED_CONTAINER_FULL_CHAIN_KINDS:
            if any(
                not _path_is_within_any(item, PINNED_CONTAINER_LIBRARY_ROOTS)
                for item in (resolved_path, *chain)
            ):
                fail(f"{label} escaped pinned system-library roots")
    return {
        "claim_kind": claim_kind,
        "content_format": (
            "archive"
            if claim_kind in {"link_command_input", "transitive_link_input"}
            and path.endswith(".a")
            else "elf"
            if claim_kind
            in {"elf_dependency", "link_command_input", "transitive_link_input"}
            else None
        ),
        "path": path,
        "resolved_path": resolved_path,
        "symlink_chain": chain,
        "sha256": digest,
    }


def _collect_pinned_elf_dependencies(
    value: Any,
    *,
    expected_identity: Any,
    lane_root: Path | None,
    local_runtime_owners: set[tuple[str, str]],
    label: str,
) -> tuple[list[dict[str, Any]], list[dict[str, str]]]:
    elf = _require_object(value, label)
    _exact_keys(
        elf,
        {
            "path",
            "resolved_path",
            "sha256",
            "readelf_header_sha256",
            "readelf_program_headers_sha256",
            "readelf_dynamic_sha256",
            "readelf_symbols_sha256",
            "ldd_normalized_sha256",
            "needed",
            "python_init_symbols",
            "dependencies",
        },
        label,
    )
    identity = {
        "path": _canonical_absolute_path(elf.get("path"), f"{label}.path"),
        "resolved_path": _canonical_absolute_path(
            elf.get("resolved_path"), f"{label}.resolved_path"
        ),
        "sha256": _require_text(elf.get("sha256"), f"{label}.sha256"),
    }
    expected = _require_object(expected_identity, f"{label} expected identity")
    expected_projection = {
        key: _require_text(expected.get(key), f"{label} expected {key}")
        for key in ("path", "resolved_path", "sha256")
    }
    digest_fields = (
        "sha256",
        "readelf_header_sha256",
        "readelf_program_headers_sha256",
        "readelf_dynamic_sha256",
        "readelf_symbols_sha256",
        "ldd_normalized_sha256",
    )
    python_init_symbols = [
        _require_text(item, f"{label}.python_init_symbols")
        for item in _require_list(
            elf["python_init_symbols"], f"{label}.python_init_symbols"
        )
    ]
    if (
        identity != expected_projection
        or any(
            HEX64.fullmatch(_require_text(elf.get(field), f"{label}.{field}"))
            is None
            for field in digest_fields
        )
        or python_init_symbols != sorted(set(python_init_symbols))
        or any(
            re.fullmatch(r"PyInit_[A-Za-z0-9_]+", symbol) is None
            for symbol in python_init_symbols
        )
    ):
        fail(f"{label} ELF/artifact identity drifted")
    claims: list[dict[str, Any]] = []
    external_rows: list[dict[str, str]] = []
    needed = [
        _require_text(item, f"{label}.needed")
        for item in _require_list(elf["needed"], f"{label}.needed")
    ]
    if needed != sorted(set(needed)):
        fail(f"{label}.needed is not exact, unique, and sorted")
    dependencies = _require_list(elf["dependencies"], f"{label}.dependencies")
    dependency_order: list[tuple[str, str]] = []
    dependency_sonames: set[str] = set()
    for index, raw_dependency in enumerate(dependencies):
        dependency_label = f"{label}.dependencies[{index}]"
        dependency = _require_object(raw_dependency, dependency_label)
        _exact_keys(
            dependency,
            {
                "soname",
                "path",
                "resolved_path",
                "symlink_chain",
                "sha256",
                "scope",
            },
            dependency_label,
        )
        soname = _require_text(
            dependency.get("soname"), f"{dependency_label}.soname"
        )
        scope = _require_text(
            dependency.get("scope"), f"{dependency_label}.scope"
        )
        path = _canonical_absolute_path(
            dependency.get("path"), f"{dependency_label}.path"
        )
        resolved_path = _canonical_absolute_path(
            dependency.get("resolved_path"), f"{dependency_label}.resolved_path"
        )
        digest = _require_text(
            dependency.get("sha256"), f"{dependency_label}.sha256"
        )
        if (
            soname in dependency_sonames
            or HEX64.fullmatch(digest) is None
            or scope not in {"pinned_container", "qualification_output"}
            or (
                scope == "qualification_output"
                and (
                    not path.startswith("/qualification-output/")
                    or not resolved_path.startswith("/qualification-output/")
                )
            )
        ):
            fail(f"{dependency_label} runtime dependency identity drifted")
        dependency_sonames.add(soname)
        dependency_order.append((soname, resolved_path))
        if scope == "pinned_container":
            claim = _pinned_container_claim(
                dependency,
                claim_kind="elf_dependency",
                label=dependency_label,
            )
            claims.append(claim)
            external_rows.append(
                {
                    "soname": soname,
                    "resolved_path": claim["resolved_path"],
                    "sha256": claim["sha256"],
                }
            )
        else:
            if lane_root is None:
                fail(f"{dependency_label} has no retained-lane replay root")
            physical, replayed_resolved, replayed_chain = (
                _resolve_lane_dependency_path(
                    path,
                    lane_root=lane_root,
                    label=f"{dependency_label} retained runtime dependency",
                )
            )
            chain = [
                _canonical_absolute_path(
                    item, f"{dependency_label}.symlink_chain"
                )
                for item in _require_list(
                    dependency.get("symlink_chain"),
                    f"{dependency_label}.symlink_chain",
                )
            ]
            _validate_native_file_magic(
                physical,
                expected="elf",
                label=f"{dependency_label} retained runtime dependency",
            )
            if (
                replayed_resolved != resolved_path
                or replayed_chain != chain
                or _sha256_file(physical) != digest
                or (resolved_path, digest) not in local_runtime_owners
            ):
                fail(f"{dependency_label} retained runtime owner/bytes drifted")
    if (
        not dependencies
        or dependency_order != sorted(dependency_order)
        or not set(needed).issubset(dependency_sonames)
    ):
        fail(f"{label} runtime dependency/DT_NEEDED closure drifted")
    return claims, external_rows


def _validate_binding_extension_elf_contract(value: Any, *, label: str) -> None:
    elf = _require_object(value, label)
    python_init_symbols = [
        _require_text(item, f"{label}.python_init_symbols")
        for item in _require_list(
            elf.get("python_init_symbols"), f"{label}.python_init_symbols"
        )
    ]
    needed = [
        _require_text(item, f"{label}.needed")
        for item in _require_list(elf.get("needed"), f"{label}.needed")
    ]
    if python_init_symbols != ["PyInit_manyforge_core_py"] or any(
        soname.startswith("libpython") for soname in needed
    ):
        fail(f"{label} binding PyInit/libpython contract drifted")


def _collect_target_pinned_container_claims(
    value: Any,
    *,
    lane_root: Path | None,
    local_runtime_owners: set[tuple[str, str]],
    label: str,
) -> tuple[list[dict[str, Any]], list[dict[str, str]]]:
    proofs = _require_list(value, label)
    claims: list[dict[str, Any]] = []
    external_rows: list[dict[str, str]] = []
    for proof_index, raw_proof in enumerate(proofs):
        proof_label = f"{label}[{proof_index}]"
        proof = _require_object(raw_proof, proof_label)
        for command_index, raw_command in enumerate(
            _require_list(proof.get("commands"), f"{proof_label}.commands")
        ):
            command_label = f"{proof_label}.commands[{command_index}]"
            command = _require_object(raw_command, command_label)
            for input_index, raw_input in enumerate(
                _require_list(
                    command.get("input_evidence", []),
                    f"{command_label}.input_evidence",
                )
            ):
                input_row = _require_object(
                    raw_input,
                    f"{command_label}.input_evidence[{input_index}]",
                )
                if input_row.get("scope") == "pinned_container":
                    command_kind = _require_text(
                        command.get("kind"), f"{command_label}.kind"
                    )
                    claims.append(
                        _pinned_container_claim(
                            input_row,
                            claim_kind=(
                                "link_command_input"
                                if command_kind == "link"
                                else "command_input"
                            ),
                            label=(
                                f"{command_label}.input_evidence[{input_index}]"
                            ),
                        )
                    )
            closure_value = command.get("dependency_closure")
            if closure_value is not None:
                closure = _require_object(
                    closure_value, f"{command_label}.dependency_closure"
                )
                for dependency_index, raw_dependency in enumerate(
                    _require_list(
                        closure.get("files"),
                        f"{command_label}.dependency_closure.files",
                    )
                ):
                    dependency = _require_object(
                        raw_dependency,
                        (
                            f"{command_label}.dependency_closure.files"
                            f"[{dependency_index}]"
                        ),
                    )
                    if dependency.get("scope") == "pinned_container":
                        claims.append(
                            _pinned_container_claim(
                                dependency,
                                claim_kind="compile_dependency",
                                label=(
                                    f"{command_label}.dependency_closure.files"
                                    f"[{dependency_index}]"
                                ),
                            )
                        )
        for dependency_index, raw_dependency in enumerate(
            _require_list(
                proof.get("transitive_external_link_inputs", []),
                f"{proof_label}.transitive_external_link_inputs",
            )
        ):
            dependency_label = (
                f"{proof_label}.transitive_external_link_inputs"
                f"[{dependency_index}]"
            )
            claims.append(
                _pinned_container_claim(
                    raw_dependency,
                    claim_kind="transitive_link_input",
                    label=dependency_label,
                )
            )
        if proof.get("elf") is not None:
            elf_claims, elf_external = _collect_pinned_elf_dependencies(
                proof["elf"],
                expected_identity=proof.get("artifact"),
                lane_root=lane_root,
                local_runtime_owners=local_runtime_owners,
                label=f"{proof_label}.elf",
            )
            claims.extend(elf_claims)
            external_rows.extend(elf_external)
    return claims, external_rows


def _collect_project_build_tool_claims(
    evidence: dict[str, Any],
    *,
    family: str,
    build_contract: dict[str, Any],
    label: str,
) -> list[dict[str, Any]]:
    project_values: Any = (
        [evidence.get("project")]
        if family == "production"
        else evidence.get("projects")
    )
    projects = _require_list(project_values, f"{label}.projects")
    if not projects:
        fail(f"{label} contains no project build-tool owners")
    claims: list[dict[str, Any]] = []
    python_contract = _require_object(
        _require_object(
            build_contract.get("toolchain_contract"),
            f"{label} toolchain contract",
        ).get("python"),
        f"{label} Python toolchain contract",
    )
    for project_index, raw_project in enumerate(projects):
        project_label = f"{label}.projects[{project_index}]"
        project = _require_object(raw_project, project_label)
        for tool_index, identity in enumerate(
            _validated_project_build_tool_identities(
                project,
                build_contract=build_contract,
                label=project_label,
            )
        ):
            claims.append(
                _pinned_container_claim(
                    identity,
                    claim_kind="build_tool",
                    label=f"{project_label}.build_tools[{tool_index}]",
                )
            )
        for fixture_index, raw_fixture in enumerate(
            _require_list(
                project.get("configure_executed_fixtures"),
                f"{project_label}.configure_executed_fixtures",
            )
        ):
            fixture_label = (
                f"{project_label}.configure_executed_fixtures[{fixture_index}]"
            )
            fixture = _require_object(raw_fixture, fixture_label)
            interpreter = _validated_frozen_tool_identity(
                fixture.get("interpreter"),
                contract=python_contract,
                label=f"{fixture_label}.interpreter",
            )
            claims.append(
                _pinned_container_claim(
                    interpreter,
                    claim_kind="build_tool",
                    label=f"{fixture_label}.interpreter",
                )
            )
    return claims


def _collect_provenance_pinned_container_claims(
    value: Any,
    *,
    family: str,
    build_contract: dict[str, Any],
    lane_root: Path | None,
    label: str,
) -> list[dict[str, Any]]:
    evidence = _require_object(value, label)
    target_fields_by_family = {
        "production": ("targets",),
        "native": ("production_targets", "test_only_targets", "targets"),
        "binding": ("dependency_targets", "module_target"),
    }
    if family not in target_fields_by_family:
        fail(f"{label} uses an unknown provenance family")
    claims = _collect_project_build_tool_claims(
        evidence,
        family=family,
        build_contract=build_contract,
        label=label,
    )
    external_rows: list[dict[str, str]] = []
    target_values: dict[str, list[Any]] = {}
    for field in target_fields_by_family[family]:
        field_value: Any = evidence.get(field)
        if field == "module_target":
            field_value = [field_value]
        target_values[field] = _require_list(field_value, f"{label}.{field}")
    local_runtime_owners: set[tuple[str, str]] = set()
    for field, proofs in target_values.items():
        if family == "binding" and field != "dependency_targets":
            continue
        for proof_index, raw_proof in enumerate(proofs):
            proof_label = f"{label}.{field}[{proof_index}]"
            proof = _require_object(raw_proof, proof_label)
            artifact = _require_object(
                proof.get("artifact"), f"{proof_label}.artifact"
            )
            local_runtime_owners.add(
                (
                    _canonical_absolute_path(
                        artifact.get("resolved_path"),
                        f"{proof_label}.artifact.resolved_path",
                    ),
                    _require_text(
                        artifact.get("sha256"), f"{proof_label}.artifact.sha256"
                    ),
                )
            )
    if family == "native":
        for installed_index, raw_installed in enumerate(
            _require_list(
                evidence.get("installed_copies"), f"{label}.installed_copies"
            )
        ):
            installed_label = f"{label}.installed_copies[{installed_index}]"
            installed = _require_object(raw_installed, installed_label)
            local_runtime_owners.add(
                (
                    _canonical_absolute_path(
                        installed.get("resolved_path"),
                        f"{installed_label}.resolved_path",
                    ),
                    _require_text(
                        installed.get("sha256"), f"{installed_label}.sha256"
                    ),
                )
            )
    for field, field_value in target_values.items():
        field_claims, field_external = _collect_target_pinned_container_claims(
            field_value,
            lane_root=lane_root,
            local_runtime_owners=local_runtime_owners,
            label=f"{label}.{field}",
        )
        claims.extend(field_claims)
        external_rows.extend(field_external)
    if family == "binding":
        _validate_binding_extension_elf_contract(
            evidence.get("elf"), label=f"{label}.elf"
        )
        elf_claims, elf_external = _collect_pinned_elf_dependencies(
            evidence.get("elf"),
            expected_identity=evidence.get("staged_extension"),
            lane_root=lane_root,
            local_runtime_owners=local_runtime_owners,
            label=f"{label}.elf",
        )
        claims.extend(elf_claims)
        external_rows.extend(elf_external)
    if family == "native":
        expected_external: dict[tuple[str, str, str], dict[str, str]] = {}
        for row in external_rows:
            key = (row["soname"], row["resolved_path"], row["sha256"])
            expected_external[key] = row
        expected_external_rows = [
            expected_external[key] for key in sorted(expected_external)
        ]
        actual_external_rows: list[dict[str, str]] = []
        for index, raw_external in enumerate(
            _require_list(
                evidence.get("external_dependencies"),
                f"{label}.external_dependencies",
            )
        ):
            external_label = f"{label}.external_dependencies[{index}]"
            external = _require_object(raw_external, external_label)
            _exact_keys(
                external,
                {"soname", "resolved_path", "sha256"},
                external_label,
            )
            actual_external_rows.append(
                {
                    "soname": _require_text(
                        external["soname"], f"{external_label}.soname"
                    ),
                    "resolved_path": _canonical_absolute_path(
                        external["resolved_path"],
                        f"{external_label}.resolved_path",
                    ),
                    "sha256": _require_text(
                        external["sha256"], f"{external_label}.sha256"
                    ),
                }
            )
        if actual_external_rows != expected_external_rows:
            fail(
                f"{label}.external_dependencies is not the exact pinned ELF projection"
            )

    inspection_rows = _require_list(
        evidence.get("inspection_tools"), f"{label}.inspection_tools"
    )
    if len(inspection_rows) != len(PINNED_CONTAINER_INSPECTION_TOOLS):
        fail(f"{label}.inspection_tools is not the exact tool catalog")
    for index, (raw_tool, expected_path) in enumerate(
        zip(
            inspection_rows,
            PINNED_CONTAINER_INSPECTION_TOOLS,
            strict=True,
        )
    ):
        tool_label = f"{label}.inspection_tools[{index}]"
        tool = _require_object(raw_tool, tool_label)
        _exact_keys(tool, {"path", "resolved_path", "sha256"}, tool_label)
        if tool.get("path") != expected_path:
            fail(f"{tool_label} path/order drifted")
        claims.append(
            _pinned_container_claim(
                tool,
                claim_kind="inspection_tool",
                label=tool_label,
            )
        )
    return claims


def _collect_pinned_container_claims(
    manifest: dict[str, Any],
    artifact_root: Path,
    successful_lanes: set[str],
    build_contract: dict[str, Any],
) -> list[dict[str, Any]]:
    inventory_contract = manifest["test_inventory_contract"]
    claims: list[dict[str, Any]] = []
    provenance_families = (
        ("production", inventory_contract["production_build_lanes"]),
        ("native", inventory_contract["native_execution_lanes"]),
        ("binding", inventory_contract["binding_build_lanes"]),
    )
    for family, rows in provenance_families:
        for row in rows:
            lane_id = row["lane_id"]
            if lane_id not in successful_lanes:
                continue
            provenance_path = _require_output_file(
                _lane_artifact_root(artifact_root, lane_id),
                row["provenance_output"],
                f"{lane_id} pinned-container replay provenance",
            )
            evidence = _read_json_object(
                provenance_path,
                f"{lane_id} pinned-container replay provenance",
            )
            claims.extend(
                _collect_provenance_pinned_container_claims(
                    evidence,
                    family=family,
                    build_contract=build_contract,
                    lane_root=_lane_artifact_root(artifact_root, lane_id),
                    label=f"{lane_id} pinned-container replay provenance",
                )
            )
    return claims


def _pinned_container_replay_argv(image_id: str) -> list[str]:
    return [
        "docker",
        "run",
        "--rm",
        "-i",
        "--read-only",
        "--platform",
        "linux/amd64",
        "--network",
        "none",
        "--cap-drop",
        "ALL",
        "--security-opt",
        "no-new-privileges=true",
        "--entrypoint",
        "/usr/local/bin/python3",
        "--workdir",
        "/",
        image_id,
        "-I",
        "-c",
        PINNED_CONTAINER_REPLAY_PROGRAM,
    ]


def _execute_pinned_container_replay(
    argv: list[str],
    request_bytes: bytes,
    repository_root: Path,
) -> dict[str, Any]:
    try:
        completed = subprocess.run(
            argv,
            cwd=repository_root,
            input=request_bytes,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=PINNED_CONTAINER_REPLAY_TIMEOUT_SECONDS,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        fail(f"exact-image pinned-container replay could not run: {error}")
    if completed.returncode != 0:
        diagnostic = completed.stderr.decode("utf-8", errors="replace")[-2000:]
        fail(
            "exact-image pinned-container replay failed with status "
            f"{completed.returncode}: {diagnostic}"
        )
    if len(completed.stdout) > PINNED_CONTAINER_REPLAY_MAX_REQUEST_BYTES * 2:
        fail("exact-image pinned-container replay response exceeds its byte bound")
    try:
        response = json.loads(
            completed.stdout.decode("utf-8"),
            object_pairs_hook=_object_without_duplicates,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"exact-image pinned-container replay returned invalid JSON: {error}")
    return _require_object(response, "exact-image pinned-container replay response")


def _validate_pinned_container_replay_response(
    claims: list[dict[str, Any]],
    response_value: Any,
) -> list[dict[str, Any]]:
    response = _require_object(
        response_value, "exact-image pinned-container replay response"
    )
    _exact_keys(
        response,
        {"schema_version", "files"},
        "exact-image pinned-container replay response",
    )
    if response["schema_version"] != PINNED_CONTAINER_REPLAY_RESPONSE_SCHEMA:
        fail("exact-image pinned-container replay response schema drifted")
    expected = {
        (claim["claim_kind"], claim["path"]): claim for claim in claims
    }
    if len(expected) != len(claims):
        fail("exact-image pinned-container replay claims are not unique")
    files: list[dict[str, Any]] = []
    seen: set[tuple[str, str]] = set()
    for index, raw_file in enumerate(
        _require_list(
            response["files"], "exact-image pinned-container replay files"
        )
    ):
        file_label = f"exact-image pinned-container replay files[{index}]"
        replayed = _require_object(raw_file, file_label)
        _exact_keys(
            replayed,
            {
                "claim_kind",
                "content_format",
                "path",
                "resolved_path",
                "symlink_chain",
                "sha256",
                "size",
            },
            file_label,
        )
        claim_kind = _require_text(
            replayed["claim_kind"], f"{file_label}.claim_kind"
        )
        content_format = replayed["content_format"]
        path = _canonical_absolute_path(replayed["path"], f"{file_label}.path")
        key = (claim_kind, path)
        claim = expected.get(key)
        if key in seen or claim is None:
            fail(f"{file_label} is duplicated or was not requested")
        seen.add(key)
        resolved_path = _canonical_absolute_path(
            replayed["resolved_path"], f"{file_label}.resolved_path"
        )
        chain = [
            _canonical_absolute_path(item, f"{file_label}.symlink_chain")
            for item in _require_list(
                replayed["symlink_chain"], f"{file_label}.symlink_chain"
            )
        ]
        digest = _require_text(replayed["sha256"], f"{file_label}.sha256")
        size = replayed["size"]
        if (
            not chain
            or chain[0] != path
            or len(chain) != len(set(chain))
            or HEX64.fullmatch(digest) is None
            or not isinstance(size, int)
            or isinstance(size, bool)
            or size < 0
            or resolved_path != claim["resolved_path"]
            or digest != claim["sha256"]
            or chain != claim["symlink_chain"]
            or content_format != claim["content_format"]
        ):
            fail(f"{file_label} differs from its authenticated provenance claim")
        files.append(
            {
                "claim_kind": claim_kind,
                "content_format": content_format,
                "path": path,
                "resolved_path": resolved_path,
                "symlink_chain": chain,
                "sha256": digest,
                "size": size,
            }
        )
    if seen != set(expected) or files != sorted(
        files, key=lambda row: (row["claim_kind"], row["path"])
    ):
        fail("exact-image pinned-container replay response is incomplete or unsorted")
    return files


def _collect_pinned_container_replay(
    manifest: dict[str, Any],
    artifact_root: Path,
    successful_lanes: set[str],
    repository_root: Path,
) -> dict[str, Any]:
    build_contract = _load_build_provenance_contract(
        manifest["test_inventory_contract"], manifest, repository_root
    )
    occurrences = _collect_pinned_container_claims(
        manifest, artifact_root, successful_lanes, build_contract
    )
    occurrence_counts = Counter(
        claim["claim_kind"] for claim in occurrences
    )
    unique_by_key: dict[tuple[str, str], dict[str, Any]] = {}
    for claim in occurrences:
        key = (claim["claim_kind"], claim["path"])
        previous = unique_by_key.setdefault(key, claim)
        if previous != claim:
            fail(
                "pinned-container provenance contains conflicting claims for "
                f"{claim['claim_kind']}:{claim['path']}"
            )
    claims = [unique_by_key[key] for key in sorted(unique_by_key)]
    if len(claims) > PINNED_CONTAINER_REPLAY_MAX_CLAIMS:
        fail("pinned-container replay exceeds its unique-claim bound")
    unique_counts = Counter(claim["claim_kind"] for claim in claims)
    if successful_lanes == REQUIRED_LANES:
        deficient = {
            claim_kind: {
                "required_minimum": required_minimum,
                "actual": unique_counts[claim_kind],
            }
            for claim_kind, required_minimum in (
                PINNED_CONTAINER_COMPLETE_MINIMUM_UNIQUE_CLAIMS.items()
            )
            if unique_counts[claim_kind] < required_minimum
        }
        if deficient:
            fail(
                "complete pinned-container replay claim-kind cardinality "
                f"drifted: {deficient}"
            )
    request = {
        "schema_version": PINNED_CONTAINER_REPLAY_REQUEST_SCHEMA,
        "claims": [
            {"claim_kind": claim["claim_kind"], "path": claim["path"]}
            for claim in claims
        ],
    }
    request_bytes = json.dumps(
        request, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    if len(request_bytes) > PINNED_CONTAINER_REPLAY_MAX_REQUEST_BYTES:
        fail("pinned-container replay request exceeds its byte bound")
    image_id = _require_text(
        manifest["qualification_environment"]["image"]["id"],
        "pinned-container replay image ID",
    )
    argv = _pinned_container_replay_argv(image_id)
    replayed_files: list[dict[str, Any]] = []
    executed = bool(claims)
    if executed:
        response = _execute_pinned_container_replay(
            argv, request_bytes, repository_root
        )
        replayed_files = _validate_pinned_container_replay_response(
            claims, response
        )
    if successful_lanes == REQUIRED_LANES and not executed:
        fail("complete qualification produced no pinned-container replay claims")
    return {
        "schema_version": PINNED_CONTAINER_REPLAY_RECEIPT_SCHEMA,
        "image_id": image_id,
        "batch_count": int(executed),
        "claim_kinds": [
            {
                "claim_kind": claim_kind,
                "occurrence_count": occurrence_counts[claim_kind],
                "unique_claim_count": unique_counts[claim_kind],
            }
            for claim_kind in PINNED_CONTAINER_CLAIM_KINDS
        ],
        "claim_occurrence_count": len(occurrences),
        "unique_claim_count": len(claims),
        "unique_resolved_file_count": len(
            {
                (row["resolved_path"], row["sha256"], row["size"])
                for row in replayed_files
            }
        ),
        "claims_sha256": _canonical_json_sha256(claims),
        "request_sha256": hashlib.sha256(request_bytes).hexdigest(),
        "replayed_file_count": len(replayed_files),
        "replayed_files_sha256": _canonical_json_sha256(replayed_files),
        "replay_program_sha256": hashlib.sha256(
            PINNED_CONTAINER_REPLAY_PROGRAM.encode("utf-8")
        ).hexdigest(),
        "docker_argv_sha256": _canonical_json_sha256(argv),
        "executed": executed,
        "passed": executed,
    }


def _collect_run_artifacts(
    manifest: dict[str, Any],
    artifact_root: Path,
    log_root: Path,
    successful_lanes: set[str],
    repository_root: Path,
    expected_manyforge_identity: dict[str, Any],
    expected_specs_identity: dict[str, Any],
) -> dict[str, Any]:
    candidate_snapshot_root = artifact_root.parent / "source"
    try:
        expected_manyforge_generation = replay_git_source_generation(
            candidate_snapshot_root,
            [".docker/roboplan/"],
        )
    except NativeBuildProvenanceError as error:
        fail(f"candidate snapshot source-generation replay failed: {error}")
    if (
        expected_manyforge_generation["base_commit"]
        != expected_manyforge_identity["head_commit"]
        or expected_manyforge_generation["base_tree"]
        != expected_manyforge_identity["head_tree"]
        or expected_manyforge_generation["patched_index_tree"]
        != expected_manyforge_identity["head_tree"]
    ):
        fail("candidate snapshot Git generation differs from captured identity")
    source_identity_files = _validate_source_identity_files(
        artifact_root,
        successful_lanes,
        expected_manyforge_identity,
        expected_specs_identity,
    )
    test_inventories, native_passed = _validate_grouped_results(
        manifest,
        artifact_root,
        log_root,
        successful_lanes,
        repository_root,
        expected_manyforge_generation,
    )
    test_inventories.extend(
        _validate_production_builds(
            manifest,
            artifact_root,
            successful_lanes,
            repository_root,
            expected_manyforge_generation,
        )
    )
    binding_results, binding_passed = _validate_binding_results(
        manifest,
        artifact_root,
        successful_lanes,
        repository_root,
        expected_manyforge_generation,
    )
    pinned_container_replay = _collect_pinned_container_replay(
        manifest,
        artifact_root,
        successful_lanes,
        repository_root,
    )

    generated: list[dict[str, Any]] = []
    generated_text: dict[str, str] = {}
    for output in manifest["binding_contract"]["generated_outputs"]:
        producer_lane = GENERATED_OUTPUT_LANES[output["role"]]
        if producer_lane not in successful_lanes:
            continue
        relative = Path(output["path"]).relative_to("/qualification-output")
        path = _require_output_file(
            _lane_artifact_root(artifact_root, producer_lane),
            relative.as_posix(),
            f"generated binding {output['role']}",
        )
        if output["role"] == "extension":
            if path.read_bytes()[:4] != b"\x7fELF":
                fail("generated manyforge_core_py extension is not an ELF binary")
        else:
            try:
                content = path.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as error:
                fail(f"generated binding {output['role']} is not UTF-8 text: {error}")
            for forbidden in (
                "manyforge_roboplan_py",
                "StructurallyQualifiedCellGraph",
                "TrustedCellGraph",
                "ParsedModelSourceIr",
                "manyforge.experimental.composable_cell.i1a.v0",
            ):
                if forbidden in content:
                    fail(f"generated binding {output['role']} contains {forbidden}")
            if "CompilerQualifiedCellIr" not in content:
                fail(f"generated binding {output['role']} omits CompilerQualifiedCellIr")
            if output["role"] == "api_inventory":
                _validate_generated_api_inventory(manifest, path)
            generated_text[output["role"]] = content
        row = _file_evidence(path, artifact_root)
        row["role"] = output["role"]
        generated.append(row)
    if "bindings_generated_api" in successful_lanes:
        required = manifest["binding_contract"]["generated_api_contract"][
            "required_read_only_nonconstructible_exports"
        ]
        for role in ("typed_stub", "api_inventory", "api_documentation"):
            missing = [name for name in required if name not in generated_text[role]]
            if missing:
                fail(f"generated binding {role} omits required exports: {missing}")

    regression, comparison = _validate_determinism_outputs(
        manifest,
        artifact_root,
        successful_lanes,
        expected_manyforge_identity,
    )
    scan_results = _validate_scan_output(
        manifest, artifact_root, successful_lanes, repository_root
    )
    fixture_witnesses = _validate_fixture_witnesses(
        manifest,
        successful_lanes,
        repository_root,
        native_passed,
        binding_passed,
        binding_results,
        regression,
        scan_results,
        generated,
    )

    return {
        "source_identity_file_evidence": source_identity_files,
        "pinned_container_replay": pinned_container_replay,
        "test_inventory_counts_and_digests": test_inventories,
        "binding_junit_summary": binding_results,
        "generated_output_sha256_and_size": generated,
        "fixture_ir_manifest_golden_and_product_digests": {
            "fixture_witnesses": fixture_witnesses,
            "determinism_products": regression,
        },
        "identity_rotation_comparison": comparison,
        "forbidden_scan_results": scan_results,
    }


def execute_lanes(
    manifest: dict[str, Any],
    lanes: dict[str, dict[str, Any]],
    requested: str,
    repository_root: Path,
    run_id: str | None,
) -> int:
    if requested != "all" and requested not in lanes:
        fail(f"unknown qualification lane: {requested}")
    packet_lineage = _validate_historical_packet_lineage(manifest, repository_root)
    selected = list(lanes.values()) if requested == "all" else [lanes[requested]]
    identifier = run_id or datetime.now(UTC).strftime("%Y%m%dT%H%M%SZ")
    if not RUN_ID.fullmatch(identifier):
        fail("run ID contains unsafe characters")
    output_root = _safe_output_root(repository_root)
    run_root = output_root / identifier
    if run_root.exists():
        fail(f"qualification run already exists: {run_root}")

    environment = check_environment(manifest, repository_root)
    identity_tool = _require_contained_regular_file(
        repository_root / manifest["candidate_identity"]["capture_tool"],
        repository_root,
        "candidate identity capture tool",
    )
    specs_root = (repository_root / "../manyforge_specs").resolve()
    source_before = capture_source_identity(identity_tool, repository_root)
    specs_before = capture_source_identity(identity_tool, specs_root)
    if source_before.get("dirty") or specs_before.get("dirty"):
        fail("qualification execution requires clean committed ManyForge and Specs trees")
    _require_commit_descends_from(
        repository_root,
        candidate=source_before["head_commit"],
        base=IMPLEMENTATION_BASES["manyforge"]["head_commit"],
        label="ManyForge",
    )
    _require_commit_descends_from(
        specs_root,
        candidate=specs_before["head_commit"],
        base=IMPLEMENTATION_BASES["manyforge_specs"]["head_commit"],
        label="Specs",
    )
    try:
        run_root.mkdir(parents=True, exist_ok=False)
        artifact_root = run_root / "artifacts"
        log_root = run_root / "logs"
        seal_root = run_root / "artifact-seals"
        artifact_root.mkdir()
        (artifact_root / "lanes").mkdir()
        log_root.mkdir()
        seal_root.mkdir()
    except FileExistsError:
        fail(f"qualification run already exists: {run_root}")
    except OSError as error:
        fail(f"cannot create qualification run directory: {error}")
    _write_exclusive_json(
        run_root,
        "host-manyforge-source-before.json",
        source_before,
    )
    _write_exclusive_json(
        run_root,
        "host-specs-source-before.json",
        specs_before,
    )
    snapshot_root, snapshot_evidence = _materialize_candidate_snapshot(
        repository_root,
        run_root,
        CAPTURE_TOOL,
        source_before,
        manifest["pinned_inputs"]["roboplan"],
    )

    lane_results: list[dict[str, Any]] = []
    lane_seals: list[dict[str, Any]] = []
    overall_status = 0
    for lane in selected:
        lane_id = lane["id"]
        producers = LANE_READ_ONLY_INPUTS.get(lane_id, ())
        sealed_ids = {row["lane_id"] for row in lane_seals}
        if any(producer not in sealed_ids for producer in producers):
            fail(
                f"{lane_id} requires producer lanes in the same cumulative run: "
                f"{list(producers)}"
            )
        if producers:
            _validate_lane_artifact_seals(
                artifact_root, seal_root, set(producers)
            )
        lane_root = _lane_artifact_root(artifact_root, lane_id)
        lane_root.mkdir(mode=0o700)
        argv = render_lane(
            manifest, lane, snapshot_root, lane_root, artifact_root
        )
        log_name = f"{lane['ordinal']:02d}-{lane['id']}.log"
        log_path = log_root / log_name
        with _exclusive_output_file(log_root, log_name, binary=True) as log:
            process = subprocess.Popen(
                argv,
                cwd=repository_root,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            assert process.stdout is not None
            for chunk in iter(lambda: process.stdout.read(64 * 1024), b""):
                log.write(chunk)
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
            returncode = process.wait()
            if returncode == 0:
                invalid_outputs: list[str] = []
                for output in lane["expected_outputs"]:
                    path = lane_root / output
                    try:
                        metadata = path.lstat()
                    except OSError:
                        invalid_outputs.append(f"{output} (missing)")
                        continue
                    if (
                        stat.S_ISLNK(metadata.st_mode)
                        or not stat.S_ISREG(metadata.st_mode)
                        or metadata.st_size == 0
                    ):
                        invalid_outputs.append(f"{output} (unsafe or empty)")
                if invalid_outputs:
                    message = (
                        "\nqualification runner: declared outputs are missing, unsafe, or empty: "
                        + ", ".join(invalid_outputs)
                        + "\n"
                    ).encode()
                    log.write(message)
                    sys.stdout.buffer.write(message)
                    sys.stdout.buffer.flush()
                    returncode = 97
                else:
                    lane_seals.append(
                        _seal_lane_artifacts(artifact_root, seal_root, lane_id)
                    )
        lane_results.append(
            {
                "ordinal": lane["ordinal"],
                "id": lane["id"],
                "argv": argv,
                "exit_status": returncode,
                "log_path": log_path.relative_to(run_root).as_posix(),
                "log_sha256": _sha256_file(log_path),
                "log_size": log_path.stat().st_size,
            }
        )
        if returncode != 0:
            overall_status = returncode
            break

    source_after = capture_source_identity(identity_tool, repository_root)
    specs_after = capture_source_identity(identity_tool, specs_root)
    _write_exclusive_json(
        run_root,
        "host-manyforge-source-after.json",
        source_after,
    )
    _write_exclusive_json(
        run_root,
        "host-specs-source-after.json",
        specs_after,
    )
    unchanged = source_before == source_after and specs_before == specs_after
    in_container_identity_equal: bool | None = None
    if requested == "all" and overall_status == 0:
        try:
            in_container_before = json.loads(
                (
                    _lane_artifact_root(artifact_root, "preflight")
                    / "candidate-source.json"
                ).read_text(encoding="utf-8")
            )
            in_container_after = json.loads(
                (
                    _lane_artifact_root(artifact_root, "final_recheck")
                    / "final-candidate-source.json"
                ).read_text(encoding="utf-8")
            )
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            fail(f"in-container source identity output is unreadable: {error}")
        in_container_identity_equal = (
            in_container_before == source_before
            and in_container_after == source_before
        )
        unchanged = unchanged and in_container_identity_equal
    if not unchanged:
        overall_status = overall_status or 1
    successful_lanes = {
        row["id"] for row in lane_results if row["exit_status"] == 0
    }
    automatic_lanes_succeeded_before_artifacts = (
        requested == "all" and overall_status == 0
    )
    artifact_validation_error: str | None = None
    try:
        validated_lane_seals = _validate_lane_artifact_seals(
            artifact_root, seal_root, successful_lanes
        )
        artifacts = _collect_run_artifacts(
            manifest,
            artifact_root,
            log_root,
            successful_lanes,
            snapshot_root,
            source_before,
            specs_before,
        )
    except ManifestError as error:
        artifact_validation_error = str(error)
        overall_status = overall_status or 98
        artifacts = {
            "source_identity_file_evidence": [],
            "pinned_container_replay": None,
            "test_inventory_counts_and_digests": [],
            "binding_junit_summary": [],
            "generated_output_sha256_and_size": [],
            "fixture_ir_manifest_golden_and_product_digests": {
                "fixture_witnesses": [],
                "determinism_products": [],
            },
            "identity_rotation_comparison": None,
            "forbidden_scan_results": None,
        }
        validated_lane_seals = []
    try:
        tracked_digests = _candidate_tracked_file_digests(
            manifest,
            repository_root,
            require_final_rotation=automatic_lanes_succeeded_before_artifacts,
        )
    except ManifestError as error:
        if artifact_validation_error:
            artifact_validation_error += f"; {error}"
        else:
            artifact_validation_error = str(error)
        overall_status = overall_status or 98
        tracked_digests = _candidate_tracked_file_digests(
            manifest,
            repository_root,
            require_final_rotation=False,
        )
    result = {
        "schema_version": RESULT_SCHEMA,
        "manifest_sha256": _sha256_file(
            repository_root
            / "verification/composable_cell_i2/qualification_manifest.v1.json"
        ),
        "verifier_sha256": _sha256_file(Path(__file__).resolve()),
        "historical_packet_lineage": packet_lineage,
        "candidate_manyforge_source_identity": source_before,
        "candidate_specs_source_identity": specs_before,
        "candidate_source_snapshot": snapshot_evidence,
        "candidate_tracked_input_digests": tracked_digests,
        "resolved_image_inspection": environment["image"],
        "abi_and_dependency_probe": environment["probe"],
        "exact_lane_argv": [row["argv"] for row in lane_results],
        "lane_exit_statuses": [
            {"id": row["id"], "exit_status": row["exit_status"]}
            for row in lane_results
        ],
        "lane_artifact_seals": validated_lane_seals,
        "log_sha256": [
            {
                "id": row["id"],
                "path": row["log_path"],
                "sha256": row["log_sha256"],
                "size": row["log_size"],
            }
            for row in lane_results
        ],
        **artifacts,
        "artifact_validation_error": artifact_validation_error,
        "final_unchanged_source_identity": {
            "passed": unchanged,
            "in_container_equal": in_container_identity_equal,
            "manyforge": source_after,
            "manyforge_specs": specs_after,
        },
        "independent_adversarial_review": None,
        "automatic_lanes_passed": requested == "all" and overall_status == 0,
        "complete": False,
    }
    result_path = run_root / "qualification-result.json"
    _write_exclusive_json(
        run_root,
        result_path.name,
        result,
    )
    print(f"qualification result: {result_path}")
    return overall_status


def _validate_result_tracked_digests(
    manifest: dict[str, Any], result: dict[str, Any], repository_root: Path
) -> None:
    rows = _require_list(
        result["candidate_tracked_input_digests"],
        "qualification result candidate tracked digests",
    )
    expected_rows = {
        row["path"]: row for row in manifest["pinned_inputs"]["tracked_files"]
    }
    if len(rows) != len(expected_rows):
        fail("qualification result tracked-input count drifted")
    candidate_commit = result["candidate_manyforge_source_identity"]["head_commit"]
    seen: set[str] = set()
    for index, raw in enumerate(rows):
        row = _require_object(raw, f"qualification tracked digest {index}")
        _exact_keys(
            row,
            {
                "path",
                "policy",
                "baseline_sha256",
                "candidate_sha256",
                "size",
                "changed",
            },
            f"qualification tracked digest {index}",
        )
        path = row["path"]
        expected = expected_rows.get(path)
        if path in seen or expected is None:
            fail(f"qualification tracked digest path is unknown or duplicated: {path}")
        seen.add(path)
        blob = _git_blob(repository_root, candidate_commit, path)
        candidate_digest = hashlib.sha256(blob).hexdigest()
        if (
            row["policy"] != expected["policy"]
            or row["baseline_sha256"] != expected["sha256"]
            or row["candidate_sha256"] != candidate_digest
            or row["size"] != len(blob)
            or not isinstance(row["changed"], bool)
            or row["changed"] != (candidate_digest != expected["sha256"])
        ):
            fail(f"qualification tracked digest evidence is wrong: {path}")
        if expected["policy"] == "immutable_in_i2" and row["changed"]:
            fail(f"immutable I2 input changed in the qualified commit: {path}")
        if expected["policy"] in {"must_change_and_requalify", "rotate_and_requalify"} and not row[
            "changed"
        ]:
            fail(f"required I2 input did not rotate in the qualified commit: {path}")
    if seen != set(expected_rows):
        fail("qualification tracked digest inventory is incomplete")


def _validate_adversarial_review(
    review_path: Path,
    result_path: Path,
    manifest_sha256: str,
    verifier_sha256: str,
    result: dict[str, Any],
) -> dict[str, Any]:
    review = _read_json_object(review_path, "independent adversarial review")
    _exact_keys(
        review,
        {
            "schema_version",
            "independent",
            "reviewer",
            "reviewed_at_utc",
            "status",
            "reviewed_result_sha256",
            "manifest_sha256",
            "verifier_sha256",
            "candidate_manyforge_source_identity",
            "candidate_specs_source_identity",
            "scope",
            "findings",
            "unresolved_findings",
        },
        "independent adversarial review",
    )
    if (
        review["schema_version"] != REVIEW_SCHEMA
        or review["independent"] is not True
        or review["status"] != "pass"
        or review["reviewed_result_sha256"] != _sha256_file(result_path)
        or review["manifest_sha256"] != manifest_sha256
        or review["verifier_sha256"] != verifier_sha256
        or review["candidate_manyforge_source_identity"]
        != result["candidate_manyforge_source_identity"]
        or review["candidate_specs_source_identity"]
        != result["candidate_specs_source_identity"]
    ):
        fail("independent adversarial review is not bound to the exact passing result")
    _require_text(review["reviewer"], "independent adversarial review reviewer")
    reviewed_at = _require_text(
        review["reviewed_at_utc"], "independent adversarial review timestamp"
    )
    if not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z", reviewed_at):
        fail("independent adversarial review timestamp is not canonical UTC")
    scope = _require_object(review["scope"], "independent adversarial review scope")
    _exact_keys(
        scope,
        {
            "checks",
            "reviewed_lane_ids",
            "reviewed_evidence_fields",
            "reviewed_log_count",
            "finding_policy",
        },
        "independent adversarial review scope",
    )
    expected_lane_ids = [row["id"] for row in result["lane_exit_statuses"]]
    if scope != {
        "checks": REQUIRED_REVIEW_CHECKS,
        "reviewed_lane_ids": expected_lane_ids,
        "reviewed_evidence_fields": sorted(
            REQUIRED_EVIDENCE_FIELDS - {"independent_adversarial_review"}
        ),
        "reviewed_log_count": len(result["log_sha256"]),
        "finding_policy": (
            "blocker/high findings must be resolved; medium/low may be "
            "resolved or accepted_non_blocking; unresolved findings forbid pass"
        ),
    }:
        fail("independent adversarial review scope is not the exact checklist")
    findings = _require_list(review["findings"], "independent adversarial review findings")
    finding_ids: set[str] = set()
    derived_unresolved: set[str] = set()
    for index, raw in enumerate(findings):
        finding = _require_object(raw, f"adversarial finding {index}")
        _exact_keys(
            finding,
            {"id", "severity", "disposition", "evidence"},
            f"adversarial finding {index}",
        )
        finding_id = _require_text(finding["id"], f"adversarial finding {index} id")
        if finding_id in finding_ids:
            fail(f"adversarial finding is duplicated: {finding_id}")
        finding_ids.add(finding_id)
        if finding["severity"] not in {"blocker", "high", "medium", "low"}:
            fail(f"adversarial finding has an unknown severity: {finding_id}")
        disposition = finding["disposition"]
        if disposition not in {"resolved", "accepted_non_blocking", "unresolved"}:
            fail(f"adversarial finding has an unknown disposition: {finding_id}")
        if finding["severity"] in {"blocker", "high"} and disposition != "resolved":
            fail(f"adversarial blocker/high finding is not resolved: {finding_id}")
        if disposition == "accepted_non_blocking" and finding["severity"] not in {
            "medium",
            "low",
        }:
            fail(f"only medium/low findings may be accepted non-blocking: {finding_id}")
        if disposition == "unresolved":
            derived_unresolved.add(finding_id)
        _require_text(finding["evidence"], f"adversarial finding {index} evidence")
    unresolved_raw = _require_list(
        review["unresolved_findings"], "independent adversarial unresolved findings"
    )
    unresolved = _unique_text_set(
        unresolved_raw, "independent adversarial unresolved findings"
    )
    if unresolved != derived_unresolved or unresolved:
        fail("independent adversarial review has unresolved findings")
    return review


def verify_and_close_result(
    manifest: dict[str, Any],
    lanes: dict[str, dict[str, Any]],
    result_path: Path,
    review_path: Path,
    repository_root: Path,
) -> Path:
    output_root = _safe_output_root(repository_root)
    result_path = _require_contained_regular_file(
        result_path, output_root, "qualification result"
    )
    if result_path.name != "qualification-result.json":
        fail("qualification result must use the canonical filename")
    run_root = result_path.parent
    artifact_root = run_root / "artifacts"
    log_root = run_root / "logs"
    if (
        not artifact_root.is_dir()
        or artifact_root.is_symlink()
        or not log_root.is_dir()
        or log_root.is_symlink()
    ):
        fail("qualification run is missing safe artifact/log directories")
    review_path = _require_contained_regular_file(
        review_path, run_root, "independent adversarial review"
    )
    result = _read_json_object(result_path, "qualification result")
    expected_result_keys = REQUIRED_EVIDENCE_FIELDS | {
        "schema_version",
        "artifact_validation_error",
        "automatic_lanes_passed",
        "complete",
    }
    _exact_keys(result, expected_result_keys, "qualification result")
    manifest_path = repository_root / "verification/composable_cell_i2/qualification_manifest.v1.json"
    manifest_sha256 = _sha256_file(manifest_path)
    verifier_sha256 = _sha256_file(Path(__file__).resolve())
    if (
        result["schema_version"] != RESULT_SCHEMA
        or result["manifest_sha256"] != manifest_sha256
        or result["verifier_sha256"] != verifier_sha256
        or result["automatic_lanes_passed"] is not True
        or result["artifact_validation_error"] is not None
        or result["complete"] is not False
        or result["independent_adversarial_review"] is not None
    ):
        fail("qualification result is not an exact automatically passing pre-review result")
    packet_lineage = _validate_historical_packet_lineage(manifest, repository_root)
    if result["historical_packet_lineage"] != packet_lineage:
        fail("qualification result historical packet lineage drifted")
    manyforge_identity = _validate_source_identity(
        result["candidate_manyforge_source_identity"],
        repository=repository_root,
        label="qualification result ManyForge source",
    )
    specs_root = (repository_root / "../manyforge_specs").resolve()
    specs_identity = _validate_source_identity(
        result["candidate_specs_source_identity"],
        repository=specs_root,
        label="qualification result Specs source",
    )
    _require_commit_descends_from(
        repository_root,
        candidate=manyforge_identity["head_commit"],
        base=IMPLEMENTATION_BASES["manyforge"]["head_commit"],
        label="ManyForge",
    )
    _require_commit_descends_from(
        specs_root,
        candidate=specs_identity["head_commit"],
        base=IMPLEMENTATION_BASES["manyforge_specs"]["head_commit"],
        label="Specs",
    )
    identity_tool = _require_contained_regular_file(
        repository_root / manifest["candidate_identity"]["capture_tool"],
        repository_root,
        "candidate identity capture tool",
    )
    if capture_source_identity(identity_tool, repository_root) != result[
        "candidate_manyforge_source_identity"
    ]:
        fail("closure must run from the exact clean reviewed ManyForge candidate")
    if capture_source_identity(identity_tool, specs_root) != result[
        "candidate_specs_source_identity"
    ]:
        fail("closure must run from the exact clean reviewed Specs candidate")
    snapshot_root = run_root / "source"
    snapshot_evidence = _validate_candidate_snapshot(
        snapshot_root,
        run_root,
        CAPTURE_TOOL,
        result["candidate_manyforge_source_identity"],
        manifest["pinned_inputs"]["roboplan"],
    )
    if result["candidate_source_snapshot"] != snapshot_evidence:
        fail("qualification result candidate snapshot evidence drifted")
    _validate_result_tracked_digests(manifest, result, repository_root)

    expected_image = manifest["qualification_environment"]["image"]
    if result["resolved_image_inspection"] != {
        "Id": expected_image["id"],
        "Os": expected_image["os"],
        "Architecture": expected_image["architecture"],
    }:
        fail("qualification result image inspection drifted")
    python = manifest["qualification_environment"]["python"]
    expected_probe = {
        "machine": manifest["qualification_environment"]["machine"],
        "python": python["version"],
        "cache_tag": python["cache_tag"],
        "soabi": python["soabi"],
        "extension_suffix": python["extension_suffix"],
        "multiarch": python["multiarch"],
        "address_bits": manifest["qualification_environment"]["address_bits"],
        "glibc": manifest["qualification_environment"]["glibc"],
        "cmake": manifest["qualification_environment"]["cmake"],
        "ninja": manifest["qualification_environment"]["ninja"],
        "cxx_compiler": manifest["qualification_environment"]["cxx_compiler"],
        "py_debug": python["py_debug"],
        "gil_disabled": python["gil_disabled"],
        "pybind11": manifest["qualification_environment"]["pybind11"]["version"],
        "pybind11_cmake_dir": manifest["qualification_environment"]["pybind11"][
            "cmake_dir"
        ],
        "pinocchio": manifest["pinned_inputs"]["consumer_stack"]["pinocchio"],
        "coal": manifest["pinned_inputs"]["consumer_stack"]["coal"],
        "check_urdf_sha256": manifest["pinned_inputs"]["consumer_stack"][
            "check_urdf_sha256"
        ],
    }
    if result["abi_and_dependency_probe"] != expected_probe:
        fail("qualification result ABI/dependency probe drifted")

    expected_argv = [
        render_lane(
            manifest,
            lane,
            snapshot_root,
            _lane_artifact_root(artifact_root, lane["id"]),
            artifact_root,
        )
        for lane in lanes.values()
    ]
    if result["exact_lane_argv"] != expected_argv:
        fail("qualification result did not execute the exact complete lane argv")
    expected_statuses = [{"id": lane_id, "exit_status": 0} for lane_id in lanes]
    if result["lane_exit_statuses"] != expected_statuses:
        fail("qualification result lane status inventory is incomplete or nonzero")
    seal_root = run_root / "artifact-seals"
    recalculated_seals = _validate_lane_artifact_seals(
        artifact_root, seal_root, set(lanes)
    )
    if result["lane_artifact_seals"] != recalculated_seals:
        fail("qualification result lane artifact seals drifted")
    log_rows = _require_list(result["log_sha256"], "qualification log evidence")
    if len(log_rows) != len(lanes):
        fail("qualification result log inventory is incomplete")
    for lane, row in zip(lanes.values(), log_rows, strict=True):
        if not isinstance(row, dict) or set(row) != {
            "id",
            "path",
            "sha256",
            "size",
        }:
            fail(f"qualification log evidence is malformed for {lane['id']}")
        expected_relative = f"logs/{lane['ordinal']:02d}-{lane['id']}.log"
        if row["path"] != expected_relative:
            fail(f"qualification log path drifted for {lane['id']}")
        path = _require_output_file(
            log_root,
            f"{lane['ordinal']:02d}-{lane['id']}.log",
            f"qualification lane log {lane['id']}",
        )
        if (
            row["id"] != lane["id"]
            or row["sha256"] != _sha256_file(path)
            or row["size"] != path.stat().st_size
        ):
            fail(f"qualification log evidence drifted for {lane['id']}")
    recalculated = _collect_run_artifacts(
        manifest,
        artifact_root,
        log_root,
        set(lanes),
        snapshot_root,
        result["candidate_manyforge_source_identity"],
        result["candidate_specs_source_identity"],
    )
    for field, value in recalculated.items():
        if result[field] != value:
            fail(f"qualification result artifact evidence drifted: {field}")
    final_identity = _require_object(
        result["final_unchanged_source_identity"], "final unchanged source identity"
    )
    _exact_keys(
        final_identity,
        {"passed", "in_container_equal", "manyforge", "manyforge_specs"},
        "final unchanged source identity",
    )
    if (
        final_identity["passed"] is not True
        or final_identity["in_container_equal"] is not True
        or final_identity["manyforge"] != result["candidate_manyforge_source_identity"]
        or final_identity["manyforge_specs"] != result["candidate_specs_source_identity"]
    ):
        fail("qualification source identity changed during the cumulative run")
    review = _validate_adversarial_review(
        review_path,
        result_path,
        manifest_sha256,
        verifier_sha256,
        result,
    )
    final_seals = _validate_lane_artifact_seals(
        artifact_root, seal_root, set(lanes)
    )
    if final_seals != recalculated_seals or final_seals != result[
        "lane_artifact_seals"
    ]:
        fail("qualification lane artifacts changed during closure recomputation")
    closure = {
        "schema_version": CLOSURE_SCHEMA,
        "manifest_sha256": manifest_sha256,
        "verifier_sha256": verifier_sha256,
        "historical_packet_lineage": packet_lineage,
        "qualification_result": _file_evidence(result_path, run_root),
        "independent_adversarial_review": _file_evidence(review_path, run_root),
        "candidate_manyforge_source_identity": result[
            "candidate_manyforge_source_identity"
        ],
        "candidate_specs_source_identity": result["candidate_specs_source_identity"],
        "reviewer": review["reviewer"],
        "reviewed_at_utc": review["reviewed_at_utc"],
        "complete": True,
    }
    closure_path = run_root / "qualification-closure.json"
    _write_exclusive_json(run_root, closure_path.name, closure)
    print(json.dumps(closure, indent=2, sort_keys=True))
    return closure_path


def repository_root_for(manifest_path: Path) -> Path:
    resolved = manifest_path.resolve()
    expected_tail = Path("verification/composable_cell_i2/qualification_manifest.v1.json")
    if tuple(resolved.parts[-len(expected_tail.parts) :]) != expected_tail.parts:
        fail("manifest must use the canonical v1 repository path")
    return resolved.parents[2]


def _binding_result_row(manifest: dict[str, Any], lane_id: str) -> dict[str, Any]:
    rows = {
        row["lane_id"]: row
        for row in manifest["test_inventory_contract"]["binding_result_lanes"]
    }
    try:
        return rows[lane_id]
    except KeyError:
        fail(f"unknown frozen binding lane: {lane_id}")


def _binding_build_row(manifest: dict[str, Any], lane_id: str) -> dict[str, Any]:
    rows = {
        row["lane_id"]: row
        for row in manifest["test_inventory_contract"]["binding_build_lanes"]
    }
    try:
        return rows[lane_id]
    except KeyError:
        fail(f"binding lane has no frozen builder contract: {lane_id}")


def _binding_case_nodes(row: dict[str, Any]) -> list[str]:
    return [f"{BINDING_NODE_PREFIX}{case_id}" for case_id in row["required_case_ids"]]


def _binding_pytest_targets(row: dict[str, Any]) -> list[str]:
    targets: list[str] = []
    if row["require_python_baseline"]:
        targets.extend(sorted(REQUIRED_EXISTING_BINDING_TESTS))
    targets.extend(_binding_case_nodes(row))
    return targets


def _binding_collection_argv(row: dict[str, Any]) -> list[str]:
    return [
        "/usr/local/bin/python3",
        "-m",
        "pytest",
        "-p",
        "no:cacheprovider",
        "--collect-only",
        "-q",
        *_binding_pytest_targets(row),
    ]


def _binding_run_argv(row: dict[str, Any]) -> list[str]:
    return [
        "/usr/local/bin/python3",
        "-m",
        "pytest",
        "-p",
        "no:cacheprovider",
        "-q",
        f"--junitxml=/qualification-output/{row['result_output']}",
        *_binding_pytest_targets(row),
    ]


def _binding_extension_host_path(artifact_root: Path, lane_id: str) -> Path:
    stage = Path(BINDING_LANE_STAGE_ROOTS[lane_id])
    if stage.is_relative_to(Path("/qualification-output")):
        root = _lane_artifact_root(artifact_root, lane_id)
        relative = stage.relative_to("/qualification-output")
    elif stage.is_relative_to(Path("/qualification-input/bindings_release")):
        root = _lane_artifact_root(artifact_root, "bindings_release")
        relative = stage.relative_to("/qualification-input/bindings_release")
    else:
        fail(f"binding stage path has no frozen host mapping: {lane_id}")
    return root / relative / BINDING_EXTENSION_NAME


def _ensure_container_output_directory(
    output_root: Path, relative: Path, label: str, *, must_be_new: bool = False
) -> Path:
    if relative.is_absolute() or ".." in relative.parts:
        fail(f"{label} is not a safe output-relative directory")
    path = output_root / relative
    if must_be_new:
        try:
            path.mkdir(parents=True, exist_ok=False)
        except OSError as error:
            fail(f"cannot create fresh {label}: {error}")
    else:
        try:
            path.mkdir(parents=True, exist_ok=True)
        except OSError as error:
            fail(f"cannot create {label}: {error}")
    current = output_root
    for part in relative.parts:
        current = current / part
        try:
            metadata = current.lstat()
        except OSError as error:
            fail(f"{label} is unavailable: {error}")
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
            fail(f"{label} contains a non-directory or symlink: {current}")
    return path


def _binding_test_environment(
    lane_id: str,
    stage_root: Path,
    evidence_root: Path,
    generated_root: Path,
) -> dict[str, str]:
    process_home = evidence_root.parent / "process-home"
    process_home.mkdir(parents=False, exist_ok=False)
    environment: dict[str, str] = {}
    extension = stage_root / BINDING_EXTENSION_NAME
    environment.update(
        {
            "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
            "LANG": "C.UTF-8",
            "LC_ALL": "C.UTF-8",
            "TZ": "UTC",
            "HOME": str(process_home),
            "TMPDIR": str(process_home),
            "XDG_CACHE_HOME": str(process_home / "xdg-cache"),
            "PYTHONNOUSERSITE": "1",
            "PYTHONDONTWRITEBYTECODE": "1",
            "PYTEST_DISABLE_PLUGIN_AUTOLOAD": "1",
            "PYTHONPATH": str(stage_root),
            "MANYFORGE_I2_BINDING_EXTENSION": str(extension),
            "MANYFORGE_I2_BINDING_EVIDENCE_ROOT": str(evidence_root),
            "MANYFORGE_I2_BINDING_STAGED_ROOT": str(stage_root),
            "MANYFORGE_I2_BINDING_GENERATED_ROOT": str(generated_root),
            "MANYFORGE_I2_BINDING_SOURCE_ROOT": "/workspace",
        }
    )
    if lane_id == "bindings_asan_ubsan":
        environment.update(NATIVE_SANITIZER_ENVIRONMENT["address+undefined"])
        environment["LD_PRELOAD"] = "/usr/lib/x86_64-linux-gnu/libasan.so.8"
    elif lane_id == "bindings_tsan":
        environment.update(NATIVE_SANITIZER_ENVIRONMENT["thread"])
        environment["TSAN_SUPPRESSIONS"] = "/workspace/scripts/ci/tsan.supp"
        environment["LD_PRELOAD"] = "/usr/lib/x86_64-linux-gnu/libtsan.so.2"
    return environment


def execute_binding_lane(
    manifest: dict[str, Any],
    lane_id: str,
    repository_root: Path,
    requested_output_root: Path,
) -> int:
    """Build inputs, then let the packet own binding semantics/evidence."""
    if lane_id not in BINDING_RESULT_LANES:
        fail(f"unknown frozen binding lane: {lane_id}")
    if repository_root.resolve() != Path("/workspace"):
        fail("binding packet execution must run from the readonly /workspace snapshot")
    try:
        output_root = requested_output_root.resolve(strict=True)
        output_metadata = output_root.lstat()
    except OSError as error:
        fail(f"binding qualification output root is absent: {error}")
    if (
        output_root != Path("/qualification-output")
        or stat.S_ISLNK(output_metadata.st_mode)
        or not stat.S_ISDIR(output_metadata.st_mode)
    ):
        fail("binding qualification output root is not the exact safe artifact mount")

    row = _binding_result_row(manifest, lane_id)
    preparation_argv = BINDING_LANE_PREPARATION.get(lane_id, [])
    preparation_home = output_root / "binding-scratch" / lane_id / "preparation-home"
    preparation_home.mkdir(parents=True, exist_ok=False)
    preparation_environment = {
        "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "TZ": "UTC",
        "HOME": str(preparation_home),
        "TMPDIR": str(preparation_home),
        "XDG_CACHE_HOME": str(preparation_home / "xdg-cache"),
        "PYTHONNOUSERSITE": "1",
        "PYTHONDONTWRITEBYTECODE": "1",
        "CMAKE_BUILD_PARALLEL_LEVEL": "2",
    }
    preparation_status = 0
    if preparation_argv:
        preparation = subprocess.run(
            preparation_argv,
            cwd=repository_root,
            env=preparation_environment,
            check=False,
        )
        preparation_status = preparation.returncode
        if preparation_status != 0:
            return preparation_status

    stage_root = Path(BINDING_LANE_STAGE_ROOTS[lane_id])
    try:
        stage_metadata = stage_root.lstat()
    except OSError as error:
        fail(f"binding stage root is absent after preparation: {error}")
    if stat.S_ISLNK(stage_metadata.st_mode) or not stat.S_ISDIR(stage_metadata.st_mode):
        fail("binding stage root is not one plain directory")
    extension = _require_contained_regular_file(
        stage_root / BINDING_EXTENSION_NAME,
        stage_root,
        f"{lane_id} exact staged extension",
        require_single_link=True,
    )
    staged_files = sorted(
        path
        for path in stage_root.rglob("*")
        if path.is_file() and not path.is_symlink()
    )
    if staged_files != [extension]:
        fail(f"{lane_id} extension-only stage contains extra files")

    generated_value = BINDING_LANE_GENERATED_ROOTS.get(lane_id)
    if generated_value is None:
        generated_root = _ensure_container_output_directory(
            output_root,
            Path("binding-scratch") / lane_id / "generated",
            f"{lane_id} unused generated root",
            must_be_new=True,
        )
    else:
        generated_root = Path(generated_value)
        try:
            generated_metadata = generated_root.lstat()
        except OSError as error:
            fail(f"{lane_id} generated root is absent after preparation: {error}")
        if stat.S_ISLNK(generated_metadata.st_mode) or not stat.S_ISDIR(
            generated_metadata.st_mode
        ):
            fail(f"{lane_id} generated root is unsafe")
    if lane_id in {"bindings_selector", "bindings_release"}:
        generated_extension = _require_contained_regular_file(
            generated_root / BINDING_EXTENSION_NAME,
            generated_root,
            f"{lane_id} generated extension",
            require_single_link=True,
        )
        if _sha256_file(generated_extension) != _sha256_file(extension):
            fail(f"{lane_id} stage and generated extension bytes differ")

    producer_lane_id = row["producer_lane_id"]
    producer_build_row = _binding_build_row(manifest, producer_lane_id)
    build_provenance_relative = _safe_relative_path(
        producer_build_row["provenance_output"],
        f"{lane_id} binding build provenance",
    )
    build_contract = _load_build_provenance_contract(
        manifest["test_inventory_contract"], manifest, repository_root
    )
    owns_build = producer_lane_id == lane_id
    if owns_build:
        if row["build_provenance_output"] != build_provenance_relative.as_posix():
            fail(f"{lane_id} binding provenance output routing drifted")
        provenance_parent = _ensure_container_output_directory(
            output_root,
            build_provenance_relative.parent,
            f"{lane_id} binding provenance parent",
        )
        if (output_root / build_provenance_relative).exists() or (
            output_root / build_provenance_relative
        ).is_symlink():
            fail(f"{lane_id} candidate precreated packet-owned build provenance")
        try:
            build_provenance = validate_binding_build_provenance(
                repository_root=repository_root,
                output_root=output_root,
                lane=producer_build_row,
                staged_extension=extension,
                binding_contract=build_contract,
            )
        except NativeBuildProvenanceError as error:
            fail(f"{lane_id} binding build provenance failed: {error}")
        build_provenance_path = _write_exclusive_json(
            provenance_parent,
            build_provenance_relative.name,
            build_provenance,
        )
    else:
        if row["build_provenance_output"] is not None:
            fail(f"{lane_id} consumer may not publish binding build provenance")
        unresolved_producer_input = Path("/qualification-input") / producer_lane_id
        try:
            producer_input_metadata = unresolved_producer_input.lstat()
            producer_input_root = unresolved_producer_input.resolve(strict=True)
        except OSError as error:
            fail(f"{lane_id} binding producer input is absent: {error}")
        expected_input_root = Path("/qualification-input/bindings_release")
        if (
            producer_input_root != expected_input_root
            or stat.S_ISLNK(producer_input_metadata.st_mode)
            or not stat.S_ISDIR(producer_input_metadata.st_mode)
        ):
            fail(f"{lane_id} binding producer input root drifted")
        build_provenance_path = _require_contained_regular_file(
            producer_input_root / build_provenance_relative,
            producer_input_root,
            f"{lane_id} release binding build provenance",
            require_single_link=True,
        )
        build_provenance = _read_json_object(
            build_provenance_path,
            f"{lane_id} release binding build provenance",
        )
    build_provenance_evidence_sha256 = _require_text(
        build_provenance.get("evidence_sha256"),
        f"{lane_id} binding build provenance evidence digest",
    )
    build_digest_input = dict(build_provenance)
    del build_digest_input["evidence_sha256"]
    if (
        build_provenance.get("schema_version")
        != "manyforge.composable_cell_i2.binding_build_provenance.v1"
        or build_provenance.get("lane_id") != producer_lane_id
        or not HEX64.fullmatch(build_provenance_evidence_sha256)
        or _canonical_json_sha256(build_digest_input)
        != build_provenance_evidence_sha256
    ):
        fail(f"{lane_id} binding build provenance identity drifted")
    build_provenance_sha256 = _sha256_file(build_provenance_path)

    collection_relative = _safe_relative_path(
        row["collection_output"], f"{lane_id} collection output"
    )
    result_relative = _safe_relative_path(
        row["result_output"], f"{lane_id} JUnit output"
    )
    witness_relative = _safe_relative_path(
        row["witness_output"], f"{lane_id} witness output"
    )
    for relative, label in (
        (collection_relative, "collection"),
        (result_relative, "JUnit"),
        (witness_relative, "witness"),
    ):
        _ensure_container_output_directory(
            output_root, relative.parent, f"{lane_id} {label} parent"
        )
        if (output_root / relative).exists() or (output_root / relative).is_symlink():
            fail(f"{lane_id} candidate precreated packet-owned {label} output")
    evidence_root = _ensure_container_output_directory(
        output_root,
        witness_relative.parent / "direct-evidence",
        f"{lane_id} direct evidence root",
        must_be_new=True,
    )
    environment = _binding_test_environment(
        lane_id, stage_root, evidence_root, generated_root
    )

    collection_argv = _binding_collection_argv(row)
    collection = subprocess.run(
        collection_argv,
        cwd=repository_root,
        env=environment,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if collection.stdout:
        sys.stdout.write(collection.stdout)
        sys.stdout.flush()
    if collection.returncode != 0:
        return collection.returncode
    collection_path = output_root / collection_relative
    with _exclusive_output_file(
        collection_path.parent, collection_path.name, binary=False
    ) as destination:
        destination.write(collection.stdout)

    run_argv = _binding_run_argv(row)
    completed = subprocess.run(
        run_argv,
        cwd=repository_root,
        env=environment,
        check=False,
    )
    if completed.returncode != 0:
        return completed.returncode
    result_path = _require_output_file(
        output_root, result_relative.as_posix(), f"{lane_id} frozen pytest JUnit"
    )
    collection_path = _require_output_file(
        output_root,
        collection_relative.as_posix(),
        f"{lane_id} frozen pytest collection",
    )

    expected_evidence_names = {
        f"{case_id}.json" for case_id in row["required_case_ids"]
    }
    actual_evidence_names = {entry.name for entry in evidence_root.iterdir()}
    if actual_evidence_names != expected_evidence_names:
        fail(f"{lane_id} direct evidence file set drifted")
    if owns_build:
        try:
            revalidated_build_provenance = validate_binding_build_provenance(
                repository_root=repository_root,
                output_root=output_root,
                lane=producer_build_row,
                staged_extension=extension,
                binding_contract=build_contract,
            )
        except NativeBuildProvenanceError as error:
            fail(f"{lane_id} post-execution binding provenance failed: {error}")
        if (
            revalidated_build_provenance != build_provenance
            or _read_json_object(
                build_provenance_path,
                f"{lane_id} persisted binding build provenance",
            )
            != build_provenance
            or _sha256_file(build_provenance_path) != build_provenance_sha256
        ):
            fail(f"{lane_id} binding build provenance changed during test execution")
    elif _sha256_file(build_provenance_path) != build_provenance_sha256:
        fail(f"{lane_id} release binding provenance changed during test execution")
    extension_sha256 = _sha256_file(extension)
    cases: list[dict[str, Any]] = []
    for case_id in row["required_case_ids"]:
        evidence_path = _require_contained_regular_file(
            evidence_root / f"{case_id}.json",
            evidence_root,
            f"{lane_id} direct evidence {case_id}",
            require_single_link=True,
        )
        evidence = _read_json_object(
            evidence_path, f"{lane_id} direct evidence {case_id}"
        )
        _exact_keys(
            evidence,
            {
                "schema_version",
                "case_id",
                "node_id",
                "extension_sha256",
                "measurements",
                "observations",
            },
            f"{lane_id} direct evidence {case_id}",
        )
        measurements = _require_object(
            evidence["measurements"], f"{lane_id} {case_id} measurements"
        )
        recalculated = _recalculate_binding_measurements(
            case_id, evidence["observations"]
        )
        if (
            evidence["schema_version"]
            != "manyforge.composable_cell_i2.direct_binding_case.v1"
            or evidence["case_id"] != case_id
            or evidence["node_id"] != f"{BINDING_NODE_PREFIX}{case_id}"
            or evidence["extension_sha256"] != extension_sha256
            or measurements != recalculated
        ):
            fail(f"{lane_id} direct evidence identity/measurements drifted: {case_id}")
        _validate_binding_measurements(case_id, recalculated)
        evidence_relative = evidence_path.relative_to(output_root).as_posix()
        cases.append(
            {
                "case_id": case_id,
                "node_ids": [f"{BINDING_NODE_PREFIX}{case_id}"],
                "measurements": recalculated,
                "evidence_files": [
                    {
                        "role": "direct_case_evidence",
                        **_file_evidence(evidence_path, output_root),
                    }
                ],
            }
        )
        if cases[-1]["evidence_files"][0]["path"] != evidence_relative:
            fail(f"{lane_id} direct evidence path derivation failed")

    witness = {
        "schema_version": "manyforge.composable_cell_i2.binding_witness.v1",
        "lane_id": lane_id,
        "framework": "packet-owned-pytest-junit",
        "preparation_argv": preparation_argv,
        "preparation_exit_status": preparation_status,
        "collection_argv": collection_argv,
        "collection_sha256": _sha256_file(collection_path),
        "child_argv": run_argv,
        "child_exit_status": completed.returncode,
        "junit_sha256": _sha256_file(result_path),
        "extension_path": str(extension),
        "extension_sha256": extension_sha256,
        "producer_lane_id": producer_lane_id,
        "build_provenance_path": build_provenance_relative.as_posix(),
        "build_provenance_sha256": build_provenance_sha256,
        "build_provenance_evidence_sha256": build_provenance_evidence_sha256,
        "case_witnesses": cases,
    }
    _write_exclusive_json(
        (output_root / witness_relative).parent,
        witness_relative.name,
        witness,
    )
    return 0


def _strict_json_file(path: Path, label: str) -> Any:
    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_object_without_duplicates,
            parse_constant=lambda token: fail(f"{label} contains {token}"),
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"{label} is not strict UTF-8 JSON: {error}")


def _framed_content_identity(
    document: dict[str, Any],
    *,
    identity_domain: str,
    api_version: str,
    own_field: str,
    label: str,
) -> str:
    if document.get("api_version") != api_version:
        fail(f"{label} api_version drifted")
    declared = document.get(own_field)
    if not isinstance(declared, str) or not declared.startswith("sha256:") or not HEX64.fullmatch(
        declared[7:]
    ):
        fail(f"{label} own identity is malformed")
    payload = {key: value for key, value in document.items() if key != own_field}
    try:
        canonical = json.dumps(
            payload,
            ensure_ascii=False,
            allow_nan=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        fail(f"{label} cannot be canonically serialized: {error}")
    preimage = (
        b"manyforge.content-identity.v1\0"
        + identity_domain.encode("ascii")
        + b"\0"
        + api_version.encode("ascii")
        + b"\0"
        + canonical
    )
    computed = "sha256:" + hashlib.sha256(preimage).hexdigest()
    if declared != computed:
        fail(f"{label} content identity does not match its exact document")
    return computed


def _determinism_raw_paths(fixture_id: str) -> dict[str, str]:
    prefix = f"fixture-raw/{fixture_id}"
    return {
        "canonical_input": f"{prefix}/canonical-input.json",
        "compiler_ir": f"{prefix}/compiler-ir.json",
        "canonical_projection": f"{prefix}/canonical-projection.json",
    }


def _determinism_fixture_contract(
    manifest: dict[str, Any], fixture_id: str
) -> tuple[list[str], list[str]]:
    fixtures = {fixture["id"]: fixture for fixture in manifest["fixture_matrix"]}
    fixture = fixtures[fixture_id]
    native_groups = sorted(
        {row["group_id"] for row in fixture["native_test_witnesses"]}
    )
    binding_cases = sorted(
        {
            case_id
            for row in fixture["binding_case_witnesses"]
            for case_id in row["case_ids"]
        }
    )
    return native_groups, binding_cases


def _validate_and_index_determinism_products(
    root: Path,
    patterns: list[str],
    label: str,
) -> list[dict[str, Any]]:
    rows: dict[str, dict[str, Any]] = {}
    seen_inodes: set[tuple[int, int]] = set()
    for pattern in patterns:
        matches = sorted(
            path for path in root.glob(pattern) if path.is_file()
        )
        if not matches:
            fail(f"{label} pattern has no products: {pattern}")
        for candidate in matches:
            path = _require_contained_regular_file(
                candidate,
                root,
                f"{label} product {candidate}",
                require_single_link=True,
            )
            relative = path.relative_to(root).as_posix()
            if relative in rows:
                continue
            inode = (path.stat().st_dev, path.stat().st_ino)
            if inode in seen_inodes:
                fail(f"{label} reuses one product inode: {relative}")
            seen_inodes.add(inode)
            if path.stat().st_size == 0:
                fail(f"{label} product is empty: {relative}")
            rows[relative] = {
                "path": relative,
                "sha256": _sha256_file(path),
                "size": path.stat().st_size,
            }
    return [rows[path] for path in sorted(rows)]


def execute_determinism_build(
    manifest: dict[str, Any],
    lane_id: str,
    repository_root: Path,
    requested_output_root: Path,
) -> int:
    if lane_id not in DETERMINISM_BUILD_LANES:
        fail(f"unknown frozen determinism build lane: {lane_id}")
    if repository_root.resolve() != Path("/workspace"):
        fail("determinism build must run from the readonly /workspace snapshot")
    try:
        output_root = requested_output_root.resolve(strict=True)
        metadata = output_root.lstat()
    except OSError as error:
        fail(f"determinism output root is absent: {error}")
    if output_root != Path("/qualification-output") or stat.S_ISLNK(
        metadata.st_mode
    ) or not stat.S_ISDIR(metadata.st_mode):
        fail("determinism output root is not the exact lane mount")
    build_id, logical_root = DETERMINISM_BUILD_LANES[lane_id]
    preparation_argv = DETERMINISM_PREPARATION[lane_id]
    environment = os.environ.copy()
    for name in (
        "PYTHONPATH",
        "PYTHONHOME",
        "ASAN_OPTIONS",
        "UBSAN_OPTIONS",
        "TSAN_OPTIONS",
        "LD_PRELOAD",
    ):
        environment.pop(name, None)
    environment.update(
        {
            "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
            "LC_ALL": "C.UTF-8",
            "PYTHONNOUSERSITE": "1",
            "PYTHONDONTWRITEBYTECODE": "1",
            "CMAKE_BUILD_PARALLEL_LEVEL": "2",
        }
    )
    completed = subprocess.run(
        preparation_argv,
        cwd=repository_root,
        env=environment,
        check=False,
    )
    if completed.returncode != 0:
        return completed.returncode
    root = output_root / logical_root
    if not root.is_dir() or root.is_symlink():
        fail(f"{lane_id} helper omitted its exact product root")

    source_commit = _git_head(repository_root)
    source_tree = _git_tree(repository_root, source_commit)
    fixture_root = root / "fixture-ir"
    try:
        fixture_root.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        fail(f"{lane_id} candidate precreated or blocked fixture witnesses: {error}")
    for fixture_id, witness_relative in manifest["rotating_identities"][
        "fixture_ir_outputs"
    ].items():
        raw_paths = _determinism_raw_paths(fixture_id)
        digests: dict[str, str] = {}
        for role, relative in raw_paths.items():
            path = _require_contained_regular_file(
                root / relative,
                root,
                f"{lane_id} {fixture_id} {role}",
                require_single_link=True,
            )
            _strict_json_file(path, f"{lane_id} {fixture_id} {role}")
            digests[role] = "sha256:" + _sha256_file(path)
        native_groups, binding_cases = _determinism_fixture_contract(
            manifest, fixture_id
        )
        witness = {
            "schema_version": FIXTURE_IR_WITNESS_SCHEMA,
            "fixture_id": fixture_id,
            "producer": "manyforge-i2-packet-determinism-executor",
            "producer_source_commit": source_commit,
            "producer_source_tree": source_tree,
            "native_witness_groups": native_groups,
            "binding_case_ids": binding_cases,
            "raw_artifacts": raw_paths,
            "canonical_input_sha256": digests["canonical_input"],
            "compiler_ir_sha256": digests["compiler_ir"],
            "canonical_projection_sha256": digests["canonical_projection"],
        }
        expected_relative = Path(witness_relative)
        if expected_relative.parent != Path("fixture-ir"):
            fail("fixture witness output escaped its frozen directory")
        _write_exclusive_json(fixture_root, expected_relative.name, witness)

    products = _validate_and_index_determinism_products(
        root,
        manifest["rotating_identities"]["regression_products"],
        lane_id,
    )
    index = {
        "schema_version": "manyforge.composable_cell_i2.determinism_build.v1",
        "lane_id": lane_id,
        "build_id": build_id,
        "logical_root": logical_root,
        "preparation_argv": preparation_argv,
        "preparation_exit_status": completed.returncode,
        "producer_source_commit": source_commit,
        "producer_source_tree": source_tree,
        "product_count": len(products),
        "products_sha256": _canonical_json_sha256(products),
        "products": products,
    }
    _write_exclusive_json(root, "build-index.json", index)
    return 0


def _determinism_input_root(logical_root: str) -> Path:
    lane_id = DETERMINISM_ROOT_LANES[logical_root]
    return Path("/qualification-input") / lane_id / logical_root


def _recompute_package_identities(package_root: Path, label: str) -> dict[str, str]:
    admission_path = _require_contained_regular_file(
        package_root / "canonical-empty-admission.json",
        package_root,
        f"{label} admission",
        require_single_link=True,
    )
    admission = _require_object(
        _strict_json_file(admission_path, f"{label} admission"),
        f"{label} admission",
    )
    admission_digest = _framed_content_identity(
        admission,
        identity_domain="canonical-empty-admission",
        api_version="manyforge.canonical_empty_admission.v1",
        own_field="admission_digest",
        label=f"{label} admission",
    )
    specifications = {
        "node_environment_digest": (
            "environment_digest",
            "node_environment",
            "node-environment-digest",
            "manyforge.node_environment_manifest.v1",
        ),
        "compiler_contract_digest": (
            "compiler_contract_digest",
            "compiler_contract",
            "compiler-contract-digest",
            "manyforge.compiler_contract_manifest.v1",
        ),
        "release_compiler_build_digest": (
            "compiler_build_digest",
            "compiler_build",
            "compiler-build-digest",
            "manyforge.compiler_build_manifest.v1",
        ),
    }
    result: dict[str, str] = {}
    for identity_name, (field, directory, domain, api_version) in specifications.items():
        declared = _require_text(admission.get(field), f"{label} {field}")
        if not declared.startswith("sha256:") or not HEX64.fullmatch(declared[7:]):
            fail(f"{label} {field} is malformed")
        path = _require_contained_regular_file(
            package_root / "build-manifests" / directory / f"{declared[7:]}.json",
            package_root,
            f"{label} {directory} manifest",
            require_single_link=True,
        )
        document = _require_object(
            _strict_json_file(path, f"{label} {directory} manifest"),
            f"{label} {directory} manifest",
        )
        computed = _framed_content_identity(
            document,
            identity_domain=domain,
            api_version=api_version,
            own_field=field,
            label=f"{label} {directory} manifest",
        )
        if declared != computed:
            fail(f"{label} admission does not join its {directory} manifest")
        result[identity_name] = computed
    retained = _require_text(
        admission.get("retained_bundle_id"), f"{label} retained bundle ID"
    )
    if not retained.startswith("sha256:") or not HEX64.fullmatch(retained[7:]):
        fail(f"{label} retained bundle ID is malformed")
    retained_path = _require_contained_regular_file(
        package_root / "retained-bundles" / "sha256" / f"{retained[7:]}.json",
        package_root,
        f"{label} retained bundle",
        require_single_link=True,
    )
    retained_document = _require_object(
        _strict_json_file(retained_path, f"{label} retained bundle"),
        f"{label} retained bundle",
    )
    retained_computed = _framed_content_identity(
        retained_document,
        identity_domain="retained-bundle-id",
        api_version="manyforge.retained_bundle.v1",
        own_field="retained_bundle_id",
        label=f"{label} retained bundle",
    )
    if retained != retained_computed:
        fail(f"{label} admission does not join its retained bundle")
    result["canonical_empty_retained_bundle_id"] = retained_computed
    result["canonical_empty_admission_digest"] = admission_digest
    return result


def execute_determinism_compare(
    manifest: dict[str, Any],
    repository_root: Path,
    requested_output_root: Path,
) -> int:
    if repository_root.resolve() != Path("/workspace"):
        fail("determinism comparison must run from readonly /workspace")
    output_root = requested_output_root.resolve(strict=True)
    if output_root != Path("/qualification-output") or output_root.is_symlink():
        fail("determinism comparison output root is not exact")
    roots = manifest["rotating_identities"]["determinism_artifact_roots"]
    product_maps: list[dict[str, dict[str, Any]]] = []
    identities: list[dict[str, str]] = []
    for logical_root in roots:
        root = _determinism_input_root(logical_root)
        index_path = _require_contained_regular_file(
            root / "build-index.json",
            root,
            f"{logical_root} build index",
            require_single_link=True,
        )
        index = _read_json_object(index_path, f"{logical_root} build index")
        _exact_keys(
            index,
            {
                "schema_version",
                "lane_id",
                "build_id",
                "logical_root",
                "preparation_argv",
                "preparation_exit_status",
                "producer_source_commit",
                "producer_source_tree",
                "product_count",
                "products_sha256",
                "products",
            },
            f"{logical_root} build index",
        )
        products = _validate_and_index_determinism_products(
            root,
            manifest["rotating_identities"]["regression_products"],
            logical_root,
        )
        lane_id = DETERMINISM_ROOT_LANES[logical_root]
        build_id = DETERMINISM_BUILD_LANES[lane_id][0]
        if (
            index.get("schema_version")
            != "manyforge.composable_cell_i2.determinism_build.v1"
            or index.get("lane_id") != lane_id
            or index.get("build_id") != build_id
            or index.get("logical_root") != logical_root
            or index.get("preparation_argv") != DETERMINISM_PREPARATION[lane_id]
            or index.get("preparation_exit_status") != 0
            or index.get("producer_source_commit") != _git_head(repository_root)
            or index.get("producer_source_tree")
            != _git_tree(repository_root, _git_head(repository_root))
            or index.get("products") != products
            or index.get("product_count") != len(products)
            or index.get("products_sha256") != _canonical_json_sha256(products)
        ):
            fail(f"{logical_root} packet-owned build index drifted")
        product_maps.append({row["path"]: row for row in products})
        identities.append(
            _recompute_package_identities(
                root / "canonical_empty_product_package", logical_root
            )
        )
    if product_maps[0] != product_maps[1]:
        fail("independently isolated determinism builds differ by path or bytes")
    if identities[0] != identities[1]:
        fail("independently recomputed determinism identities differ")
    candidate = identities[0]
    expectations = manifest["rotating_identities"]["candidate_expectations"]
    baseline = manifest["rotating_identities"]["baseline"]
    for name, relation in expectations.items():
        if (relation == "changed") != (candidate[name] != baseline[name]):
            fail(f"determinism identity relation failed: {name}")
    comparison_root = _ensure_container_output_directory(
        output_root, Path("determinism"), "determinism comparison output"
    )
    comparison = {
        "schema_version": "manyforge.composable_cell_i2.determinism_comparison.v1",
        "artifact_roots": roots,
        "artifacts_byte_identical": True,
        "identity_baseline": baseline,
        "identity_candidate": candidate,
        "identity_relations": expectations,
        "passed": True,
    }
    _write_exclusive_json(comparison_root, "comparison.json", comparison)
    return 0


def _prepare_native_release_lane(
    lane_id: str,
    row: dict[str, Any],
    repository_root: Path,
    output_root: Path,
    environment: dict[str, str],
) -> Path:
    build_root = output_root / row["build_root"]
    query_root = build_root / ".cmake/api/v1/query"
    try:
        query_root.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        fail(f"{lane_id} cannot create its fresh File API query: {error}")
    _write_exclusive_json(query_root, "codemodel-v2", {})
    _write_exclusive_json(query_root, "cmakeFiles-v1", {})
    _write_graphviz_options(build_root)

    common = [
        "/usr/bin/cmake",
        "-G",
        "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_TESTING=ON",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        f"--graphviz={build_root}/manyforge-i2-target-graph.dot",
    ]
    build_targets: list[str] = []
    if lane_id == "core_release":
        configure = [
            *common,
            "-S",
            "/workspace/manyforge_core",
            "-B",
            str(build_root),
        ]
    elif lane_id in {"deployment_release", "c02_tail_focused_release"}:
        timeout = "120" if lane_id == "deployment_release" else "180"
        configure = [
            *common,
            "-S",
            "/workspace/manyforge_deployment",
            "-B",
            str(build_root),
            "-DMANYFORGE_CORE_INCLUDE_DIR=/workspace/manyforge_core/include",
            "-DMANYFORGE_VERIFY_CANONICAL_EMPTY_RELEASE_GOLDEN=ON",
            f"-DMANYFORGE_GTEST_TIMEOUT_SECONDS={timeout}",
        ]
        if lane_id == "c02_tail_focused_release":
            build_targets = [
                "test_mf2_joint_name",
                "test_cell_structure",
                "test_cell_applicability",
                "test_cell_applicability_budget",
                "test_deployment_applicability_codec",
                "test_site_overlay_resolution",
                "test_cell_compiler",
                "test_build_manifest",
                "test_retention_qualified_deployment",
            ]
    elif lane_id in {"roboplan_release", "roboplan_consumer_focused"}:
        cmeel_prefix = Path(
            "/usr/local/lib/python3.12/site-packages/cmeel.prefix"
        )
        if not (
            cmeel_prefix / "lib/cmake/pinocchio/pinocchioConfig.cmake"
        ).is_file():
            fail("pinned Pinocchio CMake package is absent")
        environment.update(
            {
                "CMAKE_PREFIX_PATH": f"/usr/local:{cmeel_prefix}",
                "LD_LIBRARY_PATH": "/usr/local/lib",
                "BOOST_ROOT": str(cmeel_prefix),
                "BOOST_INCLUDEDIR": str(cmeel_prefix / "include"),
                "BOOST_LIBRARYDIR": str(cmeel_prefix / "lib"),
            }
        )
        configure = [
            *common,
            "-S",
            "/workspace/manyforge_roboplan",
            "-B",
            str(build_root),
            "-DMANYFORGE_GTEST_TIMEOUT_SECONDS=60",
        ]
    else:
        fail(f"unknown Release native lane: {lane_id}")

    build_argv = [
        "/usr/bin/ninja",
        "-C",
        str(build_root),
        "-j",
        "2",
        "-d",
        "keeprsp",
        *build_targets,
    ]
    for stage, argv in (("configure", configure), ("build", build_argv)):
        completed = subprocess.run(
            argv,
            cwd=repository_root,
            env=environment,
            check=False,
        )
        if completed.returncode != 0:
            fail(f"{lane_id} native {stage} failed: {completed.returncode}")
    return build_root


def execute_production_lane(
    manifest: dict[str, Any],
    lane_id: str,
    repository_root: Path,
    requested_output_root: Path,
) -> int:
    """Build and authenticate one exact BUILD_TESTING=OFF Release lane."""

    if lane_id not in PRODUCTION_EXECUTION_LANES:
        fail(f"unknown frozen production lane: {lane_id}")
    if repository_root.resolve() != Path("/workspace"):
        fail("production packet execution must run from the readonly /workspace snapshot")
    try:
        output_root = requested_output_root.resolve(strict=True)
        metadata = output_root.lstat()
    except OSError as error:
        fail(f"production qualification output root is absent: {error}")
    if (
        output_root != Path("/qualification-output")
        or stat.S_ISLNK(metadata.st_mode)
        or not stat.S_ISDIR(metadata.st_mode)
    ):
        fail("production qualification output root is not the exact safe artifact mount")

    rows = {
        row["lane_id"]: row
        for row in manifest["test_inventory_contract"]["production_build_lanes"]
    }
    row = rows[lane_id]
    build_relative = _safe_relative_path(
        row["build_root"], f"{lane_id} production build root"
    )
    inventory_relative = _safe_relative_path(
        row["inventory_output"], f"{lane_id} production inventory"
    )
    provenance_relative = _safe_relative_path(
        row["provenance_output"], f"{lane_id} production provenance"
    )
    index_relative = _safe_relative_path(
        row["execution_index_output"], f"{lane_id} production execution index"
    )
    output_parent = _ensure_container_output_directory(
        output_root,
        inventory_relative.parent,
        f"{lane_id} production evidence directory",
        must_be_new=True,
    )
    if provenance_relative.parent != inventory_relative.parent or index_relative.parent != inventory_relative.parent:
        fail(f"{lane_id} production evidence outputs do not share one frozen directory")
    build_root = _ensure_container_output_directory(
        output_root,
        build_relative,
        f"{lane_id} production build root",
        must_be_new=True,
    )
    query_root = _ensure_container_output_directory(
        output_root,
        build_relative / ".cmake/api/v1/query",
        f"{lane_id} CMake File API query directory",
        must_be_new=True,
    )
    with _exclusive_output_file(query_root, "codemodel-v2", binary=False) as query:
        query.write("{}\n")
    with _exclusive_output_file(query_root, "cmakeFiles-v1", binary=False) as query:
        query.write("{}\n")
    _write_graphviz_options(build_root)
    temporary_root = _ensure_container_output_directory(
        output_root,
        Path("tmp") / lane_id,
        f"{lane_id} production temporary root",
        must_be_new=True,
    )
    environment = _production_build_environment(lane_id)
    if environment["TMPDIR"] != str(temporary_root):
        fail(f"{lane_id} production temporary-root contract drifted")
    if build_root != output_root / build_relative:
        fail(f"{lane_id} production build path resolution drifted")
    for stage, argv in (
        ("configure", row["configure_argv"]),
        ("build", row["build_argv"]),
    ):
        completed = subprocess.run(
            argv,
            cwd=repository_root,
            env=environment,
            check=False,
        )
        if completed.returncode != 0:
            return completed.returncode

    inventory_run = subprocess.run(
        row["inventory_argv"],
        cwd=repository_root,
        env=environment,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if inventory_run.stderr:
        sys.stderr.write(inventory_run.stderr)
        sys.stderr.flush()
    if inventory_run.returncode != 0:
        return inventory_run.returncode
    try:
        inventory = json.loads(
            inventory_run.stdout,
            object_pairs_hook=_object_without_duplicates,
        )
    except json.JSONDecodeError as error:
        fail(f"{lane_id} production CTest inventory is not JSON: {error}")
    inventory = _require_object(inventory, f"{lane_id} production CTest inventory")
    if _require_list(inventory.get("tests"), f"{lane_id} production CTest tests"):
        fail(f"{lane_id} BUILD_TESTING=OFF build registered tests")
    inventory_path = _write_exclusive_json(
        output_parent, inventory_relative.name, inventory
    )

    build_contract = _load_build_provenance_contract(
        manifest["test_inventory_contract"], manifest, repository_root
    )
    try:
        provenance = validate_production_build_provenance(
            repository_root=repository_root,
            output_root=output_root,
            lane=row,
            empty_ctest_inventory=inventory,
            target_contract=build_contract,
        )
    except NativeBuildProvenanceError as error:
        fail(f"{lane_id} production build provenance failed: {error}")
    provenance_path = _write_exclusive_json(
        output_parent, provenance_relative.name, provenance
    )
    index = {
        "schema_version": "manyforge.composable_cell_i2.production_build_execution.v1",
        "lane_id": lane_id,
        "project_id": row["project_id"],
        "environment": environment,
        "configure_argv": row["configure_argv"],
        "configure_exit_status": 0,
        "build_argv": row["build_argv"],
        "build_exit_status": 0,
        "inventory_argv": row["inventory_argv"],
        "inventory_exit_status": 0,
        "inventory_path": inventory_relative.as_posix(),
        "inventory_sha256": _sha256_file(inventory_path),
        "provenance_path": provenance_relative.as_posix(),
        "provenance_sha256": _sha256_file(provenance_path),
        "provenance_evidence_sha256": provenance["evidence_sha256"],
        "passed": True,
    }
    _write_exclusive_json(output_parent, index_relative.name, index)
    return 0


def validate_native_registry_before_execution(
    manifest: dict[str, Any],
    lane_id: str,
    repository_root: Path,
    inventory_path: Path,
    *,
    revalidate_existing_provenance: bool = False,
) -> dict[str, Any]:
    if repository_root.resolve() != Path("/workspace"):
        fail("native pre-execution validation requires readonly /workspace")
    rows = {
        row["lane_id"]: row
        for row in manifest["test_inventory_contract"]["native_execution_lanes"]
    }
    if lane_id not in rows:
        fail(f"unknown native pre-execution lane: {lane_id}")
    row = rows[lane_id]
    expected_inventory = Path("/qualification-output") / row["inventory_output"]
    if inventory_path.resolve(strict=True) != expected_inventory:
        fail("native pre-execution inventory path drifted")
    contract = manifest["test_inventory_contract"]
    groups, _ = _load_baseline_test_inventory(
        contract, manifest, repository_root
    )
    target_groups, ordinary_sources = _load_native_target_contract(
        contract, manifest, repository_root
    )
    build_contract = _load_build_provenance_contract(
        contract, manifest, repository_root
    )
    registered = _ctest_inventory_names(
        expected_inventory, f"{lane_id} pre-execution CTest inventory"
    )
    required = groups[row["required_group"]]
    if not required.issubset(registered):
        fail(f"{lane_id} pre-execution registry deleted frozen tests")
    if not row["allow_additional_tests"] and registered != required:
        fail(f"{lane_id} pre-execution registry added tests")
    target_mapping = _validate_native_target_mapping(
        expected_inventory,
        lane_id=lane_id,
        build_root=row["build_root"],
        required_group=row["required_group"],
        required_names=required,
        allow_additional_tests=row["allow_additional_tests"],
        target_groups=target_groups,
        ordinary_sources=ordinary_sources,
    )
    _, derived_registered, _, _ = _derive_native_process_contract(
        expected_inventory, row
    )
    if derived_registered != registered:
        fail(f"{lane_id} pre-execution registry derivations disagree")
    inventory = _read_json_object(
        expected_inventory, f"{lane_id} native provenance inventory"
    )
    try:
        provenance = validate_native_build_provenance(
            repository_root=repository_root,
            output_root=Path("/qualification-output"),
            lane=row,
            inventory=inventory,
            target_group_mapping=target_groups[row["required_group"]],
            ordinary_mapping=ordinary_sources,
            production_target_contract=build_contract,
            retain_configure_fixture_replay=revalidate_existing_provenance,
        )
    except NativeBuildProvenanceError as error:
        fail(f"{lane_id} native build provenance failed: {error}")
    provenance_relative = _safe_relative_path(
        row["provenance_output"], f"{lane_id} native provenance output"
    )
    provenance_root = _ensure_container_output_directory(
        Path("/qualification-output"),
        provenance_relative.parent,
        f"{lane_id} native provenance directory",
    )
    if revalidate_existing_provenance:
        provenance_path = _require_contained_regular_file(
            provenance_root / provenance_relative.name,
            Path("/qualification-output"),
            f"{lane_id} existing native provenance",
            require_single_link=True,
        )
        existing = _read_json_object(
            provenance_path, f"{lane_id} existing native provenance"
        )
        if existing != provenance:
            fail(f"{lane_id} native build provenance changed after execution")
    else:
        provenance_path = _write_exclusive_json(
            provenance_root, provenance_relative.name, provenance
        )
    return {
        "schema_version": "manyforge.composable_cell_i2.native_preexecution_receipt.v1",
        "lane_id": lane_id,
        "inventory_sha256": _sha256_file(expected_inventory),
        "provenance_path": provenance_relative.as_posix(),
        "provenance_sha256": _sha256_file(provenance_path),
        "provenance_evidence_sha256": provenance["evidence_sha256"],
        "native_target_mapping_sha256": _canonical_json_sha256(target_mapping),
    }


def execute_native_lane(
    manifest: dict[str, Any],
    lane_id: str,
    repository_root: Path,
    requested_output_root: Path,
) -> int:
    """Build and directly execute one native lane from the packet."""

    if lane_id not in NATIVE_EXECUTION_LANES:
        fail(f"unknown frozen native lane: {lane_id}")
    if repository_root.resolve() != Path("/workspace"):
        fail("native packet execution must run from the readonly /workspace snapshot")
    try:
        output_root = requested_output_root.resolve(strict=True)
        metadata = output_root.lstat()
    except OSError as error:
        fail(f"native qualification output root is absent: {error}")
    if (
        output_root != Path("/qualification-output")
        or stat.S_ISLNK(metadata.st_mode)
        or not stat.S_ISDIR(metadata.st_mode)
    ):
        fail("native qualification output root is not the exact safe artifact mount")

    rows = {
        row["lane_id"]: row
        for row in manifest["test_inventory_contract"]["native_execution_lanes"]
    }
    row = rows[lane_id]

    inventory = output_root / row["inventory_output"]
    execution_index = output_root / row["execution_index_output"]
    result_root = execution_index.parent / "native"
    build_root = output_root / row["build_root"]
    temporary_root = output_root / "tmp" / lane_id
    temporary_root.mkdir(parents=True, exist_ok=False)

    environment = {
        "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "TZ": "UTC",
        "PYTHONNOUSERSITE": "1",
        "CMAKE_BUILD_PARALLEL_LEVEL": "2",
        "TMPDIR": str(temporary_root),
        "HOME": str(temporary_root),
        "XDG_CACHE_HOME": str(temporary_root / "xdg-cache"),
        "MANYFORGE_NATIVE_ARTIFACT_ROOT": str(output_root),
        "MANYFORGE_NATIVE_INVENTORY_JSON": str(inventory),
        "MANYFORGE_NATIVE_EXECUTION_INDEX_JSON": str(execution_index),
        "MANYFORGE_NATIVE_RESULT_ROOT": str(result_root),
        "MANYFORGE_NATIVE_LANE_ID": lane_id,
        **NATIVE_SANITIZER_ENVIRONMENT[row["sanitizer"]],
    }
    if row["sanitizer"] == "thread":
        environment["TSAN_SUPPRESSIONS"] = "/workspace/scripts/ci/tsan.supp"
    if lane_id.startswith("roboplan_"):
        environment["ROBOPLAN_SOURCE"] = "/workspace/.docker/roboplan"

    if row["sanitizer"] == "none":
        build_root = _prepare_native_release_lane(
            lane_id, row, repository_root, output_root, environment
        )
        runner = _require_contained_regular_file(
            repository_root / "scripts/ci/run-composable-cell-i2-native-evidence.py",
            repository_root,
            "native direct runner",
            require_single_link=True,
        )
        argv = [
            "/usr/local/bin/python3",
            str(runner),
            "--lane-id",
            lane_id,
            "--sanitizer",
            "none",
            "--artifact-root",
            str(output_root),
            "--build-dir",
            str(build_root),
            "--inventory-json",
            str(inventory),
            "--execution-index-json",
            str(execution_index),
            "--result-root",
            str(result_root),
            "--timeout-seconds",
            str(row["timeout_ceiling"]),
            "--require-zero-disabled",
            "--require-zero-skips",
            "--forbid-fallback",
        ]
        if row["test_regex"] is not None:
            argv.extend(["--test-regex", row["test_regex"]])
        runner_environment = dict(environment)
        for name in (
            "CMAKE_PREFIX_PATH",
            "LD_LIBRARY_PATH",
            "BOOST_ROOT",
            "BOOST_INCLUDEDIR",
            "BOOST_LIBRARYDIR",
        ):
            runner_environment.pop(name, None)
        completed = subprocess.run(
            argv, cwd=repository_root, env=runner_environment, check=False
        )
        return completed.returncode

    script_relative, script_arguments, build_variable, build_relative = (
        NATIVE_LANE_SCRIPTS[lane_id]
    )
    expected_build = (
        build_relative
        if not lane_id.startswith("roboplan_")
        else f"{build_relative}/build/manyforge_roboplan"
    )
    if row["build_root"] != expected_build:
        fail(f"native build-root mapping drifted for {lane_id}")
    script = _require_contained_regular_file(
        repository_root / script_relative,
        repository_root,
        f"frozen native lane script {lane_id}",
        require_single_link=True,
    )
    if not os.access(script, os.X_OK):
        fail(f"frozen native lane script is not executable: {script_relative}")
    environment[build_variable] = str(output_root / build_relative)
    completed = subprocess.run(
        [str(script), *script_arguments],
        cwd=repository_root,
        env=environment,
        check=False,
    )
    return completed.returncode


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument("--check", action="store_true")
    actions.add_argument("--list", action="store_true")
    actions.add_argument("--render", metavar="LANE")
    actions.add_argument("--run", metavar="LANE_OR_ALL")
    actions.add_argument("--scan", action="store_true")
    actions.add_argument("--verify-result", type=Path)
    actions.add_argument("--execute-native-lane", metavar="LANE")
    actions.add_argument("--execute-production-lane", metavar="LANE")
    actions.add_argument("--validate-native-registry", metavar="LANE")
    actions.add_argument("--revalidate-native-provenance", metavar="LANE")
    actions.add_argument("--execute-binding-lane", metavar="LANE")
    actions.add_argument("--execute-determinism-build", metavar="LANE")
    actions.add_argument("--execute-determinism-compare", action="store_true")
    parser.add_argument("--check-environment", action="store_true")
    parser.add_argument("--run-id")
    parser.add_argument("--review", type=Path)
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--registry-inventory", type=Path)
    args = parser.parse_args()

    repository_root = repository_root_for(args.manifest)
    manifest = load_manifest(args.manifest.resolve())
    lanes = validate_manifest(manifest, repository_root)
    environment: dict[str, Any] | None = None
    if args.check_environment:
        environment = check_environment(manifest, repository_root)

    if args.check:
        print(
            json.dumps(
                {
                    "schema_version": manifest["schema_version"],
                    "status": manifest["status"],
                    "lane_count": len(lanes),
                    "fixture_requirement_count": len(manifest["fixture_matrix"]),
                    "forbidden_scan_count": len(manifest["forbidden_scans"]),
                    "environment": environment,
                },
                indent=2,
                sort_keys=True,
            )
        )
        return 0
    if args.list:
        for lane in lanes.values():
            print(f"{lane['ordinal']:02d} {lane['id']}: {lane['claim']}")
        return 0
    if args.render:
        if args.render not in lanes:
            fail(f"unknown qualification lane: {args.render}")
        artifact_root = (
            _safe_output_root(repository_root) / "RENDERED-RUN" / "artifacts"
        )
        lane_root = _lane_artifact_root(artifact_root, args.render)
        print(
            shlex.join(
                render_lane(
                    manifest,
                    lanes[args.render],
                    repository_root,
                    lane_root,
                    artifact_root,
                )
            )
        )
        return 0
    if args.scan:
        run_scans(manifest, repository_root, args.output_root)
        return 0
    if args.verify_result is not None:
        if args.review is None:
            fail("--verify-result requires --review")
        verify_and_close_result(
            manifest,
            lanes,
            args.verify_result,
            args.review,
            repository_root,
        )
        return 0
    if args.execute_native_lane is not None:
        if args.output_root is None:
            fail("--execute-native-lane requires --output-root")
        return execute_native_lane(
            manifest,
            args.execute_native_lane,
            repository_root,
            args.output_root,
        )
    if args.execute_production_lane is not None:
        if args.output_root is None:
            fail("--execute-production-lane requires --output-root")
        return execute_production_lane(
            manifest,
            args.execute_production_lane,
            repository_root,
            args.output_root,
        )
    if args.validate_native_registry is not None:
        if args.registry_inventory is None:
            fail("--validate-native-registry requires --registry-inventory")
        receipt = validate_native_registry_before_execution(
            manifest,
            args.validate_native_registry,
            repository_root,
            args.registry_inventory,
        )
        print(json.dumps(receipt, sort_keys=True))
        return 0
    if args.revalidate_native_provenance is not None:
        if args.registry_inventory is None:
            fail("--revalidate-native-provenance requires --registry-inventory")
        receipt = validate_native_registry_before_execution(
            manifest,
            args.revalidate_native_provenance,
            repository_root,
            args.registry_inventory,
            revalidate_existing_provenance=True,
        )
        print(json.dumps(receipt, sort_keys=True))
        return 0
    if args.execute_binding_lane is not None:
        if args.output_root is None:
            fail("--execute-binding-lane requires --output-root")
        return execute_binding_lane(
            manifest,
            args.execute_binding_lane,
            repository_root,
            args.output_root,
        )
    if args.execute_determinism_build is not None:
        if args.output_root is None:
            fail("--execute-determinism-build requires --output-root")
        return execute_determinism_build(
            manifest,
            args.execute_determinism_build,
            repository_root,
            args.output_root,
        )
    if args.execute_determinism_compare:
        if args.output_root is None:
            fail("--execute-determinism-compare requires --output-root")
        return execute_determinism_compare(
            manifest,
            repository_root,
            args.output_root,
        )
    if args.output_root is not None:
        fail("--output-root is valid only with a packet-owned lane executor")
    if args.registry_inventory is not None:
        fail("--registry-inventory is valid only with pre-execution validation")
    if args.review is not None:
        fail("--review is valid only with --verify-result")
    assert args.run is not None
    return execute_lanes(
        manifest,
        lanes,
        args.run,
        repository_root,
        args.run_id,
    )


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ManifestError as error:
        print(f"I2 qualification manifest error: {error}", file=sys.stderr)
        raise SystemExit(2) from error
