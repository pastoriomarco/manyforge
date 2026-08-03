from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

import pytest

from verification.composable_cell_i2 import binding_contract_tests as binding_cases
from verification.composable_cell_i2 import verify_qualification_manifest as verifier


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = (
    REPOSITORY_ROOT
    / "verification/composable_cell_i2/qualification_manifest.v1.json"
)
BUILD_CONTRACT_PATH = (
    REPOSITORY_ROOT
    / "verification/composable_cell_i2/build_provenance_contract.v1.json"
)
GROUPED_RUNNER_PATH = (
    REPOSITORY_ROOT / "scripts/ci/run-composable-cell-i2-native-evidence.py"
)
REVIEW_TEMPLATE_PATH = (
    REPOSITORY_ROOT
    / "verification/composable_cell_i2/adversarial_review_template.v1.json"
)


def load_grouped_runner():
    name = "manyforge_i2_grouped_runner_test_module"
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, GROUPED_RUNNER_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def loaded_manifest() -> dict[str, object]:
    return verifier.load_manifest(MANIFEST_PATH)


def loaded_build_contract() -> dict[str, object]:
    return verifier.load_manifest(BUILD_CONTRACT_PATH)


def test_manifest_is_strict_and_declares_the_active_qualification_contract() -> None:
    manifest = loaded_manifest()

    lanes = verifier.validate_manifest(manifest, REPOSITORY_ROOT)

    assert manifest["schema_version"] == verifier.SCHEMA
    assert manifest["status"] == verifier.QUALIFICATION_STATUS
    assert set(lanes) == verifier.REQUIRED_LANES
    assert [lane["ordinal"] for lane in lanes.values()] == list(
        range(1, len(lanes) + 1)
    )
    assert all(lane["skip_policy"] == "forbid" for lane in lanes.values())
    assert set(binding_cases._PREDECESSORS) == (
        verifier.REQUIRED_NONCONSTRUCTIBLE_EXPORTS - {"CompilerQualifiedCellIr"}
    )


def test_adversarial_review_template_covers_the_exact_closure_scope() -> None:
    manifest = loaded_manifest()
    template = verifier.load_manifest(REVIEW_TEMPLATE_PATH)
    scope = template["scope"]

    assert scope["checks"] == verifier.REQUIRED_REVIEW_CHECKS
    assert scope["reviewed_lane_ids"] == [
        lane["id"] for lane in manifest["qualification_lanes"]
    ]
    assert scope["reviewed_evidence_fields"] == sorted(
        verifier.REQUIRED_EVIDENCE_FIELDS - {"independent_adversarial_review"}
    )
    assert scope["reviewed_log_count"] == len(manifest["qualification_lanes"])


def test_duplicate_json_key_is_rejected(tmp_path: Path) -> None:
    duplicate = tmp_path / "duplicate.json"
    duplicate.write_text(
        '{"schema_version":"first","schema_version":"second"}\n',
        encoding="utf-8",
    )

    with pytest.raises(verifier.ManifestError, match="duplicate JSON key"):
        verifier.load_manifest(duplicate)


def test_unknown_top_level_key_is_rejected() -> None:
    manifest = loaded_manifest()
    manifest["unreviewed_extension"] = True

    with pytest.raises(verifier.ManifestError, match="manifest keys differ"):
        verifier.validate_manifest(manifest, REPOSITORY_ROOT)


def test_missing_required_lane_is_rejected() -> None:
    manifest = loaded_manifest()
    manifest["qualification_lanes"] = [
        lane
        for lane in manifest["qualification_lanes"]
        if lane["id"] != "bindings_gil_concurrency"
    ]
    for ordinal, lane in enumerate(manifest["qualification_lanes"], start=1):
        lane["ordinal"] = ordinal

    with pytest.raises(verifier.ManifestError, match="lane set differs"):
        verifier.validate_manifest(manifest, REPOSITORY_ROOT)


def test_evidence_output_must_remain_in_ignored_bounded_root() -> None:
    manifest = loaded_manifest()
    manifest["evidence_contract"]["output_root"] = "build/i2"

    with pytest.raises(verifier.ManifestError, match="output root"):
        verifier.validate_manifest(manifest, REPOSITORY_ROOT)


def test_renderer_uses_exact_image_read_only_source_and_bounded_output(
    tmp_path: Path,
) -> None:
    manifest = loaded_manifest()
    lanes = verifier.validate_manifest(manifest, REPOSITORY_ROOT)
    output = tmp_path / "qualification"

    argv = verifier.render_lane(
        manifest,
        lanes["core_release"],
        REPOSITORY_ROOT,
        output,
        output,
    )

    assert argv[:2] == ["docker", "run"]
    assert manifest["qualification_environment"]["image"]["id"] in argv
    assert "--network" in argv and argv[argv.index("--network") + 1] == "none"
    assert (
        f"type=bind,src={REPOSITORY_ROOT},dst=/workspace,readonly" in argv
    )
    assert (
        f"type=bind,src={output.resolve()},dst=/qualification-output" in argv
    )
    container_command = lanes["core_release"]["container_command"]
    assert argv[-len(container_command) :] == container_command
    assert argv[-len(container_command) - 1] == (
        manifest["qualification_environment"]["image"]["id"]
    )


def test_binding_and_generated_output_inventory_is_exact() -> None:
    manifest = loaded_manifest()
    verifier.validate_manifest(manifest, REPOSITORY_ROOT)
    binding = manifest["binding_contract"]

    assert binding["production_modules"] == ["manyforge_core_py"]
    assert "manyforge_roboplan_py" in binding["forbidden_production_modules"]
    assert len(binding["registration_units"]) == 8
    assert binding["closure_integration"]["must_update"] == [
        "scripts/test-all.sh",
        ".github/workflows/python-tests.yml",
        "README.md",
    ]
    assert len(binding["closure_integration"]["must_add"]) == 6
    assert {row["role"] for row in binding["generated_outputs"]} == {
        "extension",
        "typed_stub",
        "api_inventory",
        "api_documentation",
    }


def test_required_sanitizer_lanes_disable_aslr_for_the_complete_executor() -> None:
    manifest = loaded_manifest()
    lanes = verifier.validate_manifest(manifest, REPOSITORY_ROOT)

    assert verifier.NO_ASLR_EXECUTION_LANES == {
        "core_asan_ubsan",
        "core_tsan",
        "deployment_asan_ubsan",
        "roboplan_asan_ubsan",
        "roboplan_tsan",
        "bindings_asan_ubsan",
        "bindings_tsan",
    }

    for lane_id, lane in lanes.items():
        command = lane["container_command"]
        if lane_id in verifier.NO_ASLR_EXECUTION_LANES:
            assert command[:3] == verifier.NO_ASLR_EXECUTOR_PREFIX
            assert command[3] == "python3"
            rendered = verifier.render_lane(
                manifest,
                lane,
                REPOSITORY_ROOT,
                REPOSITORY_ROOT / ".manyforge-var/test-tsan-render",
                REPOSITORY_ROOT / ".manyforge-var/test-tsan-render",
            )
            image_index = rendered.index(
                manifest["qualification_environment"]["image"]["id"]
            )
            assert rendered[image_index + 1 : image_index + 4] == (
                verifier.NO_ASLR_EXECUTOR_PREFIX
            )
        else:
            assert command[:3] != verifier.NO_ASLR_EXECUTOR_PREFIX

    missing_prefix = loaded_manifest()
    next(
        lane
        for lane in missing_prefix["qualification_lanes"]
        if lane["id"] == "roboplan_tsan"
    )["container_command"] = next(
        lane
        for lane in missing_prefix["qualification_lanes"]
        if lane["id"] == "roboplan_tsan"
    )["container_command"][3:]
    with pytest.raises(verifier.ManifestError, match="exact no-ASLR executor"):
        verifier.validate_manifest(missing_prefix, REPOSITORY_ROOT)

    wrong_prefix = loaded_manifest()
    next(
        lane
        for lane in wrong_prefix["qualification_lanes"]
        if lane["id"] == "core_tsan"
    )["container_command"][2] = "--addr-no-randomize"
    with pytest.raises(verifier.ManifestError, match="exact no-ASLR executor"):
        verifier.validate_manifest(wrong_prefix, REPOSITORY_ROOT)

    release_with_prefix = loaded_manifest()
    release_lane = next(
        lane
        for lane in release_with_prefix["qualification_lanes"]
        if lane["id"] == "core_release"
    )
    release_lane["container_command"] = [
        *verifier.NO_ASLR_EXECUTOR_PREFIX,
        *release_lane["container_command"],
    ]
    with pytest.raises(verifier.ManifestError, match="undeclared executable"):
        verifier.validate_manifest(release_with_prefix, REPOSITORY_ROOT)


def test_baseline_test_names_are_frozen_by_exact_count_and_digest() -> None:
    manifest = loaded_manifest()
    verifier.validate_manifest(manifest, REPOSITORY_ROOT)
    inventory_path = REPOSITORY_ROOT / manifest["test_inventory_contract"][
        "baseline_inventory_path"
    ]
    inventory_bytes = inventory_path.read_bytes()
    inventory = json.loads(inventory_bytes)

    assert hashlib.sha256(inventory_bytes).hexdigest() == (
        verifier.BASELINE_TEST_INVENTORY_SHA256
    )
    assert set(inventory["groups"]) == set(verifier.REQUIRED_TEST_GROUPS)
    for group_id, (count, digest) in verifier.REQUIRED_TEST_GROUPS.items():
        group = inventory["groups"][group_id]
        assert group["count"] == count
        assert len(group["names"]) == count
        assert group["names_sha256"] == digest
    assert set(inventory["witness_groups"]) == set(
        verifier.REQUIRED_WITNESS_GROUPS
    )
    for group_id, (count, digest) in verifier.REQUIRED_WITNESS_GROUPS.items():
        group = inventory["witness_groups"][group_id]
        assert group["count"] == count
        assert len(group["names"]) == count
        assert group["names_sha256"] == digest


def test_candidate_capture_tool_cannot_be_redirected() -> None:
    manifest = loaded_manifest()
    manifest["candidate_identity"]["capture_tool"] = "tests/conftest.py"

    with pytest.raises(verifier.ManifestError, match="capture tool must be exactly"):
        verifier.validate_manifest(manifest, REPOSITORY_ROOT)


def test_test_inventory_lane_cannot_drop_its_frozen_baseline() -> None:
    manifest = loaded_manifest()
    manifest["test_inventory_contract"]["native_execution_lanes"][0][
        "required_group"
    ] = "roboplan_consumer_required"

    with pytest.raises(verifier.ManifestError, match="contract drifted"):
        verifier.validate_manifest(manifest, REPOSITORY_ROOT)


def test_fixture_and_scan_references_are_closed() -> None:
    manifest = loaded_manifest()
    lanes = verifier.validate_manifest(manifest, REPOSITORY_ROOT)
    rotating = manifest["rotating_identities"]

    assert all(
        set(requirement["evidence_lanes"]).issubset(lanes)
        for requirement in manifest["fixture_matrix"]
    )
    assert all(
        set(requirement) == {
            "id",
            "evidence_lanes",
            "native_test_witnesses",
            "binding_case_witnesses",
            "python_binding_floor_lanes",
            "scan_ids",
            "determinism_artifacts",
            "generated_output_roles",
        }
        for requirement in manifest["fixture_matrix"]
    )
    assert {scan["id"] for scan in manifest["forbidden_scans"]} == (
        verifier.REQUIRED_SCAN_IDS
    )
    assert all(scan["expected_matches"] == 0 for scan in manifest["forbidden_scans"])
    assert {scan["mode"] for scan in manifest["forbidden_scans"]} == {
        "line_rg",
        "file_regex",
    }
    assert "fixture_ir_outputs" not in rotating
    assert not any(
        pattern.startswith(("fixture-raw/", "fixture-ir/"))
        for pattern in rotating["regression_products"]
    )
    semantic_fixtures = [
        fixture
        for fixture in manifest["fixture_matrix"]
        if fixture["id"].startswith(("c02m_", "c02n_", "c02o_"))
    ]
    assert semantic_fixtures
    assert all(
        fixture["native_test_witnesses"] or fixture["binding_case_witnesses"]
        for fixture in semantic_fixtures
    )
    assert all(
        fixture["determinism_artifacts"] == []
        and "deterministic_rebuild" not in fixture["evidence_lanes"]
        for fixture in semantic_fixtures
        if fixture["id"].startswith(("c02m_", "c02n_"))
    )
    final_fixture = next(
        fixture
        for fixture in semantic_fixtures
        if fixture["id"].startswith("c02o_")
    )
    assert final_fixture["determinism_artifacts"] == [
        "canonical_empty_product_package/canonical-empty-admission.json"
    ]


def test_binding_asan_uses_linked_host_and_keeps_leak_boundary_explicit() -> None:
    assert (
        verifier.NATIVE_SANITIZER_ENVIRONMENT["address+undefined"][
            "ASAN_OPTIONS"
        ]
        == "halt_on_error=1:detect_leaks=1:exitcode=66"
    )
    assert verifier.BINDING_ASAN_UBSAN_ENVIRONMENT == {
        "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=0:exitcode=66",
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1:exitcode=66",
    }
    assert verifier.BINDING_ASAN_HOST == Path(
        "/qualification-output/build/bindings-asan/sanitizer-host/"
        "manyforge-python-asan-ubsan"
    )
    assert verifier.BINDING_ASAN_HOST_REQUIRED_NEEDED == (
        "libasan.so.8",
        "libubsan.so.1",
        "libstdc++.so.6",
        "libpython3.12.so.1.0",
    )
    manifest = loaded_manifest()
    assert verifier.BINDING_ASAN_HOST_OUTPUT in next(
        lane["expected_outputs"]
        for lane in manifest["qualification_lanes"]
        if lane["id"] == "bindings_asan_ubsan"
    )
    row = next(
        item
        for item in manifest["test_inventory_contract"]["binding_result_lanes"]
        if item["lane_id"] == "bindings_asan_ubsan"
    )
    assert verifier._binding_collection_argv(row)[0] == str(
        verifier.BINDING_ASAN_HOST
    )
    assert verifier._binding_run_argv(row)[0] == str(verifier.BINDING_ASAN_HOST)
    assert "scripts/ci/core-bindings-sanitizer-host.cpp" in manifest[
        "binding_contract"
    ]["closure_integration"]["must_add"]


def test_binding_sanitizer_host_inspection_and_artifact_mapping(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    artifact_root = tmp_path / "artifacts"
    lane_root = artifact_root / "lanes/bindings_asan_ubsan"
    host = verifier._binding_sanitizer_host_artifact_path(
        artifact_root, "bindings_asan_ubsan"
    )
    assert host == lane_root / verifier.BINDING_ASAN_HOST_OUTPUT
    assert (
        verifier._binding_sanitizer_host_artifact_path(
            artifact_root, "bindings_release"
        )
        is None
    )
    assert host is not None
    host.parent.mkdir(parents=True)
    host.write_bytes(b"\x7fELFlinked-host-fixture")
    host.chmod(0o755)

    valid_output = "\n".join(
        f"Shared library: [{library}]"
        for library in (
            "libasan.so.8",
            "libstdc++.so.6",
            "libpython3.12.so.1.0",
            "libm.so.6",
            "libubsan.so.1",
            "libgcc_s.so.1",
            "libc.so.6",
        )
    )
    dynamic_output = {"value": valid_output}

    def inspect(*args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
        del kwargs
        return subprocess.CompletedProcess(
            args=args,
            returncode=0,
            stdout=dynamic_output["value"],
            stderr="",
        )

    monkeypatch.setattr(verifier.subprocess, "run", inspect)
    evidence = verifier._inspect_binding_sanitizer_host(
        host,
        lane_root,
        rendered_path=str(verifier.BINDING_ASAN_HOST),
    )
    assert evidence["path"] == str(verifier.BINDING_ASAN_HOST)
    assert evidence["needed"][0] == "libasan.so.8"

    dynamic_output["value"] = valid_output.replace(
        "Shared library: [libasan.so.8]\nShared library: [libstdc++.so.6]",
        "Shared library: [libstdc++.so.6]\nShared library: [libasan.so.8]",
    )
    with pytest.raises(verifier.ManifestError, match="dynamic runtime order"):
        verifier._inspect_binding_sanitizer_host(
            host,
            lane_root,
            rendered_path=str(verifier.BINDING_ASAN_HOST),
        )

    dynamic_output["value"] = valid_output.replace(
        "Shared library: [libubsan.so.1]", ""
    )
    with pytest.raises(verifier.ManifestError, match="dynamic runtime order"):
        verifier._inspect_binding_sanitizer_host(
            host,
            lane_root,
            rendered_path=str(verifier.BINDING_ASAN_HOST),
        )


def test_multiline_trusted_pybind_constructor_is_detected(tmp_path: Path) -> None:
    source_root = tmp_path / "manyforge_core/python"
    source_root.mkdir(parents=True)
    (source_root / "bindings.cpp").write_text(
        """
py::class_<
    CompilerQualifiedCellIr
>(module, "CompilerQualifiedCellIr")
    .def(
        py::init<>()
    );
""",
        encoding="utf-8",
    )
    manifest = loaded_manifest()
    scan = next(
        row
        for row in manifest["forbidden_scans"]
        if row["id"] == "trusted_stage_python_constructor"
    )

    result = verifier.evaluate_scan(scan, tmp_path)

    assert result["matches"] == 1
    assert result["passed"] is False
    assert result["details"][0]["path"] == "manyforge_core/python/bindings.cpp"


@pytest.mark.parametrize(
    ("scan_id", "relative_path", "content"),
    [
        (
            "direct_graph_to_retention_bypass",
            "manyforge_deployment/src/bypass.cpp",
            """
RetainedProduct build_and_retain(
    const GraphRevisionVerifiedCellGraph& graph
);
""",
        ),
        (
            "compatibility_fallback_or_alias",
            "manyforge_deployment/include/bypass.hpp",
            """
struct Legacy
    fallback {};
""",
        ),
        (
            "test_only_final_trust_bypass",
            "manyforge_deployment/test/test_bypass.cpp",
            """
friend class CompilerQualifiedCellIrTestAccess;
""",
        ),
    ],
)
def test_other_multiline_bypass_scans_cannot_be_evaded(
    tmp_path: Path,
    scan_id: str,
    relative_path: str,
    content: str,
) -> None:
    path = tmp_path / relative_path
    path.parent.mkdir(parents=True)
    path.write_text(content, encoding="utf-8")
    manifest = loaded_manifest()
    scan = next(row for row in manifest["forbidden_scans"] if row["id"] == scan_id)
    for root in scan["roots"]:
        (tmp_path / root).mkdir(parents=True, exist_ok=True)

    result = verifier.evaluate_scan(scan, tmp_path)

    assert result["matches"] == 1
    assert result["passed"] is False


def test_native_lane_cannot_permit_unqualified_targets() -> None:
    manifest = loaded_manifest()
    manifest["test_inventory_contract"]["native_execution_lanes"][0][
        "allow_additional_targets"
    ] = True

    with pytest.raises(
        verifier.ManifestError, match="permits an unqualified target"
    ):
        verifier.validate_manifest(manifest, REPOSITORY_ROOT)


@pytest.mark.parametrize(
    ("value", "expected"),
    [
        ([r"\[  SKIPPED \]"], True),
        (r"\[  SKIPPED \]", False),
        ([], False),
        ([r"\[  SKIPPED \]", r"\[  SKIPPED \]"], False),
    ],
)
def test_ctest_skip_marker_requires_the_exact_json_list_shape(
    value: object,
    expected: bool,
) -> None:
    runner = load_grouped_runner()

    assert verifier._ctest_skip_marker_is_exact(value) is expected
    assert runner.ctest_skip_marker_is_exact(value) is expected


def test_ordinary_native_command_rejects_lexical_path_substitution(
    tmp_path: Path,
) -> None:
    manifest = loaded_manifest()
    _, ordinary_sources = verifier._load_native_target_contract(
        manifest["test_inventory_contract"], manifest, REPOSITORY_ROOT
    )
    group_id = "deployment_release_baseline"
    names = sorted(verifier.NATIVE_ORDINARY_BY_GROUP[group_id])
    build_root = "build/deployment-release"
    tests: list[dict[str, object]] = []
    for name in names:
        command, environment = verifier._expected_native_ordinary(
            name, build_root, ordinary_sources[name][0]
        )
        properties = [
            {
                "name": "WORKING_DIRECTORY",
                "value": f"/qualification-output/{build_root}",
            }
        ]
        if environment:
            properties.append(
                {
                    "name": "ENVIRONMENT",
                    "value": ";".join(
                        f"{key}={value}" for key, value in environment.items()
                    ),
                }
            )
        tests.append(
            {"name": name, "command": command, "properties": properties}
        )
    command = tests[0]["command"]
    assert isinstance(command, list)
    command[0] = "/usr/bin/../bin/cmake"
    inventory = tmp_path / "inventory.json"
    inventory.write_text(json.dumps({"tests": tests}), encoding="utf-8")

    with pytest.raises(
        verifier.ManifestError, match="ordinary command/properties drifted"
    ):
        verifier._validate_native_target_mapping(
            inventory,
            lane_id="deployment_release",
            build_root=build_root,
            required_group=group_id,
            required_names=set(names),
            allow_additional_tests=False,
            target_groups={group_id: {}},
            ordinary_sources=ordinary_sources,
        )


@pytest.mark.parametrize(
    ("lane_id", "build_root", "source_group"),
    [
        (
            "deployment_release",
            "build/deployment-release",
            "deployment_release_baseline",
        ),
        (
            "roboplan_release",
            "build/roboplan-release/roboplan",
            "roboplan_release_baseline",
        ),
    ],
)
def test_ordinary_native_registry_accepts_exact_ctest_working_directory(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    lane_id: str,
    build_root: str,
    source_group: str,
) -> None:
    manifest = loaded_manifest()
    _, ordinary_sources = verifier._load_native_target_contract(
        manifest["test_inventory_contract"], manifest, REPOSITORY_ROOT
    )
    names = set(verifier.NATIVE_ORDINARY_BY_GROUP[source_group])
    group_id = f"synthetic_{lane_id}_ordinary"
    monkeypatch.setitem(verifier.NATIVE_ORDINARY_BY_GROUP, group_id, names)
    tests: list[dict[str, object]] = []
    for name in sorted(names):
        source = ordinary_sources[name][0]
        command, environment = verifier._expected_native_ordinary(
            name, build_root, source
        )
        properties: list[dict[str, object]] = [
            {
                "name": "WORKING_DIRECTORY",
                "value": f"/qualification-output/{build_root}",
            }
        ]
        if environment:
            properties.extend(
                [
                    {
                        "name": "ENVIRONMENT",
                        "value": [
                            f"{key}={value}"
                            for key, value in environment.items()
                        ],
                    },
                    {"name": "TIMEOUT", "value": 60.0},
                ]
            )
        tests.append(
            {"name": name, "command": command, "properties": properties}
        )
    inventory = tmp_path / "inventory.json"
    inventory.write_text(
        json.dumps({"tests": tests}),
        encoding="utf-8",
    )

    evidence = verifier._validate_native_target_mapping(
        inventory,
        lane_id=lane_id,
        build_root=build_root,
        required_group=group_id,
        required_names=names,
        allow_additional_tests=False,
        target_groups={group_id: {}},
        ordinary_sources=ordinary_sources,
    )

    assert {row["name"] for row in evidence["ordinary_tests"]} == names

    missing_working_directory = copy.deepcopy(tests)
    missing_working_directory[0]["properties"] = [
        row
        for row in missing_working_directory[0]["properties"]
        if row["name"] != "WORKING_DIRECTORY"
    ]
    inventory.write_text(
        json.dumps({"tests": missing_working_directory}), encoding="utf-8"
    )
    with pytest.raises(verifier.ManifestError, match="properties drifted"):
        verifier._validate_native_target_mapping(
            inventory,
            lane_id=lane_id,
            build_root=build_root,
            required_group=group_id,
            required_names=names,
            allow_additional_tests=False,
            target_groups={group_id: {}},
            ordinary_sources=ordinary_sources,
        )

    wrong_working_directory = copy.deepcopy(tests)
    next(
        row
        for row in wrong_working_directory[0]["properties"]
        if row["name"] == "WORKING_DIRECTORY"
    )["value"] = "/qualification-output/wrong-build"
    inventory.write_text(
        json.dumps({"tests": wrong_working_directory}), encoding="utf-8"
    )
    with pytest.raises(
        verifier.ManifestError, match="ordinary command/properties drifted"
    ):
        verifier._validate_native_target_mapping(
            inventory,
            lane_id=lane_id,
            build_root=build_root,
            required_group=group_id,
            required_names=names,
            allow_additional_tests=False,
            target_groups={group_id: {}},
            ordinary_sources=ordinary_sources,
        )


@pytest.mark.parametrize(
    "relative",
    [
        "scripts/ci/run-composable-cell-i2-core-tsan.sh",
        "scripts/ci/run-composable-cell-i2-roboplan-scene-sanitizer.sh",
    ],
)
def test_i2_tsan_entrypoints_self_bootstrap_no_randomize(
    relative: str,
) -> None:
    source = (REPOSITORY_ROOT / relative).read_text(encoding="utf-8")

    assert 'expected_personality="00040000"' in source
    assert 'current_personality="$(cat /proc/self/personality)"' in source
    assert 'exec /usr/bin/setarch x86_64 -R "$0" "$@"' in source
    assert 'child_personality="$(sh -c \'cat /proc/self/personality\')"' in source


def test_native_registry_discovery_uses_only_the_closed_environment(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    runner = load_grouped_runner()
    artifact_root = tmp_path / "artifacts"
    build_root = artifact_root / "build"
    inventory_path = artifact_root / "inventory.json"
    build_root.mkdir(parents=True)
    captured: dict[str, object] = {}

    def fake_run(argv, **kwargs):
        captured["argv"] = argv
        captured["env"] = kwargs["env"]
        return runner.subprocess.CompletedProcess(
            argv, 0, stdout='{"tests":[]}\n', stderr=""
        )

    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    closed = {"PATH": "/usr/bin", "LANG": "C.UTF-8", "TZ": "UTC"}

    runner.load_registry(
        build_root,
        inventory_path,
        artifact_root,
        None,
        closed,
    )

    assert captured["env"] == closed
    assert json.loads(inventory_path.read_text(encoding="utf-8")) == {"tests": []}


@pytest.mark.parametrize(
    "marker",
    [
        "[  SKIPPED ] dependency absent\n",
        "SKIP: native capability unavailable\n",
        "dependency_unavailable: optional backend\n",
    ],
)
def test_ordinary_native_exit_zero_cannot_report_skip_or_unavailable(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    marker: str,
) -> None:
    runner = load_grouped_runner()
    artifact_root = tmp_path / "artifacts"
    result_root = artifact_root / "native"
    result_root.mkdir(parents=True)
    monkeypatch.setattr(
        runner,
        "run_process",
        lambda *_args, **_kwargs: (0, marker, False),
    )

    with pytest.raises(
        RuntimeError, match="reported a skipped or unavailable result"
    ):
        runner.process_record(
            kind="ordinary_direct",
            argv=["/usr/bin/true"],
            cwd=artifact_root,
            timeout_seconds=1.0,
            result_path=result_root / "result.json",
            log_path=result_root / "result.log",
            artifact_root=artifact_root,
            registered_tests=["OrdinaryTest"],
            binary=None,
            base_environment={},
            environment={},
        )


def _native_receipt(
    runner,
    *,
    lane_id: str,
    inventory_path: Path,
    provenance_path: str,
    provenance_sha256: str,
) -> dict[str, str]:
    return {
        "schema_version": runner.PREEXECUTION_RECEIPT_SCHEMA,
        "lane_id": lane_id,
        "inventory_sha256": runner.sha256_file(inventory_path),
        "provenance_path": provenance_path,
        "provenance_sha256": provenance_sha256,
        "provenance_evidence_sha256": "1" * 64,
        "native_target_mapping_sha256": "2" * 64,
    }


@pytest.mark.parametrize("unsafe_path", ["../proof.json", "/tmp/proof.json"])
def test_native_preexecution_receipt_rejects_unsafe_provenance_path(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    unsafe_path: str,
) -> None:
    runner = load_grouped_runner()
    artifact_root = tmp_path / "artifacts"
    artifact_root.mkdir()
    inventory_path = artifact_root / "inventory.json"
    inventory_path.write_text('{"tests":[]}\n', encoding="utf-8")
    tool = tmp_path / "tool"
    tool.write_text("packet\n", encoding="utf-8")
    monkeypatch.setattr(runner, "PYTHON", tool)
    monkeypatch.setattr(runner, "PACKET_VERIFIER", tool)
    monkeypatch.setattr(runner, "PACKET_MANIFEST", tool)
    receipt = _native_receipt(
        runner,
        lane_id="core_release",
        inventory_path=inventory_path,
        provenance_path=unsafe_path,
        provenance_sha256="0" * 64,
    )
    monkeypatch.setattr(
        runner.subprocess,
        "run",
        lambda *args, **kwargs: runner.subprocess.CompletedProcess(
            args[0], 0, stdout=json.dumps(receipt), stderr=""
        ),
    )

    with pytest.raises(RuntimeError, match="not canonical and relative"):
        runner.validate_frozen_registry(
            "core_release", inventory_path, artifact_root, {}
        )


def test_native_preexecution_receipt_rejects_provenance_digest_drift(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    runner = load_grouped_runner()
    artifact_root = tmp_path / "artifacts"
    provenance = artifact_root / "native/provenance.json"
    provenance.parent.mkdir(parents=True)
    provenance.write_text('{"passed":true}\n', encoding="utf-8")
    inventory_path = artifact_root / "inventory.json"
    inventory_path.write_text('{"tests":[]}\n', encoding="utf-8")
    tool = tmp_path / "tool"
    tool.write_text("packet\n", encoding="utf-8")
    monkeypatch.setattr(runner, "PYTHON", tool)
    monkeypatch.setattr(runner, "PACKET_VERIFIER", tool)
    monkeypatch.setattr(runner, "PACKET_MANIFEST", tool)
    receipt = _native_receipt(
        runner,
        lane_id="core_release",
        inventory_path=inventory_path,
        provenance_path="native/provenance.json",
        provenance_sha256="0" * 64,
    )
    monkeypatch.setattr(
        runner.subprocess,
        "run",
        lambda *args, **kwargs: runner.subprocess.CompletedProcess(
            args[0], 0, stdout=json.dumps(receipt), stderr=""
        ),
    )

    with pytest.raises(RuntimeError, match="bytes differ"):
        runner.validate_frozen_registry(
            "core_release", inventory_path, artifact_root, {}
        )


def test_forbidden_surface_scan_lane_declares_its_exact_artifact() -> None:
    manifest = loaded_manifest()
    lane = next(
        row
        for row in manifest["qualification_lanes"]
        if row["id"] == "forbidden_surface_scan"
    )

    assert lane["container_command"] == [
        "python3",
        "verification/composable_cell_i2/verify_qualification_manifest.py",
        "--manifest",
        "verification/composable_cell_i2/qualification_manifest.v1.json",
        "--scan",
        "--output-root",
        "/qualification-output",
    ]
    assert lane["expected_outputs"] == [verifier.FORBIDDEN_SCAN_OUTPUT]
    with pytest.raises(verifier.ManifestError, match="--scan requires --output-root"):
        verifier._forbidden_scan_output_root(None)


def test_forbidden_surface_scan_artifact_is_exclusive_validated_and_sealable(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    source_root = tmp_path / "source"
    source_root.mkdir()
    (source_root / "source.py").write_text("value = 1\n", encoding="utf-8")
    artifact_root = tmp_path / "artifacts"
    lane_root = artifact_root / "lanes/forbidden_surface_scan"
    lane_root.mkdir(parents=True)
    seal_root = tmp_path / "artifact-seals"
    seal_root.mkdir()
    manifest = {
        "forbidden_scans": [
            {
                "id": scan_id,
                "mode": "file_regex",
                "roots": ["source"],
                "pattern": "__absent_i2_forbidden_surface__",
                "globs": ["*.py"],
                "exclude_globs": [],
                "expected_matches": 0,
            }
            for scan_id in sorted(verifier.REQUIRED_SCAN_IDS)
        ]
    }
    monkeypatch.setattr(
        verifier, "_forbidden_scan_output_root", lambda requested: lane_root
    )

    results = verifier.run_scans(manifest, tmp_path, lane_root)
    result_path = lane_root / verifier.FORBIDDEN_SCAN_OUTPUT

    assert result_path.stat().st_size > 0
    assert json.loads(result_path.read_text(encoding="utf-8")) == results
    validated = verifier._validate_scan_output(
        manifest,
        artifact_root,
        {"forbidden_surface_scan"},
        tmp_path,
    )
    assert validated["results"] == results
    assert validated["result_file"]["path"] == (
        "lanes/forbidden_surface_scan/forbidden-surface-scan.json"
    )
    seal = verifier._seal_lane_artifacts(
        artifact_root, seal_root, "forbidden_surface_scan"
    )
    assert seal["inventory_count"] == 1
    with pytest.raises(
        verifier.ManifestError, match="cannot exclusively create host evidence file"
    ):
        verifier.run_scans(manifest, tmp_path, lane_root)


def test_output_evidence_rejects_empty_files_and_symlinks(tmp_path: Path) -> None:
    empty = tmp_path / "empty.json"
    empty.write_text("", encoding="utf-8")
    target = tmp_path / "target.json"
    target.write_text("{}\n", encoding="utf-8")
    symlink = tmp_path / "linked.json"
    symlink.symlink_to(target)

    with pytest.raises(verifier.ManifestError, match="must not be empty"):
        verifier._require_output_file(tmp_path, "empty.json", "empty evidence")
    with pytest.raises(verifier.ManifestError, match="symlinked path component"):
        verifier._require_output_file(tmp_path, "linked.json", "linked evidence")

    nested = tmp_path / "nested"
    nested.symlink_to(tmp_path, target_is_directory=True)
    with pytest.raises(verifier.ManifestError, match="symlinked path component"):
        verifier._require_output_file(
            tmp_path, "nested/target.json", "intermediate symlink evidence"
        )

    hardlink = tmp_path / "hardlink.json"
    hardlink.hardlink_to(target)
    with pytest.raises(verifier.ManifestError, match="exactly one hard link"):
        verifier._require_output_file(tmp_path, "hardlink.json", "hardlink evidence")


def test_evidence_required_field_set_cannot_be_weakened() -> None:
    manifest = loaded_manifest()
    manifest["evidence_contract"]["required_fields"].remove(
        "binding_junit_summary"
    )

    with pytest.raises(verifier.ManifestError, match="required-field set"):
        verifier.validate_manifest(manifest, REPOSITORY_ROOT)


def test_generated_api_inventory_requires_read_only_nonconstructible_trust(
    tmp_path: Path,
) -> None:
    manifest = loaded_manifest()
    required = manifest["binding_contract"]["generated_api_contract"][
        "required_read_only_nonconstructible_exports"
    ]
    inventory = {
        "schema_version": "manyforge.composable_cell_i2.binding_api_inventory.v1",
        "module": "manyforge_core_py",
        "framework": "pybind11",
        "python_abi": {
            "implementation": "CPython",
            "version": "3.12.13",
            "soabi": "cpython-312-x86_64-linux-gnu",
            "extension_suffix": ".cpython-312-x86_64-linux-gnu.so",
            "gil_disabled": False,
        },
        "exports": [
            {
                "name": name,
                "kind": "class",
                "constructible": False,
                "writable_attributes": [],
                "trusted_projection": True,
            }
            for name in required
        ],
    }
    path = tmp_path / "api.json"
    path.write_text(json.dumps(inventory), encoding="utf-8")

    assert required[0] in verifier._validate_generated_api_inventory(manifest, path)

    inventory["exports"][0]["constructible"] = True
    path.write_text(json.dumps(inventory), encoding="utf-8")
    with pytest.raises(verifier.ManifestError, match="constructible or writable"):
        verifier._validate_generated_api_inventory(manifest, path)


def test_grouped_result_rejects_a_producer_authored_summary(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    manifest = loaded_manifest()
    inventory = json.loads(
        (
            REPOSITORY_ROOT
            / "verification/composable_cell_i2/baseline_test_inventory.v1.json"
        ).read_text(encoding="utf-8")
    )
    names = inventory["groups"]["roboplan_scene_sanitizer_required"]["names"]
    artifact_root = tmp_path / "artifacts"
    log_root = tmp_path / "logs"
    lane_root = artifact_root / "lanes/roboplan_asan_ubsan"
    inventory_path = lane_root / "roboplan-asan/ctest-inventory.json"
    inventory_path.parent.mkdir(parents=True)
    log_root.mkdir()
    binary = (
        "/qualification-output/build/roboplan-asan/build/"
        "manyforge_roboplan/test_roboplan_scene_generation"
    )
    library_path = ":".join(
        [
            "/qualification-output/build/roboplan-asan/install/roboplan/lib",
            (
                "/qualification-output/build/roboplan-asan/install/"
                "roboplan_example_models/lib"
            ),
            "/usr/local/lib",
            "/usr/local/lib/python3.12/site-packages/cmeel.prefix/lib",
        ]
    )
    inventory_path.write_text(
        json.dumps(
            {
                "tests": [
                    {
                        "name": name,
                        "command": [
                            binary,
                            f"--gtest_filter={name}",
                            "--gtest_also_run_disabled_tests",
                        ],
                        "properties": [
                            {
                                "name": "WORKING_DIRECTORY",
                                "value": str(Path(binary).parent),
                            },
                            {
                                "name": "SKIP_REGULAR_EXPRESSION",
                                "value": [r"\[  SKIPPED \]"],
                            },
                            {"name": "TIMEOUT", "value": 60},
                            {
                                "name": "ENVIRONMENT",
                                "value": f"LD_LIBRARY_PATH={library_path}",
                            },
                        ],
                    }
                    for name in names
                ]
            }
        ),
        encoding="utf-8",
    )
    provenance_path = lane_root / "roboplan-asan/native-build-provenance.json"
    provenance_path.write_text("{}\n", encoding="utf-8")
    monkeypatch.setattr(
        verifier,
        "_validate_native_build_provenance_artifact",
        lambda *_args, **_kwargs: (
            {"evidence_sha256": "0" * 64},
            {
                "build/roboplan-asan/build/manyforge_roboplan/"
                "test_roboplan_scene_generation": "0" * 64
            },
            [],
        ),
    )
    result_path = lane_root / "roboplan-asan/execution-index.json"
    result_path.write_text(
        json.dumps(
            {
                "schema_version": "manyforge.grouped_test_result.v1",
                "lane_id": "roboplan_asan_ubsan",
                "registered_tests": names,
                "enabled_tests": names,
                "disabled_tests": [],
                "excluded_tests": [],
                "executed_tests": names,
                "passed_tests": names[1:],
                "skipped_tests": names[:1],
                "failed_tests": [],
                "fallback_binaries": 0,
            }
        ),
        encoding="utf-8",
    )

    with pytest.raises(verifier.ManifestError, match="keys differ"):
        verifier._validate_grouped_results(
            manifest,
            artifact_root,
            log_root,
            {"roboplan_asan_ubsan"},
            REPOSITORY_ROOT,
            {},
        )


def _synthetic_native_provenance(
    *,
    row: dict[str, object],
    binary_logical: str,
    binary_sha256: str,
) -> dict[str, object]:
    evidence: dict[str, object] = {
        "schema_version": "manyforge.composable_cell_i2.native_build_provenance.v1",
        "lane_id": row["lane_id"],
        "required_group": row["required_group"],
        "sanitizer": row["sanitizer"],
        "build_root": f"/qualification-output/{row['build_root']}",
        "provenance_output": f"/qualification-output/{row['provenance_output']}",
        "projects": [],
        "targets": [
            {
                "role": "gtest",
                "artifact": {
                    "path": binary_logical,
                    "resolved_path": binary_logical,
                    "sha256": binary_sha256,
                },
            }
        ],
        "production_targets": [],
        "test_only_targets": [],
        "utility_target_relationships": [],
        "production_parity": [],
        "test_only_parity": [],
        "execution_parity": [],
        "cross_profile_parity": [],
        "ordinary_tests": [],
        "installed_copies": [],
        "qualified_install_inputs": [],
        "install_projects": [],
        "response_files": [],
        "external_dependencies": [],
        "inspection_tools": [],
        "passed": True,
    }
    evidence["evidence_sha256"] = verifier._canonical_json_sha256(evidence)
    return evidence


def test_host_rehashes_every_named_native_output_file(
    tmp_path: Path,
) -> None:
    row = loaded_manifest()["test_inventory_contract"]["native_execution_lanes"][0]
    lane_root = tmp_path / "lane"
    binary_relative = Path(row["build_root"]) / "test_synthetic"
    binary = lane_root / binary_relative
    binary.parent.mkdir(parents=True)
    binary.write_bytes(b"\x7fELFsynthetic")
    binary_logical = f"/qualification-output/{binary_relative.as_posix()}"
    evidence = _synthetic_native_provenance(
        row=row,
        binary_logical=binary_logical,
        binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
    )
    provenance = lane_root / row["provenance_output"]
    provenance.parent.mkdir(parents=True, exist_ok=True)
    provenance.write_text(json.dumps(evidence), encoding="utf-8")

    files = verifier._authenticate_provenance_files(
        evidence,
        lane_root=lane_root,
        repository_root=REPOSITORY_ROOT,
        label="synthetic native provenance",
    )
    assert {file["path"] for file in files} == {binary_logical}

    binary.write_bytes(b"\x7fELFmutated")
    with pytest.raises(verifier.ManifestError, match="changed after"):
        verifier._authenticate_provenance_files(
            evidence,
            lane_root=lane_root,
            repository_root=REPOSITORY_ROOT,
            label="synthetic native provenance",
        )


def test_host_native_provenance_rejects_self_hash_drift(tmp_path: Path) -> None:
    row = loaded_manifest()["test_inventory_contract"]["native_execution_lanes"][0]
    lane_root = tmp_path / "lane"
    binary_relative = Path(row["build_root"]) / "test_synthetic"
    binary = lane_root / binary_relative
    binary.parent.mkdir(parents=True)
    binary.write_bytes(b"\x7fELFsynthetic")
    evidence = _synthetic_native_provenance(
        row=row,
        binary_logical=(
            f"/qualification-output/{binary_relative.as_posix()}"
        ),
        binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
    )
    evidence["passed"] = False
    provenance = lane_root / row["provenance_output"]
    provenance.parent.mkdir(parents=True, exist_ok=True)
    provenance.write_text(json.dumps(evidence), encoding="utf-8")

    with pytest.raises(verifier.ManifestError, match="identity drifted"):
        verifier._validate_native_build_provenance_artifact(
            provenance,
            lane_root=lane_root,
            repository_root=REPOSITORY_ROOT,
            row=row,
            target_mapping={},
            build_contract=json.loads(
                (
                    REPOSITORY_ROOT
                    / "verification/composable_cell_i2/"
                    "build_provenance_contract.v1.json"
                ).read_text(encoding="utf-8")
            ),
            expected_manyforge_generation={},
        )


def test_historical_packet_lineage_is_exact_and_non_qualifying() -> None:
    manifest = loaded_manifest()

    lineage = verifier._validate_historical_packet_lineage(
        manifest, REPOSITORY_ROOT
    )

    assert lineage["disposition"] == "historical_non_qualifying"
    assert lineage["initial_packet_commit"] == verifier.HISTORICAL_PACKET_COMMIT
    assert lineage["initial_packet_tree"] == verifier.HISTORICAL_PACKET_TREE
    assert lineage["initial_packet_parent_commit"] == verifier.IMPLEMENTATION_BASES[
        "manyforge"
    ]["head_commit"]
    assert {row["path"] for row in lineage["files"]} == (
        verifier.HISTORICAL_NEW_PACKET_PATHS | verifier.HISTORICAL_SUPPORT_PATHS
    )
    assert sum(row["origin"] == "initial_new" for row in lineage["files"]) == 17
    assert all(row["initial_git_blob"] for row in lineage["files"])
    assert verifier._git_head(REPOSITORY_ROOT) != lineage["initial_packet_commit"]


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("disposition", "qualified"),
        ("initial_packet_commit", "0" * 40),
        ("initial_packet_tree", "0" * 40),
        ("initial_packet_parent_commit", "0" * 40),
    ],
)
def test_historical_packet_lineage_identity_is_closed(
    field: str, value: str
) -> None:
    manifest = loaded_manifest()
    manifest["historical_packet_lineage"][field] = value

    with pytest.raises(
        verifier.ManifestError, match="historical packet lineage identity drifted"
    ):
        verifier._validate_historical_packet_lineage(
            manifest, REPOSITORY_ROOT
        )


@pytest.mark.parametrize(
    "obsolete_field",
    [
        "freeze_commit_directly_follows_implementation_base",
        "blob_and_mode_immutable_after_freeze",
        "external_anchor_path",
    ],
)
def test_historical_packet_lineage_rejects_obsolete_process_rules(
    obsolete_field: str,
) -> None:
    manifest = loaded_manifest()
    manifest["historical_packet_lineage"][obsolete_field] = True

    with pytest.raises(verifier.ManifestError, match="keys differ"):
        verifier._validate_historical_packet_lineage(manifest, REPOSITORY_ROOT)


def test_candidate_must_descend_from_implementation_base() -> None:
    base = verifier.IMPLEMENTATION_BASES["manyforge"]["head_commit"]
    verifier._require_commit_descends_from(
        REPOSITORY_ROOT,
        candidate=verifier._git_head(REPOSITORY_ROOT),
        base=base,
        label="ManyForge",
    )

    with pytest.raises(verifier.ManifestError, match="does not descend"):
        verifier._require_commit_descends_from(
            REPOSITORY_ROOT,
            candidate="08222188293df8e12f90dc72e204c21f22f92ea1",
            base=base,
            label="ManyForge",
        )


def test_binding_measurement_counts_reject_json_booleans() -> None:
    with pytest.raises(verifier.ManifestError, match="non-negative integer"):
        verifier._validate_binding_measurements(
            "ros_free_import_and_sole_module_inventory",
            {
                "imported_module_count": True,
                "forbidden_module_count": False,
                "ros_module_count": False,
                "extension_suffix_matches": True,
            },
        )

    predecessor_measurements = {
        "projection_count": len(binding_cases._PREDECESSORS),
        "construct_attempts": 2 * len(binding_cases._PREDECESSORS),
        "construct_rejections": 2 * len(binding_cases._PREDECESSORS),
        "write_attempts": 1,
        "write_rejections": 1,
    }
    verifier._validate_binding_measurements(
        "all_trusted_predecessor_projections_nonconstructible_and_read_only",
        predecessor_measurements,
    )
    predecessor_measurements["projection_count"] -= 1
    with pytest.raises(verifier.ManifestError, match="measurements failed"):
        verifier._validate_binding_measurements(
            "all_trusted_predecessor_projections_nonconstructible_and_read_only",
            predecessor_measurements,
        )


def _synthetic_pinned_identity(
    path: str,
    *,
    digest: str,
    scope: str = "pinned_container",
) -> dict[str, object]:
    return {
        "path": path,
        "resolved_path": path,
        "symlink_chain": [path],
        "sha256": digest,
        "scope": scope,
    }


def _synthetic_elf(
    dependency: dict[str, object],
) -> dict[str, object]:
    return {
        "path": "/qualification-output/synthetic",
        "resolved_path": "/qualification-output/synthetic",
        "sha256": "0" * 64,
        "readelf_header_sha256": "1" * 64,
        "readelf_program_headers_sha256": "2" * 64,
        "readelf_dynamic_sha256": "3" * 64,
        "readelf_symbols_sha256": "4" * 64,
        "ldd_normalized_sha256": "5" * 64,
        "needed": [dependency["soname"]],
        "python_init_symbols": [],
        "dependencies": [dependency],
    }


def _synthetic_project_build_tools() -> dict[str, object]:
    toolchain = loaded_build_contract()["toolchain_contract"]
    project: dict[str, object] = {"configure_executed_fixtures": []}
    for evidence_field, contract_field, version_field, expected_version in (
        verifier.PROJECT_BUILD_TOOL_FIELDS
    ):
        contract = toolchain[contract_field]
        row: dict[str, object] = {
            "path": contract["lexical_path"],
            "resolved_path": contract["resolved_path"],
            "sha256": contract["sha256"],
        }
        if version_field is not None:
            row[version_field] = (
                expected_version
                if expected_version is not None
                else contract[version_field]
            )
        project[evidence_field] = row
    python = toolchain["python"]
    project["configure_executed_fixtures"] = [
        {
            "interpreter": {
                "path": python["lexical_path"],
                "resolved_path": python["resolved_path"],
                "sha256": python["sha256"],
            }
        }
    ]
    return project


def _synthetic_pinned_provenance() -> dict[str, object]:
    link_input = {
        **_synthetic_pinned_identity(
            "/usr/lib/libsynthetic.a", digest="a" * 64
        ),
        "kind": "archive",
    }
    command_input = {
        **_synthetic_pinned_identity(
            "/usr/lib/libcommand.so", digest="b" * 64
        ),
        "kind": "shared_library",
    }
    compile_dependency = {
        **_synthetic_pinned_identity(
            "/usr/include/synthetic.hpp", digest="c" * 64
        ),
        "cross_profile_sha256": "c" * 64,
        "producer": None,
        "source_identity": None,
    }
    transitive = _synthetic_pinned_identity(
        "/usr/lib/libtransitive.so", digest="d" * 64
    )
    elf_dependency = {
        "soname": "libelf-synthetic.so",
        **_synthetic_pinned_identity(
            "/usr/lib/libelf-synthetic.so", digest="e" * 64
        ),
    }
    proof = {
        "artifact": {
            "path": "/qualification-output/synthetic",
            "resolved_path": "/qualification-output/synthetic",
            "sha256": "0" * 64,
        },
        "commands": [
            {
                "kind": "link",
                "input_evidence": [link_input],
                "dependency_closure": None,
            },
            {
                "kind": "archive",
                "input_evidence": [command_input],
                "dependency_closure": None,
            },
            {
                "kind": "compile",
                "input_evidence": [],
                "dependency_closure": {"files": [compile_dependency]},
            },
        ],
        "transitive_external_link_inputs": [transitive],
        "elf": _synthetic_elf(elf_dependency),
    }
    return {
        "projects": [_synthetic_project_build_tools()],
        "production_targets": [],
        "test_only_targets": [],
        "targets": [proof],
        "external_dependencies": [
            {
                "soname": elf_dependency["soname"],
                "resolved_path": elf_dependency["resolved_path"],
                "sha256": elf_dependency["sha256"],
            }
        ],
        "installed_copies": [],
        "inspection_tools": [
            {
                "path": path,
                "resolved_path": path,
                "sha256": str(index) * 64,
            }
            for index, path in enumerate(
                verifier.PINNED_CONTAINER_INSPECTION_TOOLS, start=1
            )
        ],
    }


def test_pinned_container_claim_collection_preserves_each_route() -> None:
    claims = verifier._collect_provenance_pinned_container_claims(
        _synthetic_pinned_provenance(),
        family="native",
        build_contract=loaded_build_contract(),
        lane_root=None,
        label="synthetic native provenance",
    )

    assert {claim["claim_kind"] for claim in claims} == set(
        verifier.PINNED_CONTAINER_CLAIM_KINDS
    )
    link_claim = next(
        claim for claim in claims if claim["claim_kind"] == "link_command_input"
    )
    assert link_claim["content_format"] == "archive"
    assert next(
        claim for claim in claims if claim["claim_kind"] == "elf_dependency"
    )["content_format"] == "elf"
    assert next(
        claim
        for claim in claims
        if claim["claim_kind"] == "transitive_link_input"
    )["content_format"] == "elf"
    compile_claim = next(
        claim for claim in claims if claim["claim_kind"] == "compile_dependency"
    )
    assert compile_claim["symlink_chain"] == [compile_claim["path"]]
    assert len(
        [claim for claim in claims if claim["claim_kind"] == "build_tool"]
    ) == len(verifier.PINNED_CONTAINER_BUILD_TOOLS) + 1
    build_tool_claims = [
        claim for claim in claims if claim["claim_kind"] == "build_tool"
    ]
    assert all(claim["content_format"] is None for claim in build_tool_claims)
    python_claim = next(
        claim
        for claim in build_tool_claims
        if claim["path"] == "/usr/local/bin/python3"
    )
    assert python_claim["symlink_chain"] == [
        "/usr/local/bin/python3",
        "/usr/local/bin/python3.12",
    ]


def test_pinned_container_claim_collection_rejects_external_projection_drift() -> None:
    evidence = _synthetic_pinned_provenance()
    evidence["external_dependencies"] = []

    with pytest.raises(verifier.ManifestError, match="exact pinned ELF projection"):
        verifier._collect_provenance_pinned_container_claims(
            evidence,
            family="native",
            build_contract=loaded_build_contract(),
            lane_root=None,
            label="synthetic native provenance",
        )


def test_pinned_container_claim_collection_rejects_compile_endpoint_escape() -> None:
    evidence = _synthetic_pinned_provenance()
    dependency = evidence["targets"][0]["commands"][2]["dependency_closure"][
        "files"
    ][0]
    dependency["resolved_path"] = "/opt/synthetic.hpp"
    dependency["symlink_chain"] = [
        dependency["path"],
        dependency["resolved_path"],
    ]

    with pytest.raises(
        verifier.ManifestError,
        match="endpoint-only pinned compiler semantics",
    ):
        verifier._collect_provenance_pinned_container_claims(
            evidence,
            family="native",
            build_contract=loaded_build_contract(),
            lane_root=None,
            label="synthetic native provenance",
        )


def test_pinned_container_claim_collection_rejects_project_tool_drift() -> None:
    evidence = _synthetic_pinned_provenance()
    evidence["projects"][0]["cmake"]["sha256"] = "f" * 64

    with pytest.raises(verifier.ManifestError, match="frozen build-tool identity"):
        verifier._collect_provenance_pinned_container_claims(
            evidence,
            family="native",
            build_contract=loaded_build_contract(),
            lane_root=None,
            label="synthetic native provenance",
        )


def test_pinned_container_claim_collection_rejects_fixture_interpreter_drift() -> None:
    evidence = _synthetic_pinned_provenance()
    interpreter = evidence["projects"][0]["configure_executed_fixtures"][0][
        "interpreter"
    ]
    interpreter["sha256"] = "f" * 64

    with pytest.raises(verifier.ManifestError, match="frozen build-tool identity"):
        verifier._collect_provenance_pinned_container_claims(
            evidence,
            family="native",
            build_contract=loaded_build_contract(),
            lane_root=None,
            label="synthetic native provenance",
        )


def test_pinned_container_claim_collection_rejects_ninja_version_drift() -> None:
    evidence = _synthetic_pinned_provenance()
    evidence["projects"][0]["ninja"]["version"] = "1.11.2"

    with pytest.raises(verifier.ManifestError, match="frozen build-tool version"):
        verifier._collect_provenance_pinned_container_claims(
            evidence,
            family="native",
            build_contract=loaded_build_contract(),
            lane_root=None,
            label="synthetic native provenance",
        )


@pytest.mark.parametrize("mutation", ["empty", "missing_needed"])
def test_pinned_container_claim_collection_rejects_open_elf_closure(
    mutation: str,
) -> None:
    evidence = _synthetic_pinned_provenance()
    elf = evidence["targets"][0]["elf"]
    if mutation == "empty":
        elf["dependencies"] = []
        evidence["external_dependencies"] = []
    else:
        elf["needed"] = ["libmissing.so"]

    with pytest.raises(
        verifier.ManifestError, match="runtime dependency/DT_NEEDED closure drifted"
    ):
        verifier._collect_provenance_pinned_container_claims(
            evidence,
            family="native",
            build_contract=loaded_build_contract(),
            lane_root=None,
            label="synthetic native provenance",
        )


def test_pinned_container_elf_allows_empty_needed_with_nonempty_dependencies(
) -> None:
    evidence = _synthetic_pinned_provenance()
    evidence["targets"][0]["elf"]["needed"] = []

    claims = verifier._collect_provenance_pinned_container_claims(
        evidence,
        family="native",
        build_contract=loaded_build_contract(),
        lane_root=None,
        label="synthetic native provenance",
    )

    assert any(claim["claim_kind"] == "elf_dependency" for claim in claims)


def test_pinned_container_claim_collection_rejects_elf_artifact_mismatch() -> None:
    evidence = _synthetic_pinned_provenance()
    evidence["targets"][0]["elf"]["sha256"] = "f" * 64

    with pytest.raises(verifier.ManifestError, match="ELF/artifact identity drifted"):
        verifier._collect_provenance_pinned_container_claims(
            evidence,
            family="native",
            build_contract=loaded_build_contract(),
            lane_root=None,
            label="synthetic native provenance",
        )


@pytest.mark.parametrize("mutation", ["duplicate_needed", "duplicate_soname"])
def test_pinned_container_claim_collection_rejects_duplicate_elf_names(
    mutation: str,
) -> None:
    evidence = _synthetic_pinned_provenance()
    elf = evidence["targets"][0]["elf"]
    if mutation == "duplicate_needed":
        elf["needed"] = [elf["needed"][0], elf["needed"][0]]
        message = "needed is not exact"
    else:
        elf["dependencies"].append(copy.deepcopy(elf["dependencies"][0]))
        message = "runtime dependency identity drifted"

    with pytest.raises(verifier.ManifestError, match=message):
        verifier._collect_provenance_pinned_container_claims(
            evidence,
            family="native",
            build_contract=loaded_build_contract(),
            lane_root=None,
            label="synthetic native provenance",
        )


@pytest.mark.parametrize("mutation", ["bytes", "chain", "magic", "unowned"])
def test_pinned_container_claim_collection_rejects_unbound_local_elf_dependency(
    tmp_path: Path,
    mutation: str,
) -> None:
    evidence = _synthetic_pinned_provenance()
    logical = "/qualification-output/local/libowned.so"
    physical = tmp_path / "local/libowned.so"
    physical.parent.mkdir(parents=True)
    physical.write_bytes(b"\x7fELFowned")
    digest = hashlib.sha256(physical.read_bytes()).hexdigest()
    dependency = {
        "soname": "liblocal.so",
        "path": logical,
        "resolved_path": logical,
        "symlink_chain": [logical],
        "sha256": digest,
        "scope": "qualification_output",
    }
    elf = evidence["targets"][0]["elf"]
    elf["dependencies"].append(dependency)
    elf["dependencies"].sort(
        key=lambda row: (row["soname"], row["resolved_path"])
    )
    if mutation != "unowned":
        evidence["production_targets"].append(
            {
                "artifact": {
                    "path": logical,
                    "resolved_path": logical,
                    "sha256": digest,
                },
                "commands": [],
                "transitive_external_link_inputs": [],
            }
        )
    if mutation == "bytes":
        dependency["sha256"] = "f" * 64
        evidence["production_targets"][0]["artifact"]["sha256"] = "f" * 64
    elif mutation == "chain":
        dependency["symlink_chain"] = [logical, "/qualification-output/other.so"]
    elif mutation == "magic":
        physical.write_bytes(b"not an ELF image")
        changed_digest = hashlib.sha256(physical.read_bytes()).hexdigest()
        dependency["sha256"] = changed_digest
        evidence["production_targets"][0]["artifact"][
            "sha256"
        ] = changed_digest

    with pytest.raises(
        verifier.ManifestError,
        match=(
            "not an ELF"
            if mutation == "magic"
            else "retained runtime owner/bytes drifted"
        ),
    ):
        verifier._collect_provenance_pinned_container_claims(
            evidence,
            family="native",
            build_contract=loaded_build_contract(),
            lane_root=tmp_path,
            label="synthetic native provenance",
        )


def _synthetic_binding_pinned_provenance() -> dict[str, object]:
    base = _synthetic_pinned_provenance()
    dependency = {
        "soname": "libsynthetic.so",
        **_synthetic_pinned_identity(
            "/usr/lib/libsynthetic.so", digest="e" * 64
        ),
    }
    elf = _synthetic_elf(dependency)
    elf["python_init_symbols"] = ["PyInit_manyforge_core_py"]
    return {
        "projects": base["projects"],
        "dependency_targets": [],
        "module_target": {
            "artifact": {
                "path": "/qualification-output/module/manyforge_core_py.so",
                "resolved_path": (
                    "/qualification-output/module/manyforge_core_py.so"
                ),
                "sha256": "9" * 64,
            },
            "commands": [],
            "transitive_external_link_inputs": [],
        },
        "staged_extension": {
            "path": elf["path"],
            "resolved_path": elf["resolved_path"],
            "sha256": elf["sha256"],
        },
        "elf": elf,
        "inspection_tools": base["inspection_tools"],
    }


def test_binding_provenance_collects_exact_staged_extension_elf() -> None:
    claims = verifier._collect_provenance_pinned_container_claims(
        _synthetic_binding_pinned_provenance(),
        family="binding",
        build_contract=loaded_build_contract(),
        lane_root=None,
        label="synthetic binding provenance",
    )

    assert any(claim["claim_kind"] == "elf_dependency" for claim in claims)


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        ("staged_identity", "ELF/artifact identity drifted"),
        ("wrong_pyinit", "PyInit/libpython contract"),
        ("libpython_needed", "PyInit/libpython contract"),
    ],
)
def test_binding_provenance_rejects_staged_elf_contract_drift(
    mutation: str,
    message: str,
) -> None:
    evidence = _synthetic_binding_pinned_provenance()
    if mutation == "staged_identity":
        evidence["staged_extension"]["sha256"] = "f" * 64
    elif mutation == "wrong_pyinit":
        evidence["elf"]["python_init_symbols"] = ["PyInit_other"]
    else:
        evidence["elf"]["needed"].append("libpython3.12.so.1.0")
        evidence["elf"]["needed"].sort()

    with pytest.raises(verifier.ManifestError, match=message):
        verifier._collect_provenance_pinned_container_claims(
            evidence,
            family="binding",
            build_contract=loaded_build_contract(),
            lane_root=None,
            label="synthetic binding provenance",
        )


@pytest.mark.parametrize("mutation", ["wrong_pyinit", "libpython_needed"])
def test_binding_extension_elf_contract_rejects_wrong_runtime_identity(
    mutation: str,
) -> None:
    dependency = {
        "soname": "libsynthetic.so",
        **_synthetic_pinned_identity(
            "/usr/lib/libsynthetic.so", digest="e" * 64
        ),
    }
    elf = _synthetic_elf(dependency)
    elf["python_init_symbols"] = ["PyInit_manyforge_core_py"]
    if mutation == "wrong_pyinit":
        elf["python_init_symbols"] = ["PyInit_other"]
    else:
        elf["needed"] = ["libpython3.12.so.1.0", dependency["soname"]]
        elf["needed"].sort()

    with pytest.raises(verifier.ManifestError, match="PyInit/libpython contract"):
        verifier._validate_binding_extension_elf_contract(
            elf, label="synthetic binding ELF"
        )


def test_binding_module_cannot_self_own_a_local_runtime_dependency(
    tmp_path: Path,
) -> None:
    base = _synthetic_pinned_provenance()
    module_path = "/qualification-output/module/manyforge_core_py.so"
    physical_module = tmp_path / "module/manyforge_core_py.so"
    physical_module.parent.mkdir(parents=True)
    physical_module.write_bytes(b"\x7fELFsynthetic binding module")
    module_digest = hashlib.sha256(physical_module.read_bytes()).hexdigest()
    module_artifact = {
        "path": module_path,
        "resolved_path": module_path,
        "sha256": module_digest,
    }
    dependency = {
        "soname": "manyforge_core_py.so",
        "path": module_path,
        "resolved_path": module_path,
        "symlink_chain": [module_path],
        "sha256": module_digest,
        "scope": "qualification_output",
    }
    elf = _synthetic_elf(dependency)
    elf["python_init_symbols"] = ["PyInit_manyforge_core_py"]
    evidence = {
        "projects": base["projects"],
        "dependency_targets": [],
        "module_target": {
            "artifact": module_artifact,
            "commands": [],
            "transitive_external_link_inputs": [],
        },
        "staged_extension": {
            "path": elf["path"],
            "resolved_path": elf["resolved_path"],
            "sha256": elf["sha256"],
        },
        "elf": elf,
        "inspection_tools": base["inspection_tools"],
    }

    with pytest.raises(
        verifier.ManifestError, match="retained runtime owner/bytes drifted"
    ):
        verifier._collect_provenance_pinned_container_claims(
            evidence,
            family="binding",
            build_contract=loaded_build_contract(),
            lane_root=tmp_path,
            label="synthetic binding provenance",
        )


def test_pinned_container_full_chain_cannot_escape_library_roots() -> None:
    escaped = _synthetic_pinned_identity(
        "/usr/lib/libsynthetic.so", digest="a" * 64
    )
    escaped["resolved_path"] = "/etc/passwd"
    escaped["symlink_chain"] = [escaped["path"], "/etc/passwd"]

    with pytest.raises(verifier.ManifestError, match="system-library roots"):
        verifier._pinned_container_claim(
            escaped,
            claim_kind="elf_dependency",
            label="escaped synthetic ELF dependency",
        )


def _synthetic_merged_usr_pinned_identity() -> dict[str, object]:
    path = "/usr/lib/x86_64-linux-gnu/libexpat.so"
    return {
        "path": path,
        "resolved_path": "/usr/lib/x86_64-linux-gnu/libexpat.so.1.8.10",
        "symlink_chain": [
            path,
            "/lib/x86_64-linux-gnu/libexpat.so.1.8.10",
        ],
        "sha256": "a" * 64,
        "scope": "pinned_container",
        "kind": "shared_library",
    }


def test_pinned_container_shape_accepts_merged_usr_lexical_chain_endpoint(
    tmp_path: Path,
) -> None:
    identity = _synthetic_merged_usr_pinned_identity()

    verifier._validate_replayed_file_identity(
        identity,
        lane_root=tmp_path / "unused-lane",
        repository_root=tmp_path / "unused-repository",
        label="synthetic merged-usr link input",
    )
    claim = verifier._pinned_container_claim(
        identity,
        claim_kind="link_command_input",
        label="synthetic merged-usr link input",
    )
    replayed = verifier._validate_pinned_container_replay_response(
        [claim],
        {
            "schema_version": verifier.PINNED_CONTAINER_REPLAY_RESPONSE_SCHEMA,
            "files": [_synthetic_replay_file(claim)],
        },
    )

    assert replayed[0]["resolved_path"] == identity["resolved_path"]
    assert replayed[0]["symlink_chain"] == identity["symlink_chain"]


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        ("empty", "pinned-container symlink evidence is malformed"),
        ("wrong_root", "pinned-container symlink evidence is malformed"),
        ("duplicate", "pinned-container symlink evidence is malformed"),
        ("noncanonical", "pinned-container symlink evidence is malformed"),
        ("root_escape", "system-library roots"),
    ],
)
def test_pinned_container_shape_rejects_malformed_merged_usr_claims(
    tmp_path: Path,
    mutation: str,
    message: str,
) -> None:
    identity = _synthetic_merged_usr_pinned_identity()
    path = str(identity["path"])
    if mutation == "empty":
        identity["symlink_chain"] = []
    elif mutation == "wrong_root":
        identity["symlink_chain"] = ["/lib/libexpat.so"]
    elif mutation == "duplicate":
        identity["symlink_chain"] = [path, path]
    elif mutation == "noncanonical":
        identity["symlink_chain"] = [path, "/lib/../lib/libexpat.so"]
    else:
        identity["resolved_path"] = "/etc/libexpat.so.1.8.10"
        identity["symlink_chain"] = [path, "/etc/libexpat.so.1.8.10"]

    with pytest.raises(verifier.ManifestError, match=message):
        verifier._validate_replayed_file_identity(
            identity,
            lane_root=tmp_path / "unused-lane",
            repository_root=tmp_path / "unused-repository",
            label="malformed synthetic merged-usr link input",
        )
        verifier._pinned_container_claim(
            identity,
            claim_kind="link_command_input",
            label="malformed synthetic merged-usr link input",
        )


@pytest.mark.parametrize("mutation", ["chain", "resolved_path", "sha256"])
def test_merged_usr_replay_rejects_authenticated_claim_drift(
    mutation: str,
) -> None:
    claim = verifier._pinned_container_claim(
        _synthetic_merged_usr_pinned_identity(),
        claim_kind="link_command_input",
        label="synthetic merged-usr link input",
    )
    replayed = _synthetic_replay_file(claim)
    if mutation == "chain":
        replayed["symlink_chain"] = [
            claim["path"],
            "/lib/x86_64-linux-gnu/libother.so.1",
        ]
    elif mutation == "resolved_path":
        replayed["resolved_path"] = (
            "/usr/lib/x86_64-linux-gnu/libother.so.1"
        )
    else:
        replayed["sha256"] = "f" * 64

    with pytest.raises(
        verifier.ManifestError,
        match="differs from its authenticated provenance claim",
    ):
        verifier._validate_pinned_container_replay_response(
            [claim],
            {
                "schema_version": (
                    verifier.PINNED_CONTAINER_REPLAY_RESPONSE_SCHEMA
                ),
                "files": [replayed],
            },
        )


def _synthetic_replay_file(
    claim: dict[str, object], *, size: int = 8
) -> dict[str, object]:
    return {
        "claim_kind": claim["claim_kind"],
        "content_format": claim["content_format"],
        "path": claim["path"],
        "resolved_path": claim["resolved_path"],
        "symlink_chain": claim["symlink_chain"],
        "sha256": claim["sha256"],
        "size": size,
    }


def test_pinned_container_replay_receipt_is_deduplicated_and_digest_only(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    compile_claim = verifier._pinned_container_claim(
        _synthetic_pinned_identity(
            "/usr/include/synthetic.hpp", digest="a" * 64
        ),
        claim_kind="compile_dependency",
        label="synthetic compiler dependency",
    )
    link_claim = verifier._pinned_container_claim(
        _synthetic_pinned_identity(
            "/usr/lib/libsynthetic.so", digest="b" * 64
        ),
        claim_kind="link_command_input",
        label="synthetic link input",
    )
    monkeypatch.setattr(
        verifier,
        "_collect_pinned_container_claims",
        lambda _manifest, _artifact_root, _successful, _build_contract: [
            compile_claim,
            compile_claim,
            link_claim,
        ],
    )
    captured_request: dict[str, object] = {}

    def replay(
        _argv: list[str], request_bytes: bytes, _repository_root: Path
    ) -> dict[str, object]:
        captured_request.update(json.loads(request_bytes))
        return {
            "schema_version": verifier.PINNED_CONTAINER_REPLAY_RESPONSE_SCHEMA,
            "files": [
                _synthetic_replay_file(claim)
                for claim in sorted(
                    [compile_claim, link_claim],
                    key=lambda row: (row["claim_kind"], row["path"]),
                )
            ],
        }

    monkeypatch.setattr(verifier, "_execute_pinned_container_replay", replay)
    receipt = verifier._collect_pinned_container_replay(
        loaded_manifest(), Path("/unused"), set(), REPOSITORY_ROOT
    )

    assert captured_request == {
        "schema_version": verifier.PINNED_CONTAINER_REPLAY_REQUEST_SCHEMA,
        "claims": [
            {
                "claim_kind": claim["claim_kind"],
                "path": claim["path"],
            }
            for claim in sorted(
                [compile_claim, link_claim],
                key=lambda row: (row["claim_kind"], row["path"]),
            )
        ],
    }
    assert receipt["claim_occurrence_count"] == 3
    assert receipt["unique_claim_count"] == 2
    assert receipt["replayed_file_count"] == 2
    assert receipt["batch_count"] == 1
    assert receipt["executed"] is True
    assert receipt["passed"] is True
    assert all(
        path not in json.dumps(receipt)
        for path in (compile_claim["path"], link_claim["path"])
    )


def test_pinned_container_replay_rejects_conflicting_same_path_claims(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    first = verifier._pinned_container_claim(
        _synthetic_pinned_identity(
            "/usr/include/synthetic.hpp", digest="a" * 64
        ),
        claim_kind="compile_dependency",
        label="first synthetic compiler dependency",
    )
    second = copy.deepcopy(first)
    second["sha256"] = "b" * 64
    monkeypatch.setattr(
        verifier,
        "_collect_pinned_container_claims",
        lambda _manifest, _artifact_root, _successful, _build_contract: [
            first,
            second,
        ],
    )

    with pytest.raises(verifier.ManifestError, match="conflicting claims"):
        verifier._collect_pinned_container_replay(
            loaded_manifest(), Path("/unused"), set(), REPOSITORY_ROOT
        )


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        (lambda files: files.clear(), "incomplete or unsorted"),
        (
            lambda files: files[0].update(sha256="f" * 64),
            "differs from its authenticated provenance claim",
        ),
        (
            lambda files: files[0].update(symlink_chain=["/usr/lib/other.so"]),
            "differs from its authenticated provenance claim",
        ),
        (
            lambda files: files[0].update(content_format="archive"),
            "differs from its authenticated provenance claim",
        ),
        (
            lambda files: files[0].update(
                resolved_path="/usr/lib/libother.so"
            ),
            "differs from its authenticated provenance claim",
        ),
        (
            lambda files: files[0].update(claim_kind="elf_dependency"),
            "duplicated or was not requested",
        ),
        (
            lambda files: files[0].update(path="/usr/lib/libother.so"),
            "duplicated or was not requested",
        ),
        (
            lambda files: files[0].update(size=-1),
            "differs from its authenticated provenance claim",
        ),
        (
            lambda files: files.append(copy.deepcopy(files[0])),
            "duplicated or was not requested",
        ),
    ],
)
def test_pinned_container_replay_response_rejects_drift(
    mutation: Callable[[list[dict[str, object]]], None],
    message: str,
) -> None:
    claim = verifier._pinned_container_claim(
        _synthetic_pinned_identity(
            "/usr/lib/libsynthetic.so", digest="a" * 64
        ),
        claim_kind="link_command_input",
        label="synthetic link input",
    )
    files = [_synthetic_replay_file(claim)]
    mutation(files)

    with pytest.raises(verifier.ManifestError, match=message):
        verifier._validate_pinned_container_replay_response(
            [claim],
            {
                "schema_version": verifier.PINNED_CONTAINER_REPLAY_RESPONSE_SCHEMA,
                "files": files,
            },
        )


def test_pinned_container_replay_response_rejects_schema_drift() -> None:
    with pytest.raises(verifier.ManifestError, match="response schema drifted"):
        verifier._validate_pinned_container_replay_response(
            [],
            {
                "schema_version": "invented.replay.response.v0",
                "files": [],
            },
        )


def test_pinned_container_replay_response_rejects_unsorted_rows() -> None:
    claims = [
        verifier._pinned_container_claim(
            _synthetic_pinned_identity(path, digest=digest),
            claim_kind=claim_kind,
            label=f"synthetic {claim_kind}",
        )
        for claim_kind, path, digest in (
            ("link_command_input", "/usr/lib/libdirect.so", "a" * 64),
            (
                "transitive_link_input",
                "/usr/lib/libtransitive.so",
                "b" * 64,
            ),
        )
    ]

    with pytest.raises(verifier.ManifestError, match="incomplete or unsorted"):
        verifier._validate_pinned_container_replay_response(
            claims,
            {
                "schema_version": (
                    verifier.PINNED_CONTAINER_REPLAY_RESPONSE_SCHEMA
                ),
                "files": [
                    _synthetic_replay_file(claim)
                    for claim in reversed(claims)
                ],
            },
        )


def test_pinned_container_replay_uses_one_mount_free_exact_image_batch() -> None:
    image_id = loaded_manifest()["qualification_environment"]["image"]["id"]
    argv = verifier._pinned_container_replay_argv(image_id)

    assert argv[:3] == ["docker", "run", "--rm"]
    assert image_id in argv
    assert "-i" in argv
    assert "--read-only" in argv
    assert argv[argv.index("--platform") + 1] == "linux/amd64"
    assert argv[argv.index("--network") + 1] == "none"
    assert argv[argv.index("--cap-drop") + 1] == "ALL"
    assert argv[argv.index("--security-opt") + 1] == (
        "no-new-privileges=true"
    )
    assert argv[argv.index("--entrypoint") + 1] == "/usr/local/bin/python3"
    assert argv[argv.index("--workdir") + 1] == "/"
    assert argv[argv.index(image_id) + 1 :] == [
        "-I",
        "-c",
        verifier.PINNED_CONTAINER_REPLAY_PROGRAM,
    ]
    assert "--mount" not in argv
    assert "--volume" not in argv
    assert "-v" not in argv
    assert "!<arch>\\n" in verifier.PINNED_CONTAINER_REPLAY_PROGRAM
    assert "\\x7fELF" in verifier.PINNED_CONTAINER_REPLAY_PROGRAM
    assert "endpoint(path, require_single_link=False)" in (
        verifier.PINNED_CONTAINER_REPLAY_PROGRAM
    )
    assert 'within(resolved, "/usr") or within(resolved, "/lib")' in (
        verifier.PINNED_CONTAINER_REPLAY_PROGRAM
    )


def _synthetic_complete_replay_claims() -> list[dict[str, object]]:
    paths_by_kind = {
        "build_tool": verifier.PINNED_CONTAINER_BUILD_TOOLS,
        "compile_dependency": ("/usr/include/synthetic.hpp",),
        "elf_dependency": ("/usr/lib/libelf-synthetic.so",),
        "inspection_tool": verifier.PINNED_CONTAINER_INSPECTION_TOOLS,
        "link_command_input": ("/usr/lib/libdirect.so",),
        "transitive_link_input": ("/usr/lib/libtransitive.so",),
    }
    claims: list[dict[str, object]] = []
    for claim_kind, paths in paths_by_kind.items():
        for path in paths:
            identity: dict[str, object] = {
                "path": path,
                "resolved_path": path,
                "sha256": hashlib.sha256(path.encode("utf-8")).hexdigest(),
            }
            if claim_kind not in {"build_tool", "inspection_tool"}:
                identity.update(
                    {
                        "scope": "pinned_container",
                        "symlink_chain": [path],
                    }
                )
            claims.append(
                verifier._pinned_container_claim(
                    identity,
                    claim_kind=claim_kind,
                    label=f"synthetic complete {claim_kind}",
                )
            )
    return claims


@pytest.mark.parametrize(
    "deficient_kind",
    [
        claim_kind
        for claim_kind, required_minimum in (
            verifier.PINNED_CONTAINER_COMPLETE_MINIMUM_UNIQUE_CLAIMS.items()
        )
        if required_minimum > 0
    ],
)
def test_complete_pinned_container_replay_rejects_each_required_kind_deficit(
    monkeypatch: pytest.MonkeyPatch,
    deficient_kind: str,
) -> None:
    claims = _synthetic_complete_replay_claims()
    claims.remove(
        next(claim for claim in claims if claim["claim_kind"] == deficient_kind)
    )
    monkeypatch.setattr(
        verifier,
        "_collect_pinned_container_claims",
        lambda _manifest, _artifact_root, _successful, _build_contract: claims,
    )

    with pytest.raises(verifier.ManifestError, match="claim-kind cardinality"):
        verifier._collect_pinned_container_replay(
            loaded_manifest(),
            Path("/unused"),
            verifier.REQUIRED_LANES,
            REPOSITORY_ROOT,
        )


def test_complete_pinned_container_replay_rejects_inspection_only_claims(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    inspection_claims = [
        verifier._pinned_container_claim(
            {
                "path": path,
                "resolved_path": path,
                "sha256": str(index) * 64,
            },
            claim_kind="inspection_tool",
            label=f"synthetic inspection tool {path}",
        )
        for index, path in enumerate(
            verifier.PINNED_CONTAINER_INSPECTION_TOOLS, start=1
        )
    ]
    monkeypatch.setattr(
        verifier,
        "_collect_pinned_container_claims",
        lambda _manifest, _artifact_root, _successful, _build_contract: (
            inspection_claims
        ),
    )

    with pytest.raises(verifier.ManifestError, match="claim-kind cardinality"):
        verifier._collect_pinned_container_replay(
            loaded_manifest(),
            Path("/unused"),
            verifier.REQUIRED_LANES,
            REPOSITORY_ROOT,
        )


def test_manifest_json_round_trip_preserves_contract() -> None:
    manifest = loaded_manifest()
    encoded = json.dumps(manifest, sort_keys=True, separators=(",", ":"))
    decoded = json.loads(encoded)

    assert decoded == manifest
    assert copy.deepcopy(decoded) == manifest
