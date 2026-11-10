from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
from types import ModuleType
import uuid

import pytest


_REPO_ROOT = Path(__file__).resolve().parents[1]
_GENERATOR = _REPO_ROOT / "scripts" / "regen-public-contracts.py"
_FRESHNESS_WRAPPER = (
    _REPO_ROOT / "scripts" / "ci" / "check-public-contracts-fresh.sh"
)
_UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"


def _load_generator_module() -> ModuleType:
    spec = importlib.util.spec_from_file_location(
        f"regen_public_contracts_{uuid.uuid4().hex}",
        _GENERATOR,
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("arguments", ((), ("--check",)))
def test_historical_generator_shapes_fail_before_import_or_output_access(
    arguments: tuple[str, ...],
    tmp_path: Path,
) -> None:
    isolated_root = tmp_path / "isolated-repository"
    isolated_script = isolated_root / "scripts" / _GENERATOR.name
    isolated_script.parent.mkdir(parents=True)
    shutil.copyfile(_GENERATOR, isolated_script)

    reference = isolated_root / "docs" / "reference" / "REST_API.md"
    reference.parent.mkdir(parents=True)
    reference.write_text("sentinel-rest-reference\n", encoding="utf-8")

    import_marker = tmp_path / "composer-imported"
    fake_backend = isolated_root / "manyforge_composer" / "backend"
    fake_backend.mkdir(parents=True)
    (isolated_root / "manyforge_composer" / "__init__.py").write_text(
        "", encoding="utf-8"
    )
    (fake_backend / "__init__.py").write_text("", encoding="utf-8")
    (fake_backend / "app.py").write_text(
        "from pathlib import Path\n"
        "import os\n"
        "Path(os.environ['PUBLIC_CONTRACT_IMPORT_MARKER']).write_text('called')\n"
        "def create_app(state):\n"
        "    raise AssertionError('legacy aggregate imported Composer')\n",
        encoding="utf-8",
    )
    (fake_backend / "state.py").write_text(
        "class ComposerState:\n"
        "    pass\n",
        encoding="utf-8",
    )

    completed = subprocess.run(
        [sys.executable, str(isolated_script), *arguments],
        cwd=isolated_root,
        env={
            **os.environ,
            "PUBLIC_CONTRACT_IMPORT_MARKER": str(import_marker),
        },
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 78
    assert _UNAVAILABLE_REASON_CODE in completed.stderr
    assert "structural-rest" in completed.stderr
    assert reference.read_text(encoding="utf-8") == "sentinel-rest-reference\n"
    assert not import_marker.exists()

    source = isolated_script.read_text(encoding="utf-8")
    assert source.index('if __name__ == "__main__"') < source.index(
        "import argparse"
    )
    assert source.index('if __name__ == "__main__"') < source.index(
        "from pathlib import Path"
    )


def test_explicit_structural_rest_generator_routes_write_and_check(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    module = _load_generator_module()
    reference = tmp_path / "REST_API.md"
    calls: list[str] = []

    def generate() -> str:
        calls.append("structural-rest")
        return "generated structural REST\n"

    monkeypatch.setattr(module, "REST_REFERENCE_PATH", reference)
    monkeypatch.setattr(module, "REPO_ROOT", tmp_path)
    monkeypatch.setattr(module, "gen_rest_api", generate)

    assert module.main(["structural-rest"]) == 0
    assert reference.read_text(encoding="utf-8") == "generated structural REST\n"
    assert module.main(["structural-rest", "--check"]) == 0

    reference.write_text("stale structural REST\n", encoding="utf-8")
    assert module.main(["structural-rest", "--check"]) == 1
    assert reference.read_text(encoding="utf-8") == "stale structural REST\n"
    assert calls == ["structural-rest"] * 3


def test_explicit_headless_rest_generator_routes_write_and_check(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    module = _load_generator_module()
    reference = tmp_path / "HEADLESS_REST_API.md"
    calls: list[str] = []

    def generate() -> str:
        calls.append("headless-rest")
        return "generated headless REST\n"

    monkeypatch.setattr(module, "HEADLESS_REST_REFERENCE_PATH", reference)
    monkeypatch.setattr(module, "REPO_ROOT", tmp_path)
    monkeypatch.setattr(module, "gen_headless_rest_api", generate)

    assert module.main(["headless-rest"]) == 0
    assert reference.read_text(encoding="utf-8") == "generated headless REST\n"
    assert module.main(["headless-rest", "--check"]) == 0

    reference.write_text("stale headless REST\n", encoding="utf-8")
    assert module.main(["headless-rest", "--check"]) == 1
    assert reference.read_text(encoding="utf-8") == "stale headless REST\n"
    assert calls == ["headless-rest"] * 3


def test_tracked_public_contracts_include_the_program_client_boundary() -> None:
    composer = (
        _REPO_ROOT / "docs" / "reference" / "REST_API.md"
    ).read_text(encoding="utf-8")
    headless = (
        _REPO_ROOT / "docs" / "reference" / "HEADLESS_REST_API.md"
    ).read_text(encoding="utf-8")

    for contract in (composer, headless):
        assert "`GET` | `/api/program/runs`" in contract
        assert "`POST` | `/api/program/runs`" in contract
        assert (
            "`POST` | `/api/program/runs/{program_run_id}/stop`"
            in contract
        )
    assert "without owning their state" in composer
    assert "never interpreted as idle" in headless


def test_explicit_mcp_contract_write_and_check(monkeypatch, tmp_path) -> None:
    module = _load_generator_module()
    reference = tmp_path / "MCP_RUNTIME_CONTRACT.md"
    monkeypatch.setattr(module, "MCP_REFERENCE_PATH", reference)
    monkeypatch.setattr(module, "REPO_ROOT", tmp_path)
    monkeypatch.setattr(module, "gen_mcp_api", lambda: "generated MCP\n")
    assert module.main(["mcp"]) == 0
    assert module.main(["mcp", "--check"]) == 0
    reference.write_text("stale\n", encoding="utf-8")
    assert module.main(["mcp", "--check"]) == 1


def _write_recording_command(path: Path) -> None:
    path.write_text(
        "#!/usr/bin/env bash\n"
        "builtin printf '%s\\n' \"$*\" >>\"${PUBLIC_CONTRACT_CALLS}\"\n",
        encoding="utf-8",
    )
    path.chmod(0o755)


def test_historical_freshness_wrapper_cannot_reach_python_or_docker(
    tmp_path: Path,
) -> None:
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    _write_recording_command(fake_bin / "python3")
    _write_recording_command(fake_bin / "docker")
    calls = tmp_path / "external-calls"

    completed = subprocess.run(
        ["bash", str(_FRESHNESS_WRAPPER)],
        cwd=_REPO_ROOT,
        env={
            **os.environ,
            "PATH": f"{fake_bin}:{os.environ['PATH']}",
            "PUBLIC_CONTRACT_CALLS": str(calls),
        },
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 78
    assert _UNAVAILABLE_REASON_CODE in completed.stderr
    assert "structural-rest" in completed.stderr
    assert not calls.exists()


@pytest.mark.parametrize("selector", ("structural-rest", "headless-rest"))
@pytest.mark.parametrize("inside_container", (True, False))
def test_explicit_freshness_selector_routes_to_the_bounded_generator(
    inside_container: bool,
    selector: str,
    tmp_path: Path,
) -> None:
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    _write_recording_command(fake_bin / "python3")
    _write_recording_command(fake_bin / "docker")
    calls = tmp_path / "external-calls"
    environment = {
        **os.environ,
        "PATH": f"{fake_bin}:{os.environ['PATH']}",
        "PUBLIC_CONTRACT_CALLS": str(calls),
    }
    environment.pop("INSIDE_MANYFORGE_DEV_CONTAINER", None)
    if inside_container:
        environment["INSIDE_MANYFORGE_DEV_CONTAINER"] = "1"

    completed = subprocess.run(
        ["bash", str(_FRESHNESS_WRAPPER), selector],
        cwd=_REPO_ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 0, completed.stderr
    expected = (
        f"scripts/regen-public-contracts.py {selector} --check"
        if inside_container
        else "compose run --rm -e INSIDE_MANYFORGE_DEV_CONTAINER=1 dev "
        f"python3 scripts/regen-public-contracts.py {selector} --check"
    )
    assert calls.read_text(encoding="utf-8").splitlines() == [expected]



def test_removed_contact_release_clearance_field_stays_out_of_the_repository() -> None:
    # Moved from the retired release-gate removed-field hygiene check: the
    # field may survive only in tests that prove its rejection. The historical
    # baseline patches were retired.
    removed_field = "min_separation_" + "clearance_m"
    removed_bytes = removed_field.encode("ascii")
    listed = subprocess.run(
        ["git", "-C", str(_REPO_ROOT), "ls-files", "-z", "--cached", "--others",
         "--exclude-standard"],
        check=True,
        capture_output=True,
    ).stdout
    failures: list[str] = []
    for relative in sorted({p.decode() for p in listed.split(b"\0") if p}):
        path = _REPO_ROOT / relative
        if path.is_symlink() or not path.is_file():
            continue
        payload = path.read_bytes()
        if removed_bytes not in payload:
            continue
        try:
            text = payload.decode("utf-8")
        except UnicodeDecodeError:
            failures.append(f"{relative}: hygiene source is not valid UTF-8")
            continue
        parts = Path(relative).parts
        if ("tests" in parts or "test" in parts) and Path(relative).name.startswith(
            "test_"
        ):
            lowered = text.lower()
            if not any(
                word in lowered for word in ("unknown", "reject", "legacy", "baseline")
            ):
                failures.append(
                    f"{relative}: removed field is not rejection/baseline evidence"
                )
            continue
        failures.append(
            f"{relative}: removed field remains outside strict test/baseline evidence"
        )

    assert failures == []
