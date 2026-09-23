#!/usr/bin/env bash
# Generate and, unless requested otherwise, verify the I2 binding API contract.
set -euo pipefail
umask 022
export LANG=C.UTF-8
export LC_ALL=C.UTF-8
export PYTHONNOUSERSITE=1
export PYTHONDONTWRITEBYTECODE=1
export TZ=UTC
unset PYTHONPATH PYTHONHOME 2>/dev/null || true

usage() {
  echo "usage: $0 generated-api --extension <path> --generated-output-root <path> [--prepare-only]" >&2
  exit 2
}

if [[ $# -ne 5 && $# -ne 6 ]]; then
  usage
fi
if [[ "$1" != "generated-api" || "$2" != "--extension" || \
      "$4" != "--generated-output-root" ]]; then
  usage
fi
EXTENSION="$3"
GENERATED_ROOT="$5"
PREPARE_ONLY=0
if [[ $# -eq 6 ]]; then
  [[ "$6" == "--prepare-only" ]] || usage
  PREPARE_ONLY=1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

require_plain_file() {
  local path="$1"
  local label="$2"
  if [[ "${path}" != /* || "${path}" == "/" || \
        "$(realpath -e -- "${path}")" != "${path}" || \
        ! -f "${path}" || -L "${path}" || ! -s "${path}" || \
        "$(stat -c '%h' -- "${path}")" != "1" ]]; then
    echo "${label} must be one canonical nonempty regular file with one link" >&2
    exit 2
  fi
}

require_plain_file "${EXTENSION}" "candidate extension"
EXPECTED_SUFFIX="$(/usr/local/bin/python3 -c 'import sysconfig; print(sysconfig.get_config_var("EXT_SUFFIX"))')"
case "${EXPECTED_SUFFIX}" in
  .cpython-312-x86_64-linux-gnu.so|.cpython-312-aarch64-linux-gnu.so) ;;
  *) echo "active CPython extension suffix differs from the supported I2 ABI" >&2; exit 2 ;;
esac
EXPECTED_EXTENSION="manyforge_core_py${EXPECTED_SUFFIX}"
if [[ "${EXTENSION##*/}" != "${EXPECTED_EXTENSION}" ]]; then
  echo "candidate extension differs from the frozen I2 CPython ABI" >&2
  exit 2
fi
if [[ "${GENERATED_ROOT}" != /* || "${GENERATED_ROOT}" == "/" || \
      "$(realpath -m -- "${GENERATED_ROOT}")" != "${GENERATED_ROOT}" ]]; then
  echo "generated output root must be an absolute canonical non-root path" >&2
  exit 2
fi
if [[ "${GENERATED_ROOT}" == "${REPO_ROOT}" || \
      "${GENERATED_ROOT}" == "${REPO_ROOT}/"* ]]; then
  echo "generated output root must not modify the source snapshot" >&2
  exit 2
fi
if [[ -e "${GENERATED_ROOT}" || -L "${GENERATED_ROOT}" ]]; then
  echo "generated output root must be fresh: ${GENERATED_ROOT}" >&2
  exit 2
fi

mkdir -p "$(dirname "${GENERATED_ROOT}")"
mkdir "${GENERATED_ROOT}"
/usr/bin/install -m 0755 "${EXTENSION}" "${GENERATED_ROOT}/${EXPECTED_EXTENSION}"
/usr/local/bin/python3 "${SCRIPT_DIR}/generate-core-bindings-contract.py" \
  --extension "${EXTENSION}" \
  --output-root "${GENERATED_ROOT}"

EXPECTED_INVENTORY=$(printf '%s\n' manyforge_core_py-api.md manyforge_core_py.api.json "${EXPECTED_EXTENSION}" manyforge_core_py.pyi | LC_ALL=C sort)
ACTUAL_INVENTORY="$(find "${GENERATED_ROOT}" -mindepth 1 -maxdepth 1 \
  -printf '%f\n' | LC_ALL=C sort)"
if [[ "${ACTUAL_INVENTORY}" != "${EXPECTED_INVENTORY}" ]]; then
  echo "generated binding output inventory is not exact" >&2
  exit 1
fi
while IFS= read -r output; do
  require_plain_file "${GENERATED_ROOT}/${output}" "generated binding output"
done <<< "${EXPECTED_INVENTORY}"

if [[ "${PREPARE_ONLY}" == "1" ]]; then
  exit 0
fi

TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/manyforge-binding-contract.XXXXXX")"
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
export PYTHONPATH="$(dirname "${EXTENSION}")"
export MANYFORGE_I2_BINDING_EXTENSION="${EXTENSION}"
export MANYFORGE_I2_BINDING_EVIDENCE_ROOT="${TEST_ROOT}/evidence"
export MANYFORGE_I2_BINDING_STAGED_ROOT="$(dirname "${EXTENSION}")"
export MANYFORGE_I2_BINDING_GENERATED_ROOT="${GENERATED_ROOT}"
export MANYFORGE_I2_BINDING_SOURCE_ROOT="${REPO_ROOT}"
export MANYFORGE_I2_BINDING_EXECUTABLE=/usr/local/bin/python3

cd "${REPO_ROOT}"
/usr/local/bin/python3 -m pytest -p no:cacheprovider -q \
  verification/composable_cell_i2/binding_contract_tests.py::test_generated_stub_inventory_and_documentation_parity \
  verification/composable_cell_i2/binding_contract_tests.py::test_complete_forbidden_surface_absence
