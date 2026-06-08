# shellcheck shell=bash

if [[ -n "${_MANYFORGE_LIB_FRONTEND:-}" ]]; then return 0; fi
_MANYFORGE_LIB_FRONTEND=1

manyforge_build_frontend_dist() {
    local source_dir="${1:-/workspace/manyforge_composer/frontend}"
    local build_root="${2:-${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}}"
    local cache_version="manyforge-frontend-cache-v2"
    local cache_root="${build_root%/}/frontend-cache"
    local workspace_dir="${cache_root}/workspace"
    local manifest_path="${cache_root}/manifest.env"
    local helper_dir
    helper_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    local hash_script="${MANYFORGE_FRONTEND_HASH_SCRIPT:-${helper_dir}/frontend_hash.py}"
    local manifest_source_hash=""
    local manifest_dep_hash=""
    local manifest_dist_hash=""
    local manifest_toolchain_key=""
    local toolchain_key
    local dependency_cache_key
    local static_dir="${workspace_dir}/dist"
    local lock_fd=""

    echo "Building frontend (manyforge_composer/frontend)..."
    if [[ ! -d "${source_dir}" ]]; then
        echo "ERROR: frontend source directory not found: ${source_dir}" >&2
        return 1
    fi
    if [[ ! -f "${hash_script}" ]]; then
        echo "ERROR: frontend hash helper not found: ${hash_script}" >&2
        return 1
    fi
    for tool in python3 git node npm sha256sum awk tar flock uname; do
        if ! command -v "${tool}" >/dev/null 2>&1; then
            echo "ERROR: required frontend build tool not found: ${tool}" >&2
            return 1
        fi
    done
    mkdir -p "${cache_root}"
    exec {lock_fd}>"${cache_root}/build.lock"
    if ! flock "${lock_fd}"; then
        echo "ERROR: failed to acquire frontend build cache lock" >&2
        eval "exec ${lock_fd}>&-"
        return 1
    fi

    local hash_env
    if ! hash_env="$(python3 "${hash_script}" source "${source_dir}")"; then
        echo "ERROR: failed to fingerprint frontend source" >&2
        flock -u "${lock_fd}"
        eval "exec ${lock_fd}>&-"
        return 1
    fi
    eval "${hash_env}"
    if [[ "${MANYFORGE_FRONTEND_SOURCE_MODE}" == "git-head" ]] && [[ "${MANYFORGE_FRONTEND_DIRTY_FILE_COUNT:-0}" != "0" ]]; then
        echo "  (ignoring ${MANYFORGE_FRONTEND_DIRTY_FILE_COUNT} dirty/untracked frontend worktree file(s); source reference is committed HEAD)"
        echo "  (set MANYFORGE_FRONTEND_SOURCE_MODE=worktree for an explicit dev build from dirty files)"
    fi
    toolchain_key="$(
        {
            printf 'cache_version=%s\n' "${cache_version}"
            printf 'node=%s\n' "$(node --version)"
            printf 'npm=%s\n' "$(npm --version)"
            printf 'os=%s\n' "$(uname -s)"
            printf 'arch=%s\n' "$(uname -m)"
        } | sha256sum | awk '{print $1}'
    )"
    dependency_cache_key="$(
        {
            printf 'dependency_hash=%s\n' "${MANYFORGE_FRONTEND_DEP_HASH}"
            printf 'toolchain_key=%s\n' "${toolchain_key}"
        } | sha256sum | awk '{print $1}'
    )"

    if [[ -f "${manifest_path}" ]]; then
        manifest_source_hash="$(sed -n 's/^source_hash=//p' "${manifest_path}" | tail -n 1)"
        manifest_dep_hash="$(sed -n 's/^dependency_hash=//p' "${manifest_path}" | tail -n 1)"
        manifest_dist_hash="$(sed -n 's/^dist_hash=//p' "${manifest_path}" | tail -n 1)"
        manifest_toolchain_key="$(sed -n 's/^toolchain_key=//p' "${manifest_path}" | tail -n 1)"
    fi

    if [[ "${MANYFORGE_FRONTEND_FORCE_REBUILD:-0}" != "1" ]] \
        && [[ "${manifest_source_hash}" == "${MANYFORGE_FRONTEND_SOURCE_HASH}" ]] \
        && [[ "${manifest_dep_hash}" == "${MANYFORGE_FRONTEND_DEP_HASH}" ]] \
        && [[ "${manifest_toolchain_key}" == "${toolchain_key}" ]] \
        && [[ -f "${static_dir}/index.html" ]]; then
        if ! hash_env="$(python3 "${hash_script}" tree "${static_dir}")"; then
            echo "ERROR: failed to fingerprint cached frontend dist" >&2
            flock -u "${lock_fd}"
            eval "exec ${lock_fd}>&-"
            return 1
        fi
        eval "${hash_env}"
        if [[ "${MANYFORGE_FRONTEND_TREE_HASH}" == "${manifest_dist_hash}" ]]; then
            MANYFORGE_FRONTEND_STATIC_DIR="${static_dir}"
            export MANYFORGE_FRONTEND_STATIC_DIR
            echo "  (frontend cache hit: ${MANYFORGE_FRONTEND_SOURCE_HASH}; dist ${MANYFORGE_FRONTEND_TREE_HASH})"
            flock -u "${lock_fd}"
            eval "exec ${lock_fd}>&-"
            return 0
        fi
        echo "  (cached dist hash mismatch; rebuilding)"
    fi

    local tmp_dir="${cache_root}/workspace.next"
    local deps_stamp="${workspace_dir}/.manyforge-deps.hash"
    local keep_node_modules=0
    if [[ -d "${workspace_dir}/node_modules" ]] \
        && [[ "${manifest_dep_hash}" == "${MANYFORGE_FRONTEND_DEP_HASH}" ]] \
        && [[ "${manifest_toolchain_key}" == "${toolchain_key}" ]] \
        && [[ -f "${deps_stamp}" ]] \
        && [[ "$(cat "${deps_stamp}")" == "${dependency_cache_key}" ]]; then
        keep_node_modules=1
    fi

    rm -rf "${tmp_dir}"
    mkdir -p "${tmp_dir}"
    if [[ "${MANYFORGE_FRONTEND_SOURCE_MODE}" == "git-head" ]]; then
        git -c "safe.directory=*" -C "${MANYFORGE_FRONTEND_GIT_ROOT}" archive \
            --format=tar \
            "HEAD:${MANYFORGE_FRONTEND_GIT_REL}" \
            | tar -C "${tmp_dir}" -xf -
    else
        tar -C "${source_dir}" \
            --exclude="./node_modules" \
            --exclude="node_modules" \
            --exclude="./dist" \
            --exclude="dist" \
            -cf - . | tar -C "${tmp_dir}" -xf -
    fi

    if [[ "${keep_node_modules}" == "1" ]]; then
        echo "  (dependency hash unchanged; reusing cached node_modules)"
        ln -s "${workspace_dir}/node_modules" "${tmp_dir}/node_modules"
        printf '%s\n' "${dependency_cache_key}" > "${tmp_dir}/.manyforge-deps.hash"
    fi

    if ! (
        set -euo pipefail
        cd "${tmp_dir}"
        # `npm ci` (not `npm install`) so a stale node_modules against an
        # edited package-lock.json fails fast with a clear error instead of
        # producing a half-updated dependency tree.
        if [[ ! -d node_modules ]] || [[ ! -f .manyforge-deps.hash ]] || [[ "$(cat .manyforge-deps.hash)" != "${dependency_cache_key}" ]]; then
            echo "  (node_modules missing or dependency hash changed; running npm ci)"
            npm ci \
                --cache "${cache_root}/npm-cache" \
                --prefer-offline \
                --no-audit \
                --no-fund \
                2>&1
            printf '%s\n' "${dependency_cache_key}" > .manyforge-deps.hash
        else
            echo "  (dependency hash unchanged; skipping npm ci)"
        fi
        rm -rf dist
        npm run build 2>&1
    ); then
        echo "ERROR: frontend build failed for source hash ${MANYFORGE_FRONTEND_SOURCE_HASH}" >&2
        flock -u "${lock_fd}"
        eval "exec ${lock_fd}>&-"
        return 1
    fi

    if [[ ! -f "${tmp_dir}/dist/index.html" ]]; then
        echo "ERROR: frontend build completed without dist/index.html" >&2
        flock -u "${lock_fd}"
        eval "exec ${lock_fd}>&-"
        return 1
    fi
    if ! hash_env="$(python3 "${hash_script}" tree "${tmp_dir}/dist")"; then
        echo "ERROR: failed to fingerprint built frontend dist" >&2
        flock -u "${lock_fd}"
        eval "exec ${lock_fd}>&-"
        return 1
    fi
    eval "${hash_env}"

    local tmp_manifest="${manifest_path}.$$"
    {
        printf 'cache_version=%s\n' "${cache_version}"
        printf 'source_hash=%s\n' "${MANYFORGE_FRONTEND_SOURCE_HASH}"
        printf 'source_file_count=%s\n' "${MANYFORGE_FRONTEND_SOURCE_FILE_COUNT}"
        printf 'dependency_hash=%s\n' "${MANYFORGE_FRONTEND_DEP_HASH}"
        printf 'dependency_file_count=%s\n' "${MANYFORGE_FRONTEND_DEP_FILE_COUNT}"
        printf 'toolchain_key=%s\n' "${toolchain_key}"
        printf 'dependency_cache_key=%s\n' "${dependency_cache_key}"
        printf 'dist_hash=%s\n' "${MANYFORGE_FRONTEND_TREE_HASH}"
        printf 'dist_file_count=%s\n' "${MANYFORGE_FRONTEND_TREE_FILE_COUNT}"
        printf 'static_dir=%s\n' "${static_dir}"
        printf 'built_at_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "${tmp_manifest}"

    if [[ "${keep_node_modules}" == "1" ]] && [[ -L "${tmp_dir}/node_modules" ]]; then
        rm "${tmp_dir}/node_modules"
        mv "${workspace_dir}/node_modules" "${tmp_dir}/node_modules"
    fi

    rm -rf "${workspace_dir}"
    mv "${tmp_dir}" "${workspace_dir}"
    mv "${tmp_manifest}" "${manifest_path}"

    MANYFORGE_FRONTEND_STATIC_DIR="${static_dir}"
    export MANYFORGE_FRONTEND_STATIC_DIR
    echo "  (frontend dist ready at ${MANYFORGE_FRONTEND_STATIC_DIR}; source ${MANYFORGE_FRONTEND_SOURCE_HASH}; dist ${MANYFORGE_FRONTEND_TREE_HASH})"
    flock -u "${lock_fd}"
    eval "exec ${lock_fd}>&-"
}
