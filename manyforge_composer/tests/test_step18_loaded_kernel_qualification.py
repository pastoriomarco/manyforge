"""Disposition contract for the historical loaded-Kernel campaign.

Historical blob ``1d7b9c307d4593f74f6eca6ea65e6a06c033c7bb`` at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` contains 29 cases.
The old binary, worker, container, and evidence formats are not current; their
safety and adversarial intent transfers to the I8/P5 loaded C++ Kernel gate.
"""

from typing import Final


HISTORICAL_CASE_COUNT: Final = 29
HISTORICAL_CASE_FAMILIES: Final = (
    (
        "bound_load_shape_and_primary_artifact_identity",
        "historical lines 409-502",
        7,
        ("I8", "P5"),
        "30-by-3 load, cancel inventory, worker presence, image/native binary, "
        "evidence logs, and retained-manifest mutation detection",
    ),
    (
        "complete_suite_environment_and_process_binding",
        "historical lines 515-632",
        8,
        ("I8", "P5"),
        "complete gtest log/suite, environment and preload rejection, exact "
        "outer Docker argv/fresh-build guard/spec scope, run-to-process UUID",
    ),
    (
        "worker_container_mount_and_image_fencing",
        "historical lines 647-730",
        7,
        ("I8", "P5"),
        "worker liveness/PID reuse/command/label identity, image reference, "
        "nested workspace rejection, and read-only mount enforcement",
    ),
    (
        "verifier_toctou_output_and_iteration_liveness",
        "historical lines 742-840",
        7,
        ("I8", "P5"),
        "verifier hash and final rehash, ignored output containment, postwrite "
        "cleanup, and per-iteration worker-liveness evidence",
    ),
)


def test_loaded_kernel_cases_are_fully_assigned_to_p5() -> None:
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES) == 29
    assert all(family[3] == ("I8", "P5") for family in HISTORICAL_CASE_FAMILIES)
