#!/usr/bin/env bash
# Build and, unless requested otherwise, exercise one sanitized I2 Python module.
set -euo pipefail
umask 022
export GIT_OPTIONAL_LOCKS=0
export LANG=C.UTF-8
export LC_ALL=C.UTF-8
export PYTHONNOUSERSITE=1
export PYTHONDONTWRITEBYTECODE=1
export TZ=UTC
unset PYTHONPATH PYTHONHOME LD_AUDIT LD_PRELOAD 2>/dev/null || true

usage() {
  echo "usage: $0 <address|thread> --build-root <path> --stage-root <path> [--prepare-only]" >&2
  exit 2
}

if [[ $# -ne 5 && $# -ne 6 ]]; then
  usage
fi
if [[ ( "$1" != "address" && "$1" != "thread" ) || \
      "$2" != "--build-root" || "$4" != "--stage-root" ]]; then
  usage
fi
SANITIZER="$1"
BUILD_ROOT="$3"
STAGE_ROOT="$5"
PREPARE_ONLY=0
if [[ $# -eq 6 ]]; then
  [[ "$6" == "--prepare-only" ]] || usage
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

canonical_absent_path "${BUILD_ROOT}" "build root"
canonical_absent_path "${STAGE_ROOT}" "stage root"
if [[ "${BUILD_ROOT}" == "${STAGE_ROOT}" || \
      "${BUILD_ROOT}" == "${STAGE_ROOT}/"* || \
      "${STAGE_ROOT}" == "${BUILD_ROOT}/"* ]]; then
  echo "binding build and stage roots must not contain one another" >&2
  exit 2
fi

expected_personality="00040000"
current_personality="$(cat /proc/self/personality)"
if [[ "${current_personality}" != "${expected_personality}" ]]; then
  if [[ "${MANYFORGE_BINDING_SANITIZER_SETARCH_REEXEC:-0}" == "1" ]]; then
    echo "binding sanitizer ADDR_NO_RANDOMIZE re-exec did not take effect" >&2
    exit 2
  fi
  export MANYFORGE_BINDING_SANITIZER_SETARCH_REEXEC=1
  exec /usr/bin/setarch x86_64 -R "$0" "$@"
fi
unset MANYFORGE_BINDING_SANITIZER_SETARCH_REEXEC
child_personality="$(sh -c 'cat /proc/self/personality')"
if [[ "${child_personality}" != "${expected_personality}" ]]; then
  echo "binding sanitizer ADDR_NO_RANDOMIZE inheritance check failed" >&2
  exit 2
fi

if [[ "${SANITIZER}" == "thread" ]]; then
  if [[ ! -f "${SCRIPT_DIR}/tsan.supp" || -L "${SCRIPT_DIR}/tsan.supp" ]]; then
    echo "TSan suppression file is unavailable" >&2
    exit 2
  fi
  SANITIZER_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"
  LINKER_FLAGS="-fsanitize=thread"
  export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 history_size=7 exitcode=66 suppressions=${SCRIPT_DIR}/tsan.supp"
  unset ASAN_OPTIONS UBSAN_OPTIONS 2>/dev/null || true
else
  SANITIZER_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
  LINKER_FLAGS="-fsanitize=address,undefined"
  # CPython and pybind11 intentionally retain process-global type/function
  # registries through interpreter teardown.  LSan reports those host-owned
  # registrations as leaks and cannot distinguish them from extension-owned
  # allocations.  Keep ASan/UBSan enabled for the binding lifetime cases;
  # native C++ sanitizer lanes retain leak detection.
  export ASAN_OPTIONS="halt_on_error=1:detect_leaks=0:exitcode=66"
  export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:exitcode=66"
  unset TSAN_OPTIONS 2>/dev/null || true
fi

mkdir -p "$(dirname "${BUILD_ROOT}")" "$(dirname "${STAGE_ROOT}")"
mkdir "${BUILD_ROOT}" "${STAGE_ROOT}"
CORE_BUILD="${BUILD_ROOT}/core"
DEPLOYMENT_BUILD="${BUILD_ROOT}/deployment"
MODULE_BUILD="${BUILD_ROOT}/module"
mkdir "${CORE_BUILD}" "${DEPLOYMENT_BUILD}" "${MODULE_BUILD}"

PYTHON_EXECUTABLE=/usr/local/bin/python3
if [[ "${SANITIZER}" == "address" ]]; then
  SANITIZER_HOST_ROOT="${BUILD_ROOT}/sanitizer-host"
  SANITIZER_HOST="${SANITIZER_HOST_ROOT}/manyforge-python-asan-ubsan"
  mkdir "${SANITIZER_HOST_ROOT}"
  read -r -a PYTHON_CFLAGS <<< "$(/usr/local/bin/python3-config --embed --cflags)"
  read -r -a PYTHON_LDFLAGS <<< "$(/usr/local/bin/python3-config --embed --ldflags)"
  /usr/bin/c++ \
    "${PYTHON_CFLAGS[@]}" \
    -std=c++20 \
    -O1 \
    -g \
    -fno-omit-frame-pointer \
    -fsanitize=address,undefined \
    "${SCRIPT_DIR}/core-bindings-sanitizer-host.cpp" \
    -Wl,--no-as-needed \
    -lstdc++ \
    "${PYTHON_LDFLAGS[@]}" \
    -fsanitize=address,undefined \
    -o "${SANITIZER_HOST}"
  if [[ ! -f "${SANITIZER_HOST}" || -L "${SANITIZER_HOST}" || \
        ! -x "${SANITIZER_HOST}" || \
        "$(stat -c '%h' -- "${SANITIZER_HOST}")" != "1" ]]; then
    echo "ASan/UBSan Python host is not one plain executable" >&2
    exit 1
  fi
  mapfile -t HOST_NEEDED < <(
    /usr/bin/readelf --wide --dynamic "${SANITIZER_HOST}" |
      sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p'
  )
  if [[ "${HOST_NEEDED[0]:-}" != "libasan.so.8" ]]; then
    echo "ASan/UBSan Python host does not initialize libasan first" >&2
    exit 1
  fi
  for required_library in \
    libubsan.so.1 \
    libstdc++.so.6 \
    libpython3.12.so.1.0; do
    if [[ " ${HOST_NEEDED[*]} " != *" ${required_library} "* ]]; then
      echo "ASan/UBSan Python host omits ${required_library}" >&2
      exit 1
    fi
  done
  PYTHON_EXECUTABLE="${SANITIZER_HOST}"
fi

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
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_CXX_FLAGS="${SANITIZER_FLAGS}" \
  -DCMAKE_EXE_LINKER_FLAGS="${LINKER_FLAGS}" \
  -DCMAKE_SHARED_LINKER_FLAGS="${LINKER_FLAGS}" \
  --graphviz="${CORE_BUILD}/manyforge-i2-target-graph.dot" \
  -S "${REPO_ROOT}/manyforge_core" \
  -B "${CORE_BUILD}"
/usr/bin/ninja -C "${CORE_BUILD}" -j "${PARALLEL}" -d keeprsp \
  manyforge_content_identity manyforge_core

prepare_cmake_metadata "${DEPLOYMENT_BUILD}"
/usr/bin/cmake \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_CXX_FLAGS="${SANITIZER_FLAGS}" \
  -DCMAKE_EXE_LINKER_FLAGS="${LINKER_FLAGS}" \
  -DCMAKE_SHARED_LINKER_FLAGS="${LINKER_FLAGS}" \
  --graphviz="${DEPLOYMENT_BUILD}/manyforge-i2-target-graph.dot" \
  -DMANYFORGE_CORE_INCLUDE_DIR="${REPO_ROOT}/manyforge_core/include" \
  -S "${REPO_ROOT}/manyforge_deployment" \
  -B "${DEPLOYMENT_BUILD}"
/usr/bin/ninja -C "${DEPLOYMENT_BUILD}" -j "${PARALLEL}" -d keeprsp \
  manyforge_content_identity \
  manyforge_deployment \
  manyforge_deployment_admission \
  manyforge_deployment_canonical_package_generator \
  manyforge_deployment_verification_core \
  manyforge_generate_canonical_empty_package

prepare_cmake_metadata "${MODULE_BUILD}"
/usr/bin/cmake \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_CXX_FLAGS="${SANITIZER_FLAGS}" \
  -DCMAKE_EXE_LINKER_FLAGS="${LINKER_FLAGS}" \
  -DCMAKE_SHARED_LINKER_FLAGS="${LINKER_FLAGS}" \
  -DCMAKE_MODULE_LINKER_FLAGS="${LINKER_FLAGS}" \
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

EXTENSION_NAME="manyforge_core_py.cpython-312-x86_64-linux-gnu.so"
BUILT_EXTENSION="${MODULE_BUILD}/${EXTENSION_NAME}"
if [[ ! -f "${BUILT_EXTENSION}" || -L "${BUILT_EXTENSION}" ]]; then
  echo "sanitized binding build omitted the exact CPython extension" >&2
  exit 1
fi
/usr/bin/install -m 0755 "${BUILT_EXTENSION}" "${STAGE_ROOT}/${EXTENSION_NAME}"
if [[ "$(find "${STAGE_ROOT}" -mindepth 1 -maxdepth 1 -type f -printf '%f\n')" != "${EXTENSION_NAME}" || \
      "$(find "${STAGE_ROOT}" -mindepth 1 -maxdepth 1 ! -type f -print -quit)" != "" ]]; then
  echo "sanitized binding stage is not extension-only" >&2
  exit 1
fi

if [[ "${PREPARE_ONLY}" == "1" ]]; then
  exit 0
fi

TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/manyforge-binding-sanitizer.XXXXXX")"
cleanup() {
  if [[ -n "${TEST_ROOT:-}" && -d "${TEST_ROOT}" ]]; then
    rm -rf -- "${TEST_ROOT}"
  fi
}
trap cleanup EXIT
mkdir "${TEST_ROOT}/evidence" "${TEST_ROOT}/generated" "${TEST_ROOT}/process-tmp"

export TMPDIR="${TEST_ROOT}/process-tmp"
export XDG_CACHE_HOME="${TEST_ROOT}/process-tmp/xdg-cache"
export PYTHONNOUSERSITE=1
export PYTHONDONTWRITEBYTECODE=1
export PYTEST_DISABLE_PLUGIN_AUTOLOAD=1
export PYTHONPATH="${STAGE_ROOT}"
export MANYFORGE_I2_BINDING_EXTENSION="${STAGE_ROOT}/${EXTENSION_NAME}"
export MANYFORGE_I2_BINDING_EVIDENCE_ROOT="${TEST_ROOT}/evidence"
export MANYFORGE_I2_BINDING_STAGED_ROOT="${STAGE_ROOT}"
export MANYFORGE_I2_BINDING_GENERATED_ROOT="${TEST_ROOT}/generated"
export MANYFORGE_I2_BINDING_SOURCE_ROOT="${REPO_ROOT}"
export MANYFORGE_I2_BINDING_EXECUTABLE="${PYTHON_EXECUTABLE}"

if [[ "${SANITIZER}" == "thread" ]]; then
  export TSAN_SUPPRESSIONS="${SCRIPT_DIR}/tsan.supp"
  export LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtsan.so.2
  TEST_CASES=(
    verification/composable_cell_i2/binding_contract_tests.py::test_concurrent_pure_compiles_are_deterministic
  )
else
  TEST_CASES=(
    verification/composable_cell_i2/binding_contract_tests.py::test_detached_predecessor_and_final_value_lifetime
    verification/composable_cell_i2/binding_contract_tests.py::test_exception_translation_after_released_gil
    verification/composable_cell_i2/binding_contract_tests.py::test_repeated_fresh_interpreter_import_use_and_clean_shutdown
  )
fi
cd "${REPO_ROOT}"
"${PYTHON_EXECUTABLE}" -m pytest -p no:cacheprovider -q "${TEST_CASES[@]}"
