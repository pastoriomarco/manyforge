"""Fail-closed PEP 517 boundary for the historical E0 binding proof."""

from __future__ import annotations

from typing import Never


UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
PINNED_SOURCE_COMMIT = "637894c460172b47faf64c7ece2e865bb8d69593"
RESTORATION_GATE = "I2"
UNAVAILABLE_MESSAGE = (
    f"{UNAVAILABLE_REASON_CODE}: the E0 Python-boundary proof is historical "
    f"evidence pinned to ManyForge commit {PINNED_SOURCE_COMMIT}; "
    f"current-branch activation returns only at the atomic {RESTORATION_GATE} "
    "Python/compiler binding cutover"
)


class HistoricalProofUnavailable(RuntimeError):
    """Raised before external build dependencies or an extension build begin."""


def _unavailable(*_args: object, **_kwargs: object) -> Never:
    raise HistoricalProofUnavailable(UNAVAILABLE_MESSAGE)


get_requires_for_build_wheel = _unavailable
prepare_metadata_for_build_wheel = _unavailable
build_wheel = _unavailable
get_requires_for_build_sdist = _unavailable
build_sdist = _unavailable
get_requires_for_build_editable = _unavailable
prepare_metadata_for_build_editable = _unavailable
build_editable = _unavailable
