#!/usr/bin/env bash
# Build one exact RoboPlan source tree without modifying its build helper.

set -euo pipefail

if (( $# < 1 )); then
    echo "usage: build-roboplan-source-stack.sh SOURCE_ROOT [CMAKE_ARG ...]" >&2
    exit 2
fi

SOURCE_ROOT="$1"
shift
if [[ ! -d "$SOURCE_ROOT" ]]; then
    echo "RoboPlan source root does not exist: $SOURCE_ROOT" >&2
    exit 2
fi
SOURCE_ROOT="$(cd "$SOURCE_ROOT" && pwd -P)"

SOURCE_LEDGER="${ROBOPLAN_SOURCE_LEDGER:-}"
SOURCE_ID="${ROBOPLAN_SOURCE_ID:-}"
SOURCE_VERIFIER="${ROBOPLAN_SOURCE_VERIFIER:-$(dirname "$0")/verify-external-source-closure.py}"
if [[ -n "$SOURCE_LEDGER" || -n "$SOURCE_ID" ]]; then
    if [[ -z "$SOURCE_LEDGER" || -z "$SOURCE_ID" || ! -f "$SOURCE_VERIFIER" ]]; then
        echo "exact source verification requires ledger, source id, and verifier" >&2
        exit 2
    fi
    verifier_args=(
        --ledger "$SOURCE_LEDGER"
        --source-id "$SOURCE_ID"
        --root "$SOURCE_ROOT"
        --content-kind build
    )
    if [[ "${ROBOPLAN_ALLOW_STRIPPED_GIT:-0}" == "1" ]]; then
        verifier_args+=(--allow-stripped-git)
    elif [[ "${ROBOPLAN_ALLOW_STRIPPED_GIT:-0}" != "0" ]]; then
        echo "ROBOPLAN_ALLOW_STRIPPED_GIT must be 0 or 1" >&2
        exit 2
    fi
    python3 "$SOURCE_VERIFIER" "${verifier_args[@]}"
else
    printf 'RoboPlan builder is running in generic non-qualification mode\n' >&2
fi

BUILD_ROOT="${ROBOPLAN_BUILD_ROOT:-${SOURCE_ROOT}/build}"
INSTALL_ROOT="${ROBOPLAN_INSTALL_ROOT:-${SOURCE_ROOT}/install}"
BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"
if [[ ! "$BUILD_PARALLEL_LEVEL" =~ ^[1-9][0-9]*$ ]]; then
    echo "CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer" >&2
    exit 2
fi

PROJECTS=(
    roboplan_example_models
    roboplan
    roboplan_simple_ik
    roboplan_oink
    roboplan_toppra
    roboplan_rrt
    roboplan_cartesian_planning
    roboplan_examples
)

mkdir -p "$BUILD_ROOT" "$INSTALL_ROOT"

prepend_path() {
    local value="$1"
    local current="$2"
    if [[ -n "$current" ]]; then
        printf '%s:%s' "$value" "$current"
    else
        printf '%s' "$value"
    fi
}

for project in "${PROJECTS[@]}"; do
    source_dir="${SOURCE_ROOT}/${project}"
    build_dir="${BUILD_ROOT}/${project}"
    install_dir="${INSTALL_ROOT}/${project}"
    if [[ ! -f "${source_dir}/CMakeLists.txt" ]]; then
        echo "RoboPlan source package is missing: ${project}" >&2
        exit 1
    fi

    # Put the caller-owned cache arguments first and the exact source, build,
    # and install locations last. CMake resolves repeated options from the
    # right, so a caller cannot accidentally redirect one package into an
    # ambient or shared generation through a repeated path option.
    cmake "$@" \
        -S "$source_dir" \
        -B "$build_dir" \
        -DCMAKE_INSTALL_PREFIX="$install_dir"
    # RoboPlan's nanobind stub targets import the extension during the build.
    # Put this package's just-built shared libraries ahead of any ambient or
    # previously selected RoboPlan generation so stub generation cannot load a
    # same-soname library from the base image.
    project_build_libraries="${build_dir}:${build_dir}/lib"
    build_ld_library_path="$(prepend_path "$project_build_libraries" "${LD_LIBRARY_PATH:-}")"
    LD_LIBRARY_PATH="$build_ld_library_path" \
        cmake --build "$build_dir" --parallel "$BUILD_PARALLEL_LEVEL"
    cmake --install "$build_dir"

    CMAKE_PREFIX_PATH="$(prepend_path "$install_dir" "${CMAKE_PREFIX_PATH:-}")"
    LD_LIBRARY_PATH="$(prepend_path "${install_dir}/lib" "${LD_LIBRARY_PATH:-}")"
    export CMAKE_PREFIX_PATH LD_LIBRARY_PATH
done

printf 'RoboPlan source stack built from %s\n' "$SOURCE_ROOT"
