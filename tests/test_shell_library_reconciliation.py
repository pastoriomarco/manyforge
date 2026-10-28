from __future__ import annotations

from pathlib import Path


_REPO_ROOT = Path(__file__).resolve().parents[1]
_LIB = _REPO_ROOT / "scripts" / "lib"


def test_deleted_runtime_owner_libraries_are_not_dormant() -> None:
    assert not (_LIB / "preflight.sh").exists()
    assert not (_LIB / "vllm-status-watch.sh").exists()
