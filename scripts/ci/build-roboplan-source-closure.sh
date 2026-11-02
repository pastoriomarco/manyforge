#!/usr/bin/env bash
# Build exact ProxSuite, TOPPRA, and RoboPlan sources without network fallback.
#
# ProxSuite is configured with the exact jrl-cmakemodules source and without
# serialization, so the closure needs no cereal. RoboPlan takes nanoflann from
# its exact source and uses its bundled tl::expected header; TinyXML2 is the
# caller's private static build passed in the RoboPlan CMake arguments.

set -euo pipefail

if (( $# < 5 )); then
    echo "usage: build-roboplan-source-closure.sh PROXSUITE_SOURCE JRL_CMAKEMODULES_SOURCE TOPPRA_SOURCE NANOFLANN_SOURCE ROBOPLAN_SOURCE [ROBOPLAN_CMAKE_ARG ...]" >&2
    exit 2
fi

PROXSUITE_SOURCE="$1"
JRL_CMAKEMODULES_SOURCE="$2"
TOPPRA_SOURCE="$3"
NANOFLANN_SOURCE="$4"
ROBOPLAN_SOURCE="$5"
shift 5

canonical_source() {
    local label="$1"
    local path="$2"
    if [[ ! -d "$path" ]]; then
        echo "${label} source root does not exist: ${path}" >&2
        exit 2
    fi
    (cd "$path" && pwd -P)
}

PROXSUITE_SOURCE="$(canonical_source ProxSuite "$PROXSUITE_SOURCE")"
JRL_CMAKEMODULES_SOURCE="$(canonical_source jrl-cmakemodules "$JRL_CMAKEMODULES_SOURCE")"
TOPPRA_SOURCE="$(canonical_source TOPPRA "$TOPPRA_SOURCE")"
NANOFLANN_SOURCE="$(canonical_source nanoflann "$NANOFLANN_SOURCE")"
ROBOPLAN_SOURCE="$(canonical_source RoboPlan "$ROBOPLAN_SOURCE")"

SOURCE_LEDGER="${ROBOPLAN_SOURCE_LEDGER:-}"
SOURCE_VERIFIER="${ROBOPLAN_SOURCE_VERIFIER:-$(dirname "$0")/verify-external-source-closure.py}"
if [[ -z "$SOURCE_LEDGER" || ! -f "$SOURCE_LEDGER" || ! -f "$SOURCE_VERIFIER" ]]; then
    echo "exact closure build requires ROBOPLAN_SOURCE_LEDGER and its verifier" >&2
    exit 2
fi
ALLOW_DEVELOPMENT_SOURCE="${ROBOPLAN_ALLOW_DEVELOPMENT_SOURCE:-0}"
ALLOW_STRIPPED_GIT="${ROBOPLAN_ALLOW_STRIPPED_GIT:-0}"
if [[ "$ALLOW_DEVELOPMENT_SOURCE" != "0" && "$ALLOW_DEVELOPMENT_SOURCE" != "1" ]]; then
    echo "ROBOPLAN_ALLOW_DEVELOPMENT_SOURCE must be 0 or 1" >&2
    exit 2
fi
if [[ "$ALLOW_STRIPPED_GIT" != "0" && "$ALLOW_STRIPPED_GIT" != "1" ]]; then
    echo "ROBOPLAN_ALLOW_STRIPPED_GIT must be 0 or 1" >&2
    exit 2
fi
PROXSUITE_SOURCE_ID="${PROXSUITE_SOURCE_ID:-proxsuite_0_8_2_dependency}"
JRL_CMAKEMODULES_SOURCE_ID="${JRL_CMAKEMODULES_SOURCE_ID:-jrl_cmakemodules_2_3_0_dependency}"
TOPPRA_SOURCE_ID="${TOPPRA_SOURCE_ID:-toppra_0_6_10_ua1_dependency}"
NANOFLANN_SOURCE_ID="${NANOFLANN_SOURCE_ID:-nanoflann_2_0_1_dependency}"
ROBOPLAN_SOURCE_ID="${ROBOPLAN_SOURCE_ID:-roboplan_0_8_0_foundation}"
verifier_git_args=()
if [[ "$ALLOW_STRIPPED_GIT" == "1" ]]; then
    verifier_git_args+=(--allow-stripped-git)
fi
python3 "$SOURCE_VERIFIER" --ledger "$SOURCE_LEDGER" \
    --source-id "$PROXSUITE_SOURCE_ID" --root "$PROXSUITE_SOURCE" \
    --content-kind build "${verifier_git_args[@]}"
python3 "$SOURCE_VERIFIER" --ledger "$SOURCE_LEDGER" \
    --source-id "$JRL_CMAKEMODULES_SOURCE_ID" --root "$JRL_CMAKEMODULES_SOURCE" \
    --content-kind build "${verifier_git_args[@]}"
python3 "$SOURCE_VERIFIER" --ledger "$SOURCE_LEDGER" \
    --source-id "$TOPPRA_SOURCE_ID" --root "$TOPPRA_SOURCE" \
    --content-kind build "${verifier_git_args[@]}"
python3 "$SOURCE_VERIFIER" --ledger "$SOURCE_LEDGER" \
    --source-id "$NANOFLANN_SOURCE_ID" --root "$NANOFLANN_SOURCE" \
    --content-kind build "${verifier_git_args[@]}"
if [[ "$ALLOW_DEVELOPMENT_SOURCE" == "0" ]]; then
    python3 "$SOURCE_VERIFIER" --ledger "$SOURCE_LEDGER" \
        --source-id "$ROBOPLAN_SOURCE_ID" --root "$ROBOPLAN_SOURCE" \
        --content-kind build "${verifier_git_args[@]}"
else
    printf 'RoboPlan closure uses a development source; planner qualification is disabled\n' >&2
fi

if [[ ! -f "${PROXSUITE_SOURCE}/CMakeLists.txt" ]]; then
    echo "ProxSuite source is missing CMakeLists.txt" >&2
    exit 2
fi
if [[ ! -f "${TOPPRA_SOURCE}/cpp/CMakeLists.txt" ]]; then
    echo "TOPPRA source is missing cpp/CMakeLists.txt" >&2
    exit 2
fi
if [[ ! -f "${NANOFLANN_SOURCE}/include/nanoflann.hpp" ]]; then
    echo "nanoflann source is missing include/nanoflann.hpp" >&2
    exit 2
fi

SOURCE_PARENT="$(dirname "$ROBOPLAN_SOURCE")"
BUILD_ROOT="${ROBOPLAN_CLOSURE_BUILD_ROOT:-${SOURCE_PARENT}/roboplan-build-closure}"
INSTALL_ROOT="${ROBOPLAN_CLOSURE_INSTALL_ROOT:-${SOURCE_PARENT}/roboplan-install-closure}"
BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"
DEPENDENCY_BUILD_TESTING="${ROBOPLAN_DEPENDENCY_BUILD_TESTING:-OFF}"
if [[ ! "$BUILD_PARALLEL_LEVEL" =~ ^[1-9][0-9]*$ ]]; then
    echo "CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer" >&2
    exit 2
fi
if [[ "$DEPENDENCY_BUILD_TESTING" != "ON" && "$DEPENDENCY_BUILD_TESTING" != "OFF" ]]; then
    echo "ROBOPLAN_DEPENDENCY_BUILD_TESTING must be ON or OFF" >&2
    exit 2
fi

BUILD_ROOT="$(realpath -m -- "$BUILD_ROOT")"
INSTALL_ROOT="$(realpath -m -- "$INSTALL_ROOT")"
for output_root in "$BUILD_ROOT" "$INSTALL_ROOT"; do
    for source_root in "$PROXSUITE_SOURCE" "$JRL_CMAKEMODULES_SOURCE" "$TOPPRA_SOURCE" \
        "$NANOFLANN_SOURCE" "$ROBOPLAN_SOURCE"; do
        case "${output_root}/" in
            "${source_root}/"*)
                echo "closure output must remain outside every source root: ${output_root}" >&2
                exit 2
                ;;
        esac
    done
done
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

proxsuite_build="${BUILD_ROOT}/proxsuite"
proxsuite_install="${INSTALL_ROOT}/proxsuite"
toppra_build="${BUILD_ROOT}/toppra"
toppra_install="${INSTALL_ROOT}/toppra"

printf '[RoboPlan closure] configure ProxSuite\n'
cmake -S "$PROXSUITE_SOURCE" -B "$proxsuite_build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$proxsuite_install" \
    -DJRL_CMAKEMODULES_SOURCE_DIR="$JRL_CMAKEMODULES_SOURCE" \
    -DBUILD_TESTING=OFF \
    -DBUILD_PYTHON_INTERFACE=OFF \
    -DBUILD_WITH_VECTORIZATION_SUPPORT=OFF \
    -DPROXSUITE_BUILD_WITH_SERIALIZATION=OFF \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
printf '[RoboPlan closure] build ProxSuite\n'
cmake --build "$proxsuite_build" --parallel "$BUILD_PARALLEL_LEVEL"
printf '[RoboPlan closure] install ProxSuite\n'
cmake --install "$proxsuite_build"
if [[ ! -f "${proxsuite_install}/include/proxsuite/proxqp/dense/dense.hpp" ]]; then
    echo "exact ProxSuite install has an invalid public-header layout" >&2
    exit 1
fi

dependency_prefix="$(prepend_path "$proxsuite_install" "${CMAKE_PREFIX_PATH:-}")"
printf '[RoboPlan closure] configure TOPPRA\n'
CMAKE_PREFIX_PATH="$dependency_prefix" \
    cmake -S "${TOPPRA_SOURCE}/cpp" -B "$toppra_build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$toppra_install" \
        -DBUILD_TESTING="$DEPENDENCY_BUILD_TESTING" \
        -DPYTHON_BINDINGS=OFF \
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
        -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
printf '[RoboPlan closure] build TOPPRA\n'
cmake --build "$toppra_build" --parallel "$BUILD_PARALLEL_LEVEL"
if [[ "$DEPENDENCY_BUILD_TESTING" == "ON" ]]; then
    ctest --test-dir "$toppra_build" --output-on-failure
fi
printf '[RoboPlan closure] install TOPPRA\n'
cmake --install "$toppra_build"

dependency_prefix="$(prepend_path "$toppra_install" "$dependency_prefix")"
dependency_libraries="$(prepend_path "${toppra_install}/lib" "${LD_LIBRARY_PATH:-}")"
stack_environment=(
    "ROBOPLAN_BUILD_ROOT=${BUILD_ROOT}/roboplan"
    "ROBOPLAN_INSTALL_ROOT=${INSTALL_ROOT}/roboplan"
    "CMAKE_PREFIX_PATH=${dependency_prefix}"
    "LD_LIBRARY_PATH=${dependency_libraries}"
)
if [[ "$ALLOW_DEVELOPMENT_SOURCE" == "0" ]]; then
    stack_environment+=(
        "ROBOPLAN_SOURCE_LEDGER=${SOURCE_LEDGER}"
        "ROBOPLAN_SOURCE_VERIFIER=${SOURCE_VERIFIER}"
        "ROBOPLAN_SOURCE_ID=${ROBOPLAN_SOURCE_ID}"
        "ROBOPLAN_ALLOW_STRIPPED_GIT=${ALLOW_STRIPPED_GIT}"
    )
else
    # Explicitly clear any caller-wide qualification variables before the
    # nested stack builder. Otherwise a development override can accidentally
    # inherit and assert the selected RoboPlan identity even though the outer
    # closure correctly skipped that claim.
    stack_environment+=(
        "ROBOPLAN_SOURCE_LEDGER="
        "ROBOPLAN_SOURCE_ID="
        "ROBOPLAN_ALLOW_STRIPPED_GIT=0"
    )
fi
printf '[RoboPlan closure] build split RoboPlan packages\n'
env "${stack_environment[@]}" \
    bash "$(dirname "$0")/build-roboplan-source-stack.sh" \
    "$ROBOPLAN_SOURCE" \
    "$@" \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DFETCHCONTENT_UPDATES_DISCONNECTED=ON \
    -DFETCHCONTENT_SOURCE_DIR_NANOFLANN="$NANOFLANN_SOURCE" \
    -DCMAKE_DISABLE_FIND_PACKAGE_nanoflann=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_tl-expected=ON

proxsuite_cache="${BUILD_ROOT}/roboplan/roboplan_oink/CMakeCache.txt"
toppra_cache="${BUILD_ROOT}/roboplan/roboplan_toppra/CMakeCache.txt"
if ! grep -Fqx \
    "proxsuite_DIR:PATH=${proxsuite_install}/lib/cmake/proxsuite" \
    "$proxsuite_cache"; then
    echo "RoboPlan OInK did not select the exact ProxSuite install" >&2
    exit 1
fi
if ! grep -Fqx \
    "toppra_DIR:PATH=${toppra_install}/lib/cmake/toppra" \
    "$toppra_cache"; then
    echo "RoboPlan TOPPRA did not select the exact TOPPRA install" >&2
    exit 1
fi

printf 'Exact RoboPlan source closure built from %s\n' "$ROBOPLAN_SOURCE"
