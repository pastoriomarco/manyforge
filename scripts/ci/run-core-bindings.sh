#!/usr/bin/env bash
# Build and, unless requested otherwise, exercise the sole I2 Python module.
set -euo pipefail
umask 022
export GIT_OPTIONAL_LOCKS=0
export LANG=C.UTF-8
export LC_ALL=C.UTF-8
export PYTHONNOUSERSITE=1
export PYTHONDONTWRITEBYTECODE=1
export TZ=UTC
unset PYTHONPATH PYTHONHOME 2>/dev/null || true

usage() {
  echo "usage: $0 release --build-root <path> --stage-root <path> --generated-output-root <path> [--prepare-only]" >&2
  exit 2
}

if [[ $# -ne 7 && $# -ne 8 ]]; then
  usage
fi
if [[ "$1" != "release" || "$2" != "--build-root" || \
      "$4" != "--stage-root" || "$6" != "--generated-output-root" ]]; then
  usage
fi
BUILD_ROOT="$3"
STAGE_ROOT="$5"
GENERATED_ROOT="$7"
PREPARE_ONLY=0
if [[ $# -eq 8 ]]; then
  [[ "$8" == "--prepare-only" ]] || usage
  PREPARE_ONLY=1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PARALLEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
if [[ ! "${PARALLEL}" =~ ^[1-9][0-9]*$ ]]; then
  echo "CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer" >&2
  exit 2
fi

canonical_absent_path() {
  local value="$1"
  local label="$2"
  local normalized
  if [[ "${value}" != /* || "${value}" == "/" ]]; then
    echo "${label} must be an absolute non-root path" >&2
    exit 2
  fi
  normalized="$(realpath -m -- "${value}")"
  if [[ "${normalized}" != "${value}" ]]; then
    echo "${label} must be canonical and must not traverse symlinks" >&2
    exit 2
  fi
  if [[ "${value}" == "${REPO_ROOT}" || "${value}" == "${REPO_ROOT}/"* ]]; then
    echo "${label} must not modify the source snapshot" >&2
    exit 2
  fi
  if [[ -e "${value}" || -L "${value}" ]]; then
    echo "${label} must be fresh: ${value}" >&2
    exit 2
  fi
}

for entry in \
  "${BUILD_ROOT}:build root" \
  "${STAGE_ROOT}:stage root" \
  "${GENERATED_ROOT}:generated output root"; do
  canonical_absent_path "${entry%%:*}" "${entry#*:}"
done
if [[ "${BUILD_ROOT}" == "${STAGE_ROOT}" || \
      "${BUILD_ROOT}" == "${GENERATED_ROOT}" || \
      "${STAGE_ROOT}" == "${GENERATED_ROOT}" ]]; then
  echo "binding output roots must be distinct" >&2
  exit 2
fi
for left in "${BUILD_ROOT}" "${STAGE_ROOT}" "${GENERATED_ROOT}"; do
  for right in "${BUILD_ROOT}" "${STAGE_ROOT}" "${GENERATED_ROOT}"; do
    if [[ "${left}" != "${right}" && "${right}" == "${left}/"* ]]; then
      echo "binding output roots must not contain one another" >&2
      exit 2
    fi
  done
done

mkdir -p "$(dirname "${BUILD_ROOT}")" "$(dirname "${STAGE_ROOT}")"
mkdir "${BUILD_ROOT}" "${STAGE_ROOT}"
CORE_BUILD="${BUILD_ROOT}/core"
DEPLOYMENT_BUILD="${BUILD_ROOT}/deployment"
MODULE_BUILD="${BUILD_ROOT}/module"
mkdir "${CORE_BUILD}" "${DEPLOYMENT_BUILD}" "${MODULE_BUILD}"

prepare_cmake_metadata() {
  local build="$1"
  mkdir -p "${build}/.cmake/api/v1/query"
  printf '{}\n' > "${build}/.cmake/api/v1/query/codemodel-v2"
  printf '{}\n' > "${build}/.cmake/api/v1/query/cmakeFiles-v1"
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
    > "${build}/CMakeGraphVizOptions.cmake"
}

prepare_cmake_metadata "${CORE_BUILD}"
/usr/bin/cmake \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  --graphviz="${CORE_BUILD}/manyforge-i2-target-graph.dot" \
  -S "${REPO_ROOT}/manyforge_core" \
  -B "${CORE_BUILD}"
/usr/bin/ninja -C "${CORE_BUILD}" -j "${PARALLEL}" -d keeprsp \
  manyforge_content_identity manyforge_core

prepare_cmake_metadata "${DEPLOYMENT_BUILD}"
/usr/bin/cmake \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  --graphviz="${DEPLOYMENT_BUILD}/manyforge-i2-target-graph.dot" \
  -DMANYFORGE_CORE_INCLUDE_DIR="${REPO_ROOT}/manyforge_core/include" \
  -S "${REPO_ROOT}/manyforge_deployment" \
  -B "${DEPLOYMENT_BUILD}"
/usr/bin/ninja -C "${DEPLOYMENT_BUILD}" -j "${PARALLEL}" -d keeprsp \
  manyforge_build_qualified_admission_profile \
  manyforge_content_identity \
  manyforge_deployment \
  manyforge_deployment_admission \
  manyforge_deployment_canonical_package_generator \
  manyforge_deployment_verification_core \
  manyforge_emit_build_qualified_admission_profile \
  manyforge_generate_canonical_empty_package

prepare_cmake_metadata "${MODULE_BUILD}"
/usr/bin/cmake \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  --graphviz="${MODULE_BUILD}/manyforge-i2-target-graph.dot" \
  -DPython_EXECUTABLE=/usr/local/bin/python3 \
  -Dpybind11_DIR=/usr/local/lib/python3.12/site-packages/pybind11/share/cmake/pybind11 \
  -DMANYFORGE_CORE_DIR="${CORE_BUILD}" \
  -DMANYFORGE_CORE_INCLUDE_DIR="${REPO_ROOT}/manyforge_core/include" \
  -DMANYFORGE_DEPLOYMENT_DIR="${DEPLOYMENT_BUILD}" \
  -DMANYFORGE_DEPLOYMENT_INCLUDE_DIR="${REPO_ROOT}/manyforge_deployment/include" \
  -DMANYFORGE_CONTENT_IDENTITY_SOURCE_DIR="${REPO_ROOT}/manyforge_content_identity" \
  -S "${REPO_ROOT}/manyforge_core/python" \
  -B "${MODULE_BUILD}"
/usr/bin/ninja -C "${MODULE_BUILD}" -j "${PARALLEL}" -d keeprsp \
  manyforge_core_py

EXTENSION_SUFFIX="$(/usr/local/bin/python3 -c 'import sysconfig; print(sysconfig.get_config_var("EXT_SUFFIX"))')"
if [[ "${EXTENSION_SUFFIX}" != ".cpython-312-x86_64-linux-gnu.so" ]]; then
  echo "active CPython extension suffix differs from the frozen I2 ABI" >&2
  exit 2
fi
EXTENSION_NAME="manyforge_core_py${EXTENSION_SUFFIX}"
BUILT_EXTENSION="${MODULE_BUILD}/${EXTENSION_NAME}"
if [[ ! -f "${BUILT_EXTENSION}" || -L "${BUILT_EXTENSION}" ]]; then
  echo "binding build omitted the exact CPython extension" >&2
  exit 1
fi
/usr/bin/install -m 0755 "${BUILT_EXTENSION}" "${STAGE_ROOT}/${EXTENSION_NAME}"
if [[ "$(find "${STAGE_ROOT}" -mindepth 1 -maxdepth 1 -type f -printf '%f\n')" != "${EXTENSION_NAME}" || \
      "$(find "${STAGE_ROOT}" -mindepth 1 -maxdepth 1 ! -type f -print -quit)" != "" ]]; then
  echo "binding stage is not extension-only" >&2
  exit 1
fi

"${SCRIPT_DIR}/run-core-bindings-contract.sh" \
  generated-api \
  --extension "${STAGE_ROOT}/${EXTENSION_NAME}" \
  --generated-output-root "${GENERATED_ROOT}" \
  --prepare-only

if [[ "${PREPARE_ONLY}" == "1" ]]; then
  exit 0
fi

TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/manyforge-core-bindings.XXXXXX")"
cleanup() {
  if [[ -n "${TEST_ROOT:-}" && -d "${TEST_ROOT}" ]]; then
    rm -rf -- "${TEST_ROOT}"
  fi
}
trap cleanup EXIT
mkdir "${TEST_ROOT}/evidence" "${TEST_ROOT}/process-tmp"

export TMPDIR="${TEST_ROOT}/process-tmp"
export XDG_CACHE_HOME="${TEST_ROOT}/process-tmp/xdg-cache"
export PYTHONNOUSERSITE=1
export PYTHONDONTWRITEBYTECODE=1
export PYTEST_DISABLE_PLUGIN_AUTOLOAD=1
export PYTHONPATH="${STAGE_ROOT}"
export MANYFORGE_I2_BINDING_EXTENSION="${STAGE_ROOT}/${EXTENSION_NAME}"
export MANYFORGE_I2_BINDING_EVIDENCE_ROOT="${TEST_ROOT}/evidence"
export MANYFORGE_I2_BINDING_STAGED_ROOT="${STAGE_ROOT}"
export MANYFORGE_I2_BINDING_GENERATED_ROOT="${GENERATED_ROOT}"
export MANYFORGE_I2_BINDING_SOURCE_ROOT="${REPO_ROOT}"
export MANYFORGE_I2_BINDING_EXECUTABLE=/usr/local/bin/python3

cd "${REPO_ROOT}"
/usr/local/bin/python3 -m pytest -p no:cacheprovider -q \
  manyforge_core/python/tests/test_bindings_smoke.py \
  manyforge_core/python/tests/test_collision_bindings.py \
  manyforge_core/python/tests/test_command_schema_bindings.py \
  manyforge_core/python/tests/test_component_type_bindings.py \
  manyforge_core/python/tests/test_composable_cell_bindings.py \
  manyforge_core/python/tests/test_coordinate_state_bindings.py \
  manyforge_core/python/tests/test_controller_topology_bindings.py \
  manyforge_core/python/tests/test_model_asset_bindings.py \
  manyforge_core/python/tests/test_model_compiler_bindings.py \
  manyforge_core/python/tests/test_provider_manifest_bindings.py \
  manyforge_core/python/tests/test_runtime_profile_bindings.py \
  verification/composable_cell_i2/binding_contract_tests.py
