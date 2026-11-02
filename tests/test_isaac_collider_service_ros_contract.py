"""Disposition evidence for the retired Isaac collider ROS v0.1 parser.

The historical parser is acceptance input for I9/I, not a runnable fallback.
This file deliberately does not import or reproduce that wire contract.
"""

from __future__ import annotations

import hashlib
import tomllib
from pathlib import Path


HISTORICAL_SOURCE_BLOB = "395d16c3144186745882e9b09bc797fd63e601f7"
HISTORICAL_SOURCE_SHA256 = (
    "a4fd242af4a13044cc31787a9aae66fe8a8b7c47bbfd2479862272b583c77c6f"
)
HISTORICAL_TEST_BLOB = "5ad1ccec1ecee67b7e75903c35ee6cdbdc8b9259"
HISTORICAL_TEST_SHA256 = (
    "6cda77b2d9cb8c81138b6a2a36dcdb4cd90bb50bf8cf351161a683603281775e"
)
HISTORICAL_TEST_NAME_DIGEST = (
    "d18b7e2c4fc0f079550adf45dd5f920c9ca68ada54e16e7e20210dfad6ec2356"
)
RESTORATION_GATE = "I9/I"

# These cohorts cover every definition in HISTORICAL_TEST_BLOB.  Their line
# ranges refer to that blob, not to this disposition file.  I9 must make an
# explicit new-contract decision for aliases and old wire spellings; history is
# test-design input, not an obligation to keep the v0.1 protocol compatible.
HISTORICAL_ACCEPTANCE_COHORTS = (
    (
        "test_parse_query_accepts_versioned_parameterized_request",
        "36-64",
        1,
        "accept and preserve the required fields of an explicitly versioned "
        "collider query",
    ),
    (
        "test_parse_query_rejects_unsafe_coercions",
        "67-84",
        7,
        "reject unsafe boolean, list, integer, enum, purpose, and version "
        "coercions",
    ),
    (
        "test_request_id_from_text_is_best_effort",
        "87-89",
        1,
        "preserve correlation when a request is rejected without trusting "
        "malformed input",
    ),
    (
        "test_parse_query_accepts_camel_case_mesh_export_dir",
        "92-98",
        1,
        "make an explicit migration decision for historical field aliases",
    ),
    (
        "test_parse_mesh_transfer_request_accepts_source_root_request",
        "101-120",
        1,
        "qualify owner-addressed scene-source mesh selection if I9 retains it",
    ),
    (
        "test_parse_mesh_transfer_request_accepts_file_uri_request",
        "123-130",
        1,
        "qualify bounded local artifact-source handling if I9 retains it",
    ),
    (
        "test_parse_mesh_transfer_request_rejects_invalid_payloads",
        "133-153",
        4,
        "reject ambiguous source selection and out-of-range transfer chunks",
    ),
)

# Exact implementation cohorts in HISTORICAL_SOURCE_BLOB.  The future owner is
# the I9/I Isaac adapter and its focused transport contract for every cohort.
HISTORICAL_SOURCE_COHORTS = (
    ("query DTO, version, and strict parser", "10-82"),
    ("best-effort request-correlation extraction", "85-90"),
    ("mesh-request DTO, version, and strict parser", "93-133"),
    ("shared strict field validation", "136-210"),
)


def _extension_paths() -> tuple[Path, Path, Path]:
    repo_root = Path(__file__).resolve().parents[1]
    extension_root = repo_root / "exts" / "manyforge.isaac.collider_service"
    extension_source = (
        extension_root
        / "manyforge"
        / "isaac"
        / "collider_service"
        / "extension.py"
    )
    retired_contract = extension_source.with_name("ros_contract.py")
    return extension_root, extension_source, retired_contract


def test_extension_activation_is_disabled_and_fails_before_ros_or_probe_work() -> None:
    extension_root, extension_path, _retired_contract = _extension_paths()
    source = extension_path.read_text(encoding="utf-8")
    startup = source.split("def on_startup", maxsplit=1)[1].split(
        "def on_shutdown", maxsplit=1
    )[0]
    config = tomllib.loads(
        (extension_root / "config" / "extension.toml").read_text(encoding="utf-8")
    )

    assert config["settings"]["exts"]["manyforge.isaac.collider_service"][
        "enabled"
    ] is False
    assert "composable_cell_runtime_unavailable" in source
    for forbidden in ("rclpy", "thread", "_load_probe_lib", "_start_ros"):
        assert forbidden not in startup


def test_retired_ros_contract_has_exact_i9_acceptance_disposition() -> None:
    _extension_root, extension_path, retired_contract = _extension_paths()
    extension_source = extension_path.read_text(encoding="utf-8")
    names = tuple(cohort[0] for cohort in HISTORICAL_ACCEPTANCE_COHORTS)
    ordered_name_bytes = ("\n".join(names) + "\n").encode("utf-8")

    assert not retired_contract.exists()
    for forbidden in (
        "from .ros_contract",
        "import ros_contract",
        "parse_query",
        "parse_mesh_transfer_request",
    ):
        assert forbidden not in extension_source
    assert HISTORICAL_SOURCE_BLOB == "395d16c3144186745882e9b09bc797fd63e601f7"
    assert HISTORICAL_SOURCE_SHA256 == (
        "a4fd242af4a13044cc31787a9aae66fe8a8b7c47bbfd2479862272b583c77c6f"
    )
    assert HISTORICAL_TEST_BLOB == "5ad1ccec1ecee67b7e75903c35ee6cdbdc8b9259"
    assert HISTORICAL_TEST_SHA256 == (
        "6cda77b2d9cb8c81138b6a2a36dcdb4cd90bb50bf8cf351161a683603281775e"
    )
    assert len(HISTORICAL_SOURCE_COHORTS) == 4
    assert len(HISTORICAL_ACCEPTANCE_COHORTS) == 7
    assert sum(cohort[2] for cohort in HISTORICAL_ACCEPTANCE_COHORTS) == 16
    assert hashlib.sha256(ordered_name_bytes).hexdigest() == (
        HISTORICAL_TEST_NAME_DIGEST
    )
    assert RESTORATION_GATE == "I9/I"
