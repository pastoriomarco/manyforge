#!/usr/bin/env bash
# scripts/ci/verify-version-sync.sh — fail if any sub-package's declared
# version doesn't match the repo-root VERSION file.
#
# Usage:
#   ./scripts/ci/verify-version-sync.sh          # run check, exit 0/1
#
# Each version file is read separately; mismatches are reported together
# so the operator sees the full picture in one pass instead of one error
# at a time.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

if [[ ! -f "${REPO_ROOT}/VERSION" ]]; then
    echo "  ✗ Repo-root VERSION file is missing at ${REPO_ROOT}/VERSION" >&2
    exit 1
fi

EXPECTED="$(<"${REPO_ROOT}/VERSION" tr -d '[:space:]')"
if [[ -z "${EXPECTED}" ]]; then
    echo "  ✗ Repo-root VERSION file is empty" >&2
    exit 1
fi

echo "Checking that every sub-package declares version ${EXPECTED}"

ERRORS=()

check() {
    local label="$1"
    local actual="$2"
    if [[ "${actual}" == "${EXPECTED}" ]]; then
        printf '  ✓ %-50s %s\n' "${label}" "${actual}"
    else
        printf '  ✗ %-50s expected %s, got %s\n' "${label}" "${EXPECTED}" "${actual}"
        ERRORS+=("${label}: ${actual} ≠ ${EXPECTED}")
    fi
}

# C++ packages — CMakeLists.txt project(NAME VERSION X.Y.Z ...)
extract_cmake_version() {
    local file="$1"
    grep -oE 'project\([^)]*VERSION\s+[0-9]+\.[0-9]+\.[0-9]+' "${file}" \
        | grep -oE '[0-9]+\.[0-9]+\.[0-9]+$' || true
}

check "manyforge_core CMakeLists.txt" "$(extract_cmake_version "${REPO_ROOT}/manyforge_core/CMakeLists.txt")"
check "manyforge_content_identity CMakeLists.txt" "$(extract_cmake_version "${REPO_ROOT}/manyforge_content_identity/CMakeLists.txt")"
check "manyforge_persistence CMakeLists.txt" "$(extract_cmake_version "${REPO_ROOT}/manyforge_persistence/CMakeLists.txt")"
check "manyforge_scene CMakeLists.txt" "$(extract_cmake_version "${REPO_ROOT}/manyforge_scene/CMakeLists.txt")"
check "manyforge_component_manager CMakeLists.txt" "$(extract_cmake_version "${REPO_ROOT}/manyforge_component_manager/CMakeLists.txt")"
check "manyforge_deployment CMakeLists.txt" "$(extract_cmake_version "${REPO_ROOT}/manyforge_deployment/CMakeLists.txt")"
check "manyforge_headless CMakeLists.txt" "$(extract_cmake_version "${REPO_ROOT}/manyforge_headless/CMakeLists.txt")"
check "manyforge_roboplan CMakeLists.txt" "$(extract_cmake_version "${REPO_ROOT}/manyforge_roboplan/CMakeLists.txt")"

# Python packages — pyproject.toml `version = "X.Y.Z"`
extract_pyproject_version() {
    local file="$1"
    grep -oE '^version\s*=\s*"[0-9]+\.[0-9]+\.[0-9]+"' "${file}" \
        | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || true
}

for pkg in \
    manyforge_behavior \
    manyforge_ros \
    manyforge_composer \
    manyforge_assistant_bridge \
    manyforge_headless_api \
    manyforge_program_runner \
    manyforge_operator; do
    file="${REPO_ROOT}/${pkg}/pyproject.toml"
    if [[ ! -f "${file}" ]]; then
        printf '  ✗ %-50s pyproject.toml missing\n' "${pkg}"
        ERRORS+=("${pkg}: pyproject.toml missing")
        continue
    fi
    check "${pkg} pyproject.toml" "$(extract_pyproject_version "${file}")"
done

# ROS package — package.xml <version>X.Y.Z</version>
extract_rospkg_version() {
    local file="$1"
    grep -oE '<version>[0-9]+\.[0-9]+\.[0-9]+</version>' "${file}" \
        | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || true
}
for pkg in \
    manyforge_cclc_msgs \
    manyforge_ros_bringup \
    manyforge_xarm_control \
    manyforge_xarm_adapter; do
    check "${pkg} package.xml" "$(extract_rospkg_version "${REPO_ROOT}/${pkg}/package.xml")"
done

# ROS package — setup.py version="X.Y.Z" or version='X.Y.Z'
extract_setup_py_version() {
    local file="$1"
    grep -oE "version=['\"][0-9]+\\.[0-9]+\\.[0-9]+['\"]" "${file}" \
        | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || true
}
for pkg in manyforge_ros_bringup manyforge_xarm_adapter; do
    check "${pkg} setup.py" "$(extract_setup_py_version "${REPO_ROOT}/${pkg}/setup.py")"
done

# Frontend — package.json "version": "X.Y.Z"
extract_packagejson_version() {
    local file="$1"
    python3 -c "
import json, sys
data = json.load(open('${file}'))
print(data.get('version', ''))
" 2>/dev/null || true
}
check "manyforge_composer/frontend package.json" "$(extract_packagejson_version "${REPO_ROOT}/manyforge_composer/frontend/package.json")"

# Frontend lockfile — the top-level "version" mirrors package.json, and
# `packages[""].version` mirrors it too. Both must match the repo VERSION
# so `npm ci` doesn't silently regenerate the file on first install with
# a different version pin.
extract_packagelock_version() {
    local file="$1"
    python3 -c "
import json, sys
data = json.load(open('${file}'))
# Top-level version
top = data.get('version', '')
# Workspace-root entry under packages
root_pkg = (data.get('packages') or {}).get('', {}).get('version', '')
# Fail if they disagree
if top and root_pkg and top != root_pkg:
    print(f'mismatch:{top}!={root_pkg}', file=sys.stderr)
    print('')
else:
    print(top or root_pkg)
" 2>/dev/null || true
}
if [[ -f "${REPO_ROOT}/manyforge_composer/frontend/package-lock.json" ]]; then
    check "manyforge_composer/frontend package-lock.json" \
        "$(extract_packagelock_version "${REPO_ROOT}/manyforge_composer/frontend/package-lock.json")"
fi

if (( ${#ERRORS[@]} > 0 )); then
    echo
    echo "Version sync FAILED. ${#ERRORS[@]} mismatch(es):" >&2
    for err in "${ERRORS[@]}"; do
        echo "  - ${err}" >&2
    done
    echo
    echo "To fix: edit the listed file(s) so they declare version ${EXPECTED}," >&2
    echo "or edit ${REPO_ROOT}/VERSION to the version you actually want to ship." >&2
    exit 1
fi

echo
echo "All sub-package versions match repo-root VERSION (${EXPECTED})."
