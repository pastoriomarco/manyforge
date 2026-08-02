#!/usr/bin/env bash
# Composable Cell I2 packet-owned RoboPlan sanitizer evidence lane.
# Strict sanitizer lane for the patched RoboPlan Scene/CollisionContext
# generation contract. Run inside the native repository container.

set -euo pipefail
export GIT_OPTIONAL_LOCKS=0

if [[ $# -ne 1 || ( "$1" != "address" && "$1" != "thread" ) ]]; then
  echo "usage: $0 <address|thread>" >&2
  exit 2
fi

SANITIZER="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
ROBOPLAN_SOURCE="${ROBOPLAN_SOURCE:-${REPO_ROOT}/.docker/roboplan}"
MANIFEST_DIR="${REPO_ROOT}/third_party/roboplan"
BUILD_ROOT="${BUILD_ROOT:-/qualification-output/roboplan-scene-${SANITIZER}}"
PARALLEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
ARTIFACT_ROOT="${MANYFORGE_NATIVE_ARTIFACT_ROOT:-${BUILD_ROOT}}"
INVENTORY_JSON="${MANYFORGE_NATIVE_INVENTORY_JSON:-${BUILD_ROOT}/ctest-inventory.json}"
EXECUTION_INDEX_JSON="${MANYFORGE_NATIVE_EXECUTION_INDEX_JSON:-${BUILD_ROOT}/execution-index.json}"
RESULT_ROOT="${MANYFORGE_NATIVE_RESULT_ROOT:-${BUILD_ROOT}/native}"

if [[ "${SANITIZER}" == "thread" ]]; then
  expected_personality="00040000"
  current_personality="$(cat /proc/self/personality)"
  if [[ "${current_personality}" != "${expected_personality}" ]]; then
    if [[ "${MANYFORGE_TSAN_SETARCH_REEXEC:-0}" == "1" ]]; then
      echo "TSan ADDR_NO_RANDOMIZE re-exec did not take effect" >&2
      exit 2
    fi
    export MANYFORGE_TSAN_SETARCH_REEXEC=1
    exec /usr/bin/setarch x86_64 -R "$0" "$@"
  fi
  unset MANYFORGE_TSAN_SETARCH_REEXEC
  child_personality="$(sh -c 'cat /proc/self/personality')"
  if [[ "${child_personality}" != "${expected_personality}" ]]; then
    echo "TSan ADDR_NO_RANDOMIZE inheritance check failed" >&2
    exit 2
  fi
fi

expected_commit="$(
  sed -n 's/^- Commit: `\([0-9a-f]\{40\}\)`$/\1/p' \
    "${MANIFEST_DIR}/README.md" | head -n 1
)"
if [[ -z "${expected_commit}" ]]; then
  echo "RoboPlan manifest does not contain one pinned commit" >&2
  exit 2
fi
if [[ ! -d "${ROBOPLAN_SOURCE}/.git" ]]; then
  echo "RoboPlan source checkout is missing: ${ROBOPLAN_SOURCE}" >&2
  exit 2
fi
actual_commit="$(/usr/bin/git -C "${ROBOPLAN_SOURCE}" rev-parse HEAD)"
source_status="$(/usr/bin/git -C "${ROBOPLAN_SOURCE}" status --porcelain=v1 \
  --untracked-files=all --ignore-submodules=none)"
if [[ "${actual_commit}" != "${expected_commit}" || -n "${source_status}" ]]; then
  echo "RoboPlan source must be the exact clean pinned generation" >&2
  echo "expected=${expected_commit} actual=${actual_commit}" >&2
  exit 2
fi

(cd "${MANIFEST_DIR}" && sha256sum -c patches.sha256)

case "${BUILD_ROOT}" in
  /qualification-output/*) ;;
  *) echo "BUILD_ROOT must be a child of /qualification-output" >&2; exit 2 ;;
esac
if [[ -e "${BUILD_ROOT}" ]]; then
  echo "RoboPlan sanitizer build root must be fresh: ${BUILD_ROOT}" >&2
  exit 2
fi
mkdir -p "${BUILD_ROOT}"
/usr/bin/git clone --quiet --no-local --no-checkout \
  "${ROBOPLAN_SOURCE}" "${BUILD_ROOT}/source"
/usr/bin/git -C "${BUILD_ROOT}/source" checkout --quiet --detach \
  "${expected_commit}"
while read -r _digest patch; do
  case "${patch}" in
    patches/*.patch) ;;
    *) echo "invalid RoboPlan patch path: ${patch}" >&2; exit 2 ;;
  esac
  /usr/bin/git -C "${BUILD_ROOT}/source" apply --check \
    "${MANIFEST_DIR}/${patch}"
  /usr/bin/git -C "${BUILD_ROOT}/source" apply "${MANIFEST_DIR}/${patch}"
done < "${MANIFEST_DIR}/patches.sha256"
/usr/bin/git -C "${BUILD_ROOT}/source" add -A
if ! /usr/bin/git -C "${BUILD_ROOT}/source" diff --quiet \
    || [[ -n "$(/usr/bin/git -C "${BUILD_ROOT}/source" ls-files \
      --others --exclude-standard)" ]]; then
  echo "patched RoboPlan source has unstaged or untracked inputs" >&2
  exit 2
fi
expected_patched_tree="687405b677d50965c9ce5701016cb67c4579c563"
patched_tree="$(/usr/bin/git -C "${BUILD_ROOT}/source" write-tree)"
if [[ "${patched_tree}" != "${expected_patched_tree}" ]]; then
  echo "patched RoboPlan index tree differs from the frozen patch result" >&2
  echo "expected=${expected_patched_tree} actual=${patched_tree}" >&2
  exit 2
fi
verify_patched_source_generation() {
  local mode="$1"
  /usr/local/bin/python3 - "${BUILD_ROOT}/source" \
    "${BUILD_ROOT}/patched-source-generation-pre.json" "${mode}" <<'PY'
import hashlib
import json
import os
import pathlib
import stat
import subprocess
import sys

source = pathlib.Path(sys.argv[1])
destination = pathlib.Path(sys.argv[2])
mode = sys.argv[3]
if mode not in {"create", "compare"}:
    raise SystemExit("unknown patched source-generation verification mode")


def git_bytes(*arguments: str) -> bytes:
    completed = subprocess.run(
        [
            "/usr/bin/git",
            "-c",
            "color.ui=false",
            "-c",
            "diff.algorithm=myers",
            "-c",
            "diff.renames=false",
            "-c",
            "core.fileMode=true",
            "-c",
            "core.fsmonitor=false",
            "-c",
            "core.autocrlf=false",
            "-C",
            str(source),
            f"--work-tree={source}",
            *arguments,
        ],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return completed.stdout


def git_text(*arguments: str) -> str:
    return git_bytes(*arguments).decode("utf-8", errors="strict").strip()


staged_diff = git_bytes(
    "diff",
    "--cached",
    "--binary",
    "--full-index",
    "--no-color",
    "--no-ext-diff",
    "--no-textconv",
    "--no-renames",
    "--src-prefix=a/",
    "--dst-prefix=b/",
)
staged_paths = git_bytes(
    "diff", "--cached", "--name-only", "-z", "--no-renames"
)
ambient = {
    "unstaged_path_count": git_bytes(
        "diff",
        "--binary",
        "--full-index",
        "--no-color",
        "--no-ext-diff",
        "--no-textconv",
        "--no-renames",
        "--src-prefix=a/",
        "--dst-prefix=b/",
    ),
    "untracked_path_count": git_bytes("ls-files", "--others", "-z"),
    "unmerged_path_count": git_bytes("ls-files", "--unmerged", "-z"),
    "submodule_path_count": git_bytes("submodule", "status", "--recursive"),
}
if any(ambient.values()):
    raise SystemExit("patched RoboPlan generation has ambient source state")
for flag, description in (
    ("-v", "assume-unchanged or skip-worktree"),
    ("-f", "fsmonitor-clean"),
):
    tracked_flags = git_bytes("ls-files", flag, "-z")
    if any(
        record and not record.startswith(b"H ")
        for record in tracked_flags.split(b"\0")
    ):
        raise SystemExit(
            f"patched RoboPlan generation uses hidden {description} flags"
        )

if git_text("rev-parse", "--show-object-format") != "sha1":
    raise SystemExit("patched RoboPlan generation is not SHA-1 Git")
projection = []
for record in git_bytes("ls-files", "--stage", "-z").split(b"\0"):
    if not record:
        continue
    metadata, raw_path = record.split(b"\t", 1)
    raw_mode, raw_blob, raw_stage = metadata.split(b" ", 2)
    tracked_mode = raw_mode.decode("ascii", errors="strict")
    expected_blob = raw_blob.decode("ascii", errors="strict")
    if raw_stage != b"0":
        raise SystemExit("patched RoboPlan index contains an unmerged entry")
    relative = raw_path.decode("utf-8", errors="strict")
    pure = pathlib.PurePosixPath(relative)
    if (
        pure.is_absolute()
        or any(part in {"", ".", ".."} for part in pure.parts)
    ):
        raise SystemExit("patched RoboPlan index contains a malformed path")
    current = source
    for part in pure.parts[:-1]:
        current = current / part
        metadata = current.lstat()
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
            raise SystemExit(f"tracked path has a non-directory parent: {relative}")
    path = source.joinpath(*pure.parts)
    metadata = path.lstat()
    if tracked_mode in {"100644", "100755"}:
        if (
            not stat.S_ISREG(metadata.st_mode)
            or metadata.st_nlink != 1
            or bool(metadata.st_mode & stat.S_IXUSR)
            != (tracked_mode == "100755")
        ):
            raise SystemExit(f"tracked file mode drifted: {relative}")
        digest = hashlib.sha1()
        digest.update(f"blob {metadata.st_size}\0".encode("ascii"))
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
        actual_blob = digest.hexdigest()
    elif tracked_mode == "120000":
        if not stat.S_ISLNK(metadata.st_mode):
            raise SystemExit(f"tracked symlink mode drifted: {relative}")
        raw_target = os.fsencode(os.readlink(path))
        actual_blob = hashlib.sha1(
            f"blob {len(raw_target)}\0".encode("ascii") + raw_target
        ).hexdigest()
    else:
        raise SystemExit(f"unsupported tracked Gitlink: {relative}")
    if actual_blob != expected_blob:
        raise SystemExit(f"tracked bytes differ from index: {relative}")
    projection.append(
        {"path": relative, "mode": tracked_mode, "blob": expected_blob}
    )
if not projection:
    raise SystemExit("patched RoboPlan index is empty")


def canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")

evidence = {
    "schema_version": "manyforge.composable_cell_i2.patched_source_generation.v1",
    "base_commit": git_text("rev-parse", "HEAD"),
    "base_tree": git_text("rev-parse", "HEAD^{tree}"),
    "patched_index_tree": git_text("write-tree"),
    "staged_diff_sha256": hashlib.sha256(staged_diff).hexdigest(),
    "staged_path_count": len(
        [path for path in staged_paths.split(b"\0") if path]
    ),
    "staged_paths_sha256": hashlib.sha256(staged_paths).hexdigest(),
    **{key: 0 for key in ambient},
    "allowed_untracked_paths": [],
    "undeclared_untracked_path_count": 0,
    "tracked_path_count": len(projection),
    "tracked_index_projection_sha256": hashlib.sha256(
        canonical_bytes(projection)
    ).hexdigest(),
}
if mode == "create":
    descriptor = os.open(
        destination,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
        0o600,
    )
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(canonical_bytes(evidence) + b"\n")
else:
    pairs = json.loads(
        destination.read_text(encoding="utf-8"),
        object_pairs_hook=lambda rows: (
            dict(rows)
            if len(rows) == len(dict(rows))
            else (_ for _ in ()).throw(ValueError("duplicate JSON member"))
        ),
    )
    if pairs != evidence:
        raise SystemExit(
            "patched RoboPlan generation changed after prebuild capture"
        )
PY
}
verify_patched_source_generation create

case "${SANITIZER}" in
  address)
    # Pinocchio is supplied as a prebuilt, non-ASan shared library. Eigen
    # otherwise changes from the system allocator to its handmade aligned
    # allocator when __SANITIZE_ADDRESS__ is defined, making inline destructors
    # ABI-incompatible with allocations performed inside that library. Keep the
    # allocator contract identical across the DSO boundary; ASan still
    # intercepts and validates the matching malloc/free calls.
    sanitizer_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -DEIGEN_MALLOC_ALREADY_ALIGNED=1"
    export ASAN_OPTIONS="halt_on_error=1:detect_leaks=1:exitcode=66"
    export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:exitcode=66"
    unset TSAN_OPTIONS 2>/dev/null || true
    native_lane_id="${MANYFORGE_NATIVE_LANE_ID:-roboplan_asan_ubsan}"
    native_sanitizer="address+undefined"
    ;;
  thread)
    sanitizer_flags="-fsanitize=thread -fno-omit-frame-pointer"
    tsan_suppressions="${TSAN_SUPPRESSIONS:-${REPO_ROOT}/scripts/ci/tsan.supp}"
    if [[ ! -f "${tsan_suppressions}" ]]; then
      echo "TSan suppression file not found: ${tsan_suppressions}" >&2
      exit 2
    fi
    export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 history_size=7 exitcode=66 suppressions=${tsan_suppressions}"
    unset ASAN_OPTIONS UBSAN_OPTIONS 2>/dev/null || true
    native_lane_id="${MANYFORGE_NATIVE_LANE_ID:-roboplan_tsan}"
    native_sanitizer="thread"
    ;;
esac

cmeel_prefix="$(/usr/local/bin/python3 - <<'PY'
import pathlib
import site

for root in map(pathlib.Path, site.getsitepackages()):
    candidate = root / "cmeel.prefix"
    if (candidate / "lib/cmake/pinocchio/pinocchioConfig.cmake").exists():
        print(candidate)
        raise SystemExit(0)
raise SystemExit(1)
PY
)"
if [[ "${cmeel_prefix}" \
      != "/usr/local/lib/python3.12/site-packages/cmeel.prefix" ]]; then
  echo "cmeel.prefix resolved outside the frozen image location" >&2
  exit 2
fi

common_cmake_args=(
  -DBUILD_TESTING=OFF
  -DBUILD_PYTHON_BINDINGS=OFF
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DCMAKE_CXX_EXTENSIONS=OFF
  -DCMAKE_SKIP_RPATH=ON
  "-DCMAKE_CXX_FLAGS=${sanitizer_flags}"
  "-DCMAKE_EXE_LINKER_FLAGS=${sanitizer_flags}"
  "-DCMAKE_SHARED_LINKER_FLAGS=${sanitizer_flags}"
)

write_graphviz_options() {
  local build_dir="$1"
  printf '%s\n' \
    'set(GRAPHVIZ_GRAPH_NAME "manyforge_i2")' \
    'set(GRAPHVIZ_GRAPH_HEADER "node [ fontsize = \"12\" ];")' \
    'set(GRAPHVIZ_NODE_PREFIX "i2node")' \
    'set(GRAPHVIZ_EXECUTABLES TRUE)' \
    'set(GRAPHVIZ_STATIC_LIBS TRUE)' \
    'set(GRAPHVIZ_SHARED_LIBS TRUE)' \
    'set(GRAPHVIZ_MODULE_LIBS TRUE)' \
    'set(GRAPHVIZ_INTERFACE_LIBS TRUE)' \
    'set(GRAPHVIZ_OBJECT_LIBS TRUE)' \
    'set(GRAPHVIZ_UNKNOWN_LIBS TRUE)' \
    'set(GRAPHVIZ_EXTERNAL_LIBS TRUE)' \
    'set(GRAPHVIZ_CUSTOM_TARGETS TRUE)' \
    'set(GRAPHVIZ_IGNORE_TARGETS "")' \
    'set(GRAPHVIZ_GENERATE_PER_TARGET FALSE)' \
    'set(GRAPHVIZ_GENERATE_DEPENDERS FALSE)' \
    > "${build_dir}/CMakeGraphVizOptions.cmake"
}

install_project() {
  local project="$1"
  local install_prefix="${BUILD_ROOT}/install/${project}"
  mkdir -p "${BUILD_ROOT}/build/${project}/.cmake/api/v1/query"
  printf '{}\n' > \
    "${BUILD_ROOT}/build/${project}/.cmake/api/v1/query/codemodel-v2"
  printf '{}\n' > \
    "${BUILD_ROOT}/build/${project}/.cmake/api/v1/query/cmakeFiles-v1"
  write_graphviz_options "${BUILD_ROOT}/build/${project}"
  /usr/bin/cmake -S "${BUILD_ROOT}/source/${project}" \
    -G Ninja \
    --graphviz="${BUILD_ROOT}/build/${project}/manyforge-i2-target-graph.dot" \
    -B "${BUILD_ROOT}/build/${project}" \
    -DCMAKE_INSTALL_PREFIX="${install_prefix}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    "${common_cmake_args[@]}"
  /usr/bin/ninja -C "${BUILD_ROOT}/build/${project}" \
    -j "${PARALLEL}" -d keeprsp
  /usr/bin/cmake --install "${BUILD_ROOT}/build/${project}"
  export CMAKE_PREFIX_PATH="${install_prefix}:${CMAKE_PREFIX_PATH}"
  export LD_LIBRARY_PATH="${install_prefix}/lib:${LD_LIBRARY_PATH}"
}

export CMAKE_PREFIX_PATH="${cmeel_prefix}:/usr/local"
export LD_LIBRARY_PATH="${cmeel_prefix}/lib:/usr/local/lib"
install_project roboplan_example_models
install_project roboplan

manyforge_build="${BUILD_ROOT}/build/manyforge_roboplan"
mkdir -p "${manyforge_build}/.cmake/api/v1/query"
printf '{}\n' > "${manyforge_build}/.cmake/api/v1/query/codemodel-v2"
printf '{}\n' > "${manyforge_build}/.cmake/api/v1/query/cmakeFiles-v1"
write_graphviz_options "${manyforge_build}"
/usr/bin/cmake -G Ninja -S "${REPO_ROOT}/manyforge_roboplan" -B "${manyforge_build}" \
  --graphviz="${manyforge_build}/manyforge-i2-target-graph.dot" \
  -DBUILD_TESTING=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  "-DCMAKE_CXX_FLAGS=${sanitizer_flags}" \
  "-DCMAKE_EXE_LINKER_FLAGS=${sanitizer_flags}" \
  "-DCMAKE_SHARED_LINKER_FLAGS=${sanitizer_flags}"
/usr/bin/ninja -C "${manyforge_build}" -j "${PARALLEL}" -d keeprsp \
  test_roboplan_scene_generation
/usr/bin/env -u CMAKE_PREFIX_PATH -u LD_LIBRARY_PATH \
  /usr/local/bin/python3 "${SCRIPT_DIR}/run-composable-cell-i2-native-evidence.py" \
  --lane-id "${native_lane_id}" \
  --sanitizer "${native_sanitizer}" \
  --artifact-root "${ARTIFACT_ROOT}" \
  --build-dir "${manyforge_build}" \
  --inventory-json "${INVENTORY_JSON}" \
  --execution-index-json "${EXECUTION_INDEX_JSON}" \
  --result-root "${RESULT_ROOT}" \
  --test-regex '^RoboPlanSceneGenerationTest\.' \
  --timeout-seconds 300 \
  --require-zero-disabled \
  --require-zero-skips \
  --forbid-fallback

if [[ "$(/usr/bin/git -C "${ROBOPLAN_SOURCE}" rev-parse HEAD)" != "${expected_commit}" \
      || -n "$(/usr/bin/git -C "${ROBOPLAN_SOURCE}" status --porcelain=v1 \
        --untracked-files=all --ignore-submodules=none)" ]]; then
  echo "sanitizer lane modified the pinned RoboPlan checkout" >&2
  exit 2
fi
verify_patched_source_generation compare
