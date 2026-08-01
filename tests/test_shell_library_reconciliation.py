from __future__ import annotations

import os
import re
import shlex
import subprocess
from pathlib import Path


_REPO_ROOT = Path(__file__).resolve().parents[1]
_LIB = _REPO_ROOT / "scripts" / "lib"


def _bash(script: str, *, timeout: float = 10.0) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", "-c", script],
        cwd=_REPO_ROOT,
        env=os.environ.copy(),
        capture_output=True,
        text=True,
        check=False,
        timeout=timeout,
    )


def test_deleted_runtime_owner_libraries_are_not_dormant() -> None:
    assert not (_LIB / "preflight.sh").exists()
    assert not (_LIB / "vllm-status-watch.sh").exists()


def test_runner_retains_bounded_presentation_without_shell_execution(tmp_path: Path) -> None:
    runner = _LIB / "runner.sh"
    source = runner.read_text(encoding="utf-8")
    marker = tmp_path / "executed"
    literal = f"$(touch {shlex.quote(str(marker))})"
    completed = _bash(
        f"""
source {shlex.quote(str(runner))}
declare -F run >/dev/null && exit 20
declare -F run_capture >/dev/null && exit 21
declare -F install_error_trap >/dev/null && exit 22
info {shlex.quote(literal)}
oversize="$(printf '%05000d' 0)"
info "${{oversize}}" && exit 23
exit 0
"""
    )

    assert completed.returncode == 0, completed.stderr
    assert literal in completed.stderr
    assert not marker.exists()
    assert "d31708e592aa2cece604ccfca9bd37e7b64e2ac8" in source
    assert "bash -c" not in source
    assert "|| true" not in source


def test_prompts_bound_indirection_values_and_option_cardinality() -> None:
    prompts = _LIB / "prompts.sh"
    completed = _bash(
        f"""
source {shlex.quote(str(prompts))}
export MANYFORGE_NON_INTERACTIVE=1
export SELECTED_VALUE=second
[[ "$(prompt_or_default 'Choice?' SELECTED_VALUE first)" == second ]] || exit 30
[[ "$(pick_from_list 'Menu' 2 first second)" == second ]] || exit 31
prompt_or_default 'Choice?' 'BAD;NAME' first && exit 32
long_value="$(printf '%03000d' 0)"
export SELECTED_VALUE="${{long_value}}"
prompt_or_default 'Choice?' SELECTED_VALUE first && exit 33
options=()
for ((index = 0; index < 65; index++)); do options+=("option-${{index}}"); done
pick_from_list 'Menu' 1 "${{options[@]}}" && exit 34
exit 0
"""
    )

    assert completed.returncode == 0, completed.stderr
    assert "invalid environment variable name" in completed.stderr
    assert "requires 1..64 options" in completed.stderr


def test_wait_timeout_terminates_ordinary_callback_child_group(tmp_path: Path) -> None:
    wait_helper = _LIB / "wait.sh"
    child_pid_file = tmp_path / "child.pid"
    completed = _bash(
        f"""
source {shlex.quote(str(wait_helper))}
probe_with_child() {{
    sleep 60 &
    child=$!
    printf '%s\n' "${{child}}" > {shlex.quote(str(child_pid_file))}
    wait "${{child}}"
}}
set +e
wait_with_progress 'bounded probe' 1 '' '' probe_with_child
status=$?
set -e
[[ "${{status}}" == 1 ]] || exit 40
[[ -s {shlex.quote(str(child_pid_file))} ]] || exit 41
child="$(<{shlex.quote(str(child_pid_file))})"
[[ "${{child}}" =~ ^[1-9][0-9]*$ ]] || exit 42
for ((attempt = 0; attempt < 40; attempt++)); do
    if ! kill -0 "${{child}}" 2>/dev/null; then exit 0; fi
    # A callback child killed while pytest is container PID 1 can remain a
    # zombie until an init process reaps it. It is terminated even though
    # kill -0 still succeeds, so distinguish that state from a live escape.
    state="$(ps -o stat= -p "${{child}}" 2>/dev/null || true)"
    if [[ "${{state}}" == Z* ]]; then exit 0; fi
    sleep 0.05
done
exit 43
""",
        timeout=8.0,
    )

    assert completed.returncode == 0, completed.stderr
    assert "timed out" in completed.stderr


def test_wait_fast_probe_and_output_ceiling_are_bounded() -> None:
    wait_helper = _LIB / "wait.sh"
    fast = _bash(
        f"""
source {shlex.quote(str(wait_helper))}
ready_now() {{ return 0; }}
wait_with_progress 'fast probe' 2 '' '' ready_now
"""
    )
    assert fast.returncode == 0, fast.stderr
    assert "ready after" in fast.stderr

    noisy = _bash(
        f"""
source {shlex.quote(str(wait_helper))}
noisy_probe() {{ while true; do printf '%01024d' 0; done; }}
set +e
wait_with_progress 'noisy probe' 1 '' '' noisy_probe
status=$?
set -e
[[ "${{status}}" == 1 ]]
""",
        timeout=8.0,
    )
    assert noisy.returncode == 0, noisy.stderr
    assert "timed out" in noisy.stderr


def test_frontend_shell_parses_fixed_records_and_attests_snapshot() -> None:
    frontend_path = _LIB / "frontend.sh"
    frontend = frontend_path.read_text(encoding="utf-8")

    assert "2cf625416bcd6db0526a091784f8f2edc780e845" in frontend
    assert "c9855c86150de04349e9ac33a9200b9b18bd0898" in frontend
    assert 'eval "${hash_env}"' not in frontend
    assert 'eval "exec ${lock_fd}>&-"' not in frontend
    # Keep this deliberately conservative: an eval token after a separator,
    # assignment, or compound-command keyword is just as executable as one at
    # the start of a line.
    assert re.search(r"\beval\b", frontend) is None
    assert "MANYFORGE_FRONTEND_HASH_SCRIPT" not in frontend
    assert "--output-format nul" in frontend
    assert "snapshot_record" in frontend
    assert "MANYFORGE_FRONTEND_SOURCE_MODE=\"worktree\"" not in frontend

    completed = _bash(
        f"""
cache_root="$(mktemp -d)"
trap 'rm -rf -- "${{cache_root}}"' EXIT
source {shlex.quote(str(frontend_path))}
declare -A record=()
hash_identity="$(sha256sum -- {shlex.quote(str(_LIB / 'frontend_hash.py'))})"
hash_identity="${{hash_identity%% *}}"
_manyforge_frontend_load_record \
    {shlex.quote(str(_LIB / 'frontend_hash.py'))} source \
    {shlex.quote(str(_REPO_ROOT / 'manyforge_composer' / 'frontend'))} \
    "${{cache_root}}" record '' "${{hash_identity}}"
[[ "${{record[MANYFORGE_FRONTEND_SOURCE_MODE]}}" == git-head ]]
[[ "${{record[MANYFORGE_FRONTEND_SOURCE_HASH]}}" =~ ^[0-9a-f]{{64}}$ ]]
[[ "${{record[MANYFORGE_FRONTEND_SNAPSHOT_HASH]}}" =~ ^[0-9a-f]{{64}}$ ]]
"""
    )
    assert completed.returncode == 0, completed.stderr


def test_frontend_shell_bounds_record_sink_and_cleans_rejected_output(
    tmp_path: Path,
) -> None:
    frontend_path = _LIB / "frontend.sh"
    oversized_helper = tmp_path / "oversized_frontend_hash.py"
    oversized_helper.write_text(
        """from __future__ import annotations

import os
import sys
from pathlib import Path

marker = Path(sys.argv[-1])
try:
    for _ in range(32):
        os.write(1, b"x" * (64 * 1024))
except BrokenPipeError:
    marker.write_text("bounded", encoding="utf-8")
else:
    marker.write_text("unbounded", encoding="utf-8")
""",
        encoding="utf-8",
    )
    cache_root = tmp_path / "cache"
    cache_root.mkdir()
    marker = tmp_path / "producer-status"

    completed = _bash(
        f"""
source {shlex.quote(str(frontend_path))}
declare -A record=()
hash_identity="$(sha256sum -- {shlex.quote(str(oversized_helper))})"
hash_identity="${{hash_identity%% *}}"
set +e
_manyforge_frontend_load_record \
    {shlex.quote(str(oversized_helper))} source \
    {shlex.quote(str(marker))} \
    {shlex.quote(str(cache_root))} record '' "${{hash_identity}}"
status=$?
set -e
[[ "${{status}}" != 0 ]]
"""
    )

    assert completed.returncode == 0, completed.stderr
    assert marker.read_text(encoding="utf-8") == "bounded"
    assert list(cache_root.iterdir()) == []


def test_frontend_shell_rejects_symlink_build_root_before_build(tmp_path: Path) -> None:
    frontend_path = _LIB / "frontend.sh"
    real_root = tmp_path / "real-build-root"
    real_root.mkdir()
    linked_root = tmp_path / "linked-build-root"
    linked_root.symlink_to(real_root, target_is_directory=True)
    completed = _bash(
        f"""
source {shlex.quote(str(frontend_path))}
manyforge_build_frontend_dist \
    {shlex.quote(str(_REPO_ROOT / 'manyforge_composer' / 'frontend'))} \
    {shlex.quote(str(linked_root))}
"""
    )
    assert completed.returncode != 0
    assert "non-symlink directory" in completed.stderr


def test_compose_has_only_current_resource_output_global() -> None:
    compose = (_LIB / "compose.sh").read_text(encoding="utf-8")

    assert "declare -a COMPOSE_CMD" not in compose
    assert 'NATIVE_COMPOSER_IMAGE_ID=""' not in compose
    assert 'NATIVE_COMPOSER_BUILD_CACHE_VOLUME=""' not in compose
    assert "declare -a NATIVE_COMPOSER_RT_DOCKER_ARGS" in compose
