# shellcheck shell=bash
# Bounded frontend source snapshot, build, and cache helper.
#
# Exact historical sources:
#   scripts/lib/frontend.sh
#     manyforge@08222188293df8e12f90dc72e204c21f22f92ea1
#     blob 2cf625416bcd6db0526a091784f8f2edc780e845
#   scripts/lib/frontend_hash.py
#     blob c9855c86150de04349e9ac33a9200b9b18bd0898
#
# Frontend compilation remains same-role build tooling. This library does not
# launch Composer or own runtime truth. The old evaluation of helper-generated
# shell assignments and caller-selected hash program are removed; the fixed
# helper now returns a bounded, parsed record.

if [[ -n "${_MANYFORGE_LIB_FRONTEND:-}" ]]; then
    return 0
fi
_MANYFORGE_LIB_FRONTEND=1

readonly _MANYFORGE_FRONTEND_CACHE_VERSION="manyforge-frontend-cache-v3"
readonly _MANYFORGE_FRONTEND_MAX_PATH_CHARACTERS=1024
readonly _MANYFORGE_FRONTEND_MAX_MANIFEST_BYTES=16384
readonly _MANYFORGE_FRONTEND_MAX_BUILD_LOG_BLOCKS=32768
readonly _MANYFORGE_FRONTEND_MAX_HASH_SCRIPT_BYTES=1048576
readonly _MANYFORGE_FRONTEND_MAX_RECORD_BLOCKS=256
readonly _MANYFORGE_FRONTEND_RECORD_TIMEOUT_SECONDS=180
readonly _MANYFORGE_FRONTEND_COPY_TIMEOUT_SECONDS=300
readonly _MANYFORGE_FRONTEND_NPM_TIMEOUT_SECONDS=1800

_manyforge_frontend_safe_path_text() {
    if (( $# != 1 )); then
        return 2
    fi
    local value="$1"
    local LC_ALL=C
    [[ "${value}" == /* ]] \
        && (( ${#value} > 1 && ${#value} <= _MANYFORGE_FRONTEND_MAX_PATH_CHARACTERS )) \
        && [[ ! "${value}" =~ [[:cntrl:]] ]]
}

_manyforge_frontend_hash_script_identity() {
    if (( $# != 2 )); then
        return 2
    fi
    local script_path="$1"
    local output_name="$2"
    [[ "${output_name}" =~ ^[A-Za-z_][A-Za-z0-9_]{0,127}$ ]] || return 2
    local -n output_identity="${output_name}"
    output_identity=""

    [[ -f "${script_path}" && ! -L "${script_path}" ]] || return 2
    local before=""
    local after=""
    local size=""
    before="$(stat -c '%d:%i:%s:%y:%z' -- "${script_path}")" || return 2
    size="$(stat -c '%s' -- "${script_path}")" || return 2
    [[ "${size}" =~ ^[0-9]+$ ]] \
        && (( size > 0 && size <= _MANYFORGE_FRONTEND_MAX_HASH_SCRIPT_BYTES )) \
        || return 2
    output_identity="$(sha256sum -- "${script_path}")" || return 2
    output_identity="${output_identity%% *}"
    after="$(stat -c '%d:%i:%s:%y:%z' -- "${script_path}")" || return 2
    [[ "${before}" == "${after}" \
        && "${output_identity}" =~ ^[0-9a-f]{64}$ ]]
}

_manyforge_frontend_load_record() {
    if (( $# != 7 )); then
        return 2
    fi
    local hash_script="$1"
    local command="$2"
    local subject="$3"
    local cache_root="$4"
    local output_name="$5"
    local source_mode_override="$6"
    local expected_script_identity="$7"
    [[ "${output_name}" =~ ^[A-Za-z_][A-Za-z0-9_]{0,127}$ ]] || return 2
    [[ "${expected_script_identity}" =~ ^[0-9a-f]{64}$ ]] || return 2
    local -n output_record="${output_name}"
    output_record=()

    local current_script_identity=""
    _manyforge_frontend_hash_script_identity \
        "${hash_script}" current_script_identity || return 2
    [[ "${current_script_identity}" == "${expected_script_identity}" ]] || return 2

    # Execute the already-attested open file, not a second path lookup. The
    # helper is time/process-group bounded and applies its own input/scratch
    # bounds. Apply the record file limit only to the output sink so valid Git
    # batch scratch files are not mistaken for an oversized record. The path
    # identity is checked again after the invocation.
    local hash_script_fd=""
    exec {hash_script_fd}<"${hash_script}" || return 2
    local open_script_identity=""
    open_script_identity="$(sha256sum -- "/proc/self/fd/${hash_script_fd}")" || {
        exec {hash_script_fd}<&-
        return 2
    }
    open_script_identity="${open_script_identity%% *}"
    if [[ "${open_script_identity}" != "${expected_script_identity}" ]]; then
        exec {hash_script_fd}<&-
        return 2
    fi

    local record_path=""
    record_path="$(mktemp "${cache_root}/hash-record.XXXXXXXX")" || {
        exec {hash_script_fd}<&-
        return 2
    }
    local record_status=0
    if [[ -n "${source_mode_override}" ]]; then
        (
            set -o pipefail
            MANYFORGE_FRONTEND_SOURCE_MODE="${source_mode_override}" \
                timeout --signal=TERM --kill-after=5s \
                    "${_MANYFORGE_FRONTEND_RECORD_TIMEOUT_SECONDS}" \
                    python3 "/proc/self/fd/${hash_script_fd}" \
                        --output-format nul "${command}" "${subject}" \
                | (
                    ulimit -c 0 || exit 2
                    ulimit -f "${_MANYFORGE_FRONTEND_MAX_RECORD_BLOCKS}" \
                        || exit 2
                    exec cat
                )
        ) >"${record_path}" || record_status=$?
    else
        (
            set -o pipefail
            timeout --signal=TERM --kill-after=5s \
                "${_MANYFORGE_FRONTEND_RECORD_TIMEOUT_SECONDS}" \
                python3 "/proc/self/fd/${hash_script_fd}" \
                    --output-format nul "${command}" "${subject}" \
                | (
                    ulimit -c 0 || exit 2
                    ulimit -f "${_MANYFORGE_FRONTEND_MAX_RECORD_BLOCKS}" \
                        || exit 2
                    exec cat
                )
        ) >"${record_path}" || record_status=$?
    fi
    exec {hash_script_fd}<&-
    current_script_identity=""
    if ! _manyforge_frontend_hash_script_identity \
        "${hash_script}" current_script_identity \
        || [[ "${current_script_identity}" != "${expected_script_identity}" ]]; then
        rm -f -- "${record_path}"
        return 2
    fi
    if (( record_status != 0 )); then
        rm -f -- "${record_path}"
        return 1
    fi
    local record_size=""
    record_size="$(stat -c '%s' -- "${record_path}")" || {
        rm -f -- "${record_path}"
        return 2
    }
    [[ "${record_size}" =~ ^[0-9]+$ ]] \
        && (( record_size <= _MANYFORGE_FRONTEND_MAX_RECORD_BLOCKS * 512 )) \
        || { rm -f -- "${record_path}"; return 2; }

    local key=""
    local value=""
    local count=0
    local expected=0
    while IFS= read -r -d '' key; do
        if ! IFS= read -r -d '' value; then
            rm -f -- "${record_path}"
            return 2
        fi
        case "${command}:${key}" in
            source:MANYFORGE_FRONTEND_DEP_FILE_COUNT \
                | source:MANYFORGE_FRONTEND_DEP_HASH \
                | source:MANYFORGE_FRONTEND_DIRTY_FILE_COUNT \
                | source:MANYFORGE_FRONTEND_GIT_REL \
                | source:MANYFORGE_FRONTEND_GIT_ROOT \
                | source:MANYFORGE_FRONTEND_GIT_TREE_HASH \
                | source:MANYFORGE_FRONTEND_SOURCE_FILE_COUNT \
                | source:MANYFORGE_FRONTEND_SOURCE_HASH \
                | source:MANYFORGE_FRONTEND_SOURCE_MODE \
                | source:MANYFORGE_FRONTEND_SNAPSHOT_HASH \
                | tree:MANYFORGE_FRONTEND_TREE_FILE_COUNT \
                | tree:MANYFORGE_FRONTEND_TREE_HASH)
                ;;
            *)
                rm -f -- "${record_path}"
                return 2
                ;;
        esac
        if [[ -v "output_record[${key}]" ]]; then
            rm -f -- "${record_path}"
            return 2
        fi
        output_record["${key}"]="${value}"
        count=$((count + 1))
    done <"${record_path}"
    rm -f -- "${record_path}"

    case "${command}" in
        source) expected=10 ;;
        tree) expected=2 ;;
        *) return 2 ;;
    esac
    (( count == expected )) || return 2

    if [[ "${command}" == "tree" ]]; then
        [[ "${output_record[MANYFORGE_FRONTEND_TREE_HASH]}" =~ ^[0-9a-f]{64}$ ]] \
            && [[ "${output_record[MANYFORGE_FRONTEND_TREE_FILE_COUNT]}" =~ ^[0-9]+$ ]] \
            && (( output_record[MANYFORGE_FRONTEND_TREE_FILE_COUNT] <= 20000 ))
        return $?
    fi

    [[ "${output_record[MANYFORGE_FRONTEND_SOURCE_MODE]}" == "git-head" \
        || "${output_record[MANYFORGE_FRONTEND_SOURCE_MODE]}" == "worktree" ]] \
        || return 2
    [[ "${output_record[MANYFORGE_FRONTEND_SOURCE_HASH]}" =~ ^[0-9a-f]{64}$ ]] \
        || return 2
    [[ "${output_record[MANYFORGE_FRONTEND_DEP_HASH]}" =~ ^[0-9a-f]{64}$ ]] \
        || return 2
    [[ "${output_record[MANYFORGE_FRONTEND_SNAPSHOT_HASH]}" =~ ^[0-9a-f]{64}$ ]] \
        || return 2
    local count_key
    for count_key in \
        MANYFORGE_FRONTEND_SOURCE_FILE_COUNT \
        MANYFORGE_FRONTEND_DEP_FILE_COUNT \
        MANYFORGE_FRONTEND_DIRTY_FILE_COUNT; do
        [[ "${output_record[${count_key}]}" =~ ^[0-9]+$ ]] || return 2
        (( output_record[${count_key}] <= 40000 )) || return 2
    done
    if [[ "${output_record[MANYFORGE_FRONTEND_SOURCE_MODE]}" == "git-head" ]]; then
        _manyforge_frontend_safe_path_text \
            "${output_record[MANYFORGE_FRONTEND_GIT_ROOT]}" || return 2
        [[ -n "${output_record[MANYFORGE_FRONTEND_GIT_REL]}" ]] \
            && (( ${#output_record[MANYFORGE_FRONTEND_GIT_REL]} <= _MANYFORGE_FRONTEND_MAX_PATH_CHARACTERS )) \
            && [[ "${output_record[MANYFORGE_FRONTEND_GIT_TREE_HASH]}" =~ ^[0-9a-f]{40}$ ]] \
            || return 2
    else
        [[ -z "${output_record[MANYFORGE_FRONTEND_GIT_ROOT]}" \
            && -z "${output_record[MANYFORGE_FRONTEND_GIT_REL]}" \
            && -z "${output_record[MANYFORGE_FRONTEND_GIT_TREE_HASH]}" ]] \
            || return 2
    fi
}

_manyforge_frontend_load_manifest() {
    if (( $# != 2 )); then
        return 2
    fi
    local manifest_path="$1"
    local output_name="$2"
    [[ "${output_name}" =~ ^[A-Za-z_][A-Za-z0-9_]{0,127}$ ]] || return 2
    local -n output_manifest="${output_name}"
    output_manifest=()

    if [[ ! -e "${manifest_path}" && ! -L "${manifest_path}" ]]; then
        return 1
    fi
    if [[ -L "${manifest_path}" || ! -f "${manifest_path}" ]]; then
        return 2
    fi
    local manifest_before=""
    local manifest_opened=""
    local manifest_after=""
    local manifest_path_after=""
    local manifest_size=""
    manifest_before="$(stat -c '%d:%i:%s:%y:%z' -- "${manifest_path}")" \
        || return 2
    manifest_size="$(stat -c '%s' -- "${manifest_path}")" || return 2
    [[ "${manifest_size}" =~ ^[0-9]+$ ]] \
        && (( manifest_size <= _MANYFORGE_FRONTEND_MAX_MANIFEST_BYTES )) \
        || return 2

    local manifest_fd=""
    exec {manifest_fd}<"${manifest_path}" || return 2
    manifest_opened="$(stat -Lc '%d:%i:%s:%y:%z' -- "/proc/self/fd/${manifest_fd}")" \
        || { exec {manifest_fd}<&-; return 2; }
    if [[ "${manifest_opened}" != "${manifest_before}" ]]; then
        exec {manifest_fd}<&-
        return 2
    fi
    local -a manifest_lines=()
    mapfile -t manifest_lines <&"${manifest_fd}" || {
        exec {manifest_fd}<&-
        return 2
    }
    manifest_after="$(stat -Lc '%d:%i:%s:%y:%z' -- "/proc/self/fd/${manifest_fd}")" \
        || { exec {manifest_fd}<&-; return 2; }
    exec {manifest_fd}<&-
    manifest_path_after="$(stat -c '%d:%i:%s:%y:%z' -- "${manifest_path}")" \
        || return 2
    [[ "${manifest_before}" == "${manifest_after}" \
        && "${manifest_before}" == "${manifest_path_after}" ]] || return 2

    local line=""
    local key=""
    local value=""
    local count=0
    for line in "${manifest_lines[@]}"; do
        (( ${#line} <= 4096 )) || return 2
        [[ ! "${line}" =~ [[:cntrl:]] ]] || return 2
        [[ "${line}" == *=* ]] || return 2
        key="${line%%=*}"
        value="${line#*=}"
        case "${key}" in
            cache_version | source_hash | source_file_count \
                | dependency_hash | dependency_file_count | toolchain_key \
                | dependency_cache_key | dist_hash | dist_file_count \
                | static_dir | built_at_utc)
                ;;
            *) return 2 ;;
        esac
        [[ ! -v "output_manifest[${key}]" ]] || return 2
        output_manifest["${key}"]="${value}"
        count=$((count + 1))
        (( count <= 11 )) || return 2
    done
    (( count == 11 ))
}

_manyforge_frontend_tool_identity() {
    if (( $# < 3 || $# > 4 )); then
        return 2
    fi
    local cache_root="$1"
    local output_name="$2"
    shift 2
    [[ "${output_name}" =~ ^[A-Za-z_][A-Za-z0-9_]{0,127}$ ]] || return 2
    local -n output_value="${output_name}"
    local output_path=""
    output_path="$(mktemp "${cache_root}/tool-identity.XXXXXXXX")" || return 1
    if ! (
        ulimit -f 8
        timeout 10 "$@"
    ) >"${output_path}" 2>&1; then
        rm -f -- "${output_path}"
        return 1
    fi
    local output_size=""
    output_size="$(stat -c '%s' -- "${output_path}")" || {
        rm -f -- "${output_path}"
        return 1
    }
    if [[ ! "${output_size}" =~ ^[0-9]+$ ]] || (( output_size > 4096 )); then
        rm -f -- "${output_path}"
        return 1
    fi
    local captured=""
    captured="$(<"${output_path}")"
    rm -f -- "${output_path}"
    local LC_ALL=C
    if [[ -z "${captured}" || "${captured}" =~ [[:cntrl:]] ]]; then
        return 1
    fi
    output_value="${captured}"
}

_manyforge_frontend_locked_build() {
    if (( $# != 8 )); then
        return 2
    fi
    local source_dir="$1"
    local cache_root="$2"
    local workspace_dir="$3"
    local manifest_path="$4"
    local hash_script="$5"
    local static_dir="$6"
    local output_name="$7"
    local hash_script_identity="$8"
    [[ "${output_name}" =~ ^[A-Za-z_][A-Za-z0-9_]{0,127}$ ]] || return 2
    local -n output_static_dir="${output_name}"
    output_static_dir=""

    local -A source_record=()
    if ! _manyforge_frontend_load_record \
        "${hash_script}" source "${source_dir}" "${cache_root}" \
        source_record "" "${hash_script_identity}"; then
        printf 'ERROR: failed to fingerprint bounded frontend source\n' >&2
        return 1
    fi
    local source_mode="${source_record[MANYFORGE_FRONTEND_SOURCE_MODE]}"
    local source_hash="${source_record[MANYFORGE_FRONTEND_SOURCE_HASH]}"
    local source_count="${source_record[MANYFORGE_FRONTEND_SOURCE_FILE_COUNT]}"
    local dependency_hash="${source_record[MANYFORGE_FRONTEND_DEP_HASH]}"
    local dependency_count="${source_record[MANYFORGE_FRONTEND_DEP_FILE_COUNT]}"

    if [[ "${source_mode}" == "git-head" ]] \
        && [[ "${source_record[MANYFORGE_FRONTEND_DIRTY_FILE_COUNT]}" != "0" ]]; then
        printf '  (ignoring %s dirty/untracked frontend file(s); source is committed HEAD)\n' \
            "${source_record[MANYFORGE_FRONTEND_DIRTY_FILE_COUNT]}"
        printf '  (set MANYFORGE_FRONTEND_SOURCE_MODE=worktree for an explicit dirty build)\n'
    fi

    local node_version=""
    local npm_version=""
    local os_name=""
    local architecture=""
    _manyforge_frontend_tool_identity \
        "${cache_root}" node_version node --version || return 1
    _manyforge_frontend_tool_identity \
        "${cache_root}" npm_version npm --version || return 1
    _manyforge_frontend_tool_identity \
        "${cache_root}" os_name uname -s || return 1
    _manyforge_frontend_tool_identity \
        "${cache_root}" architecture uname -m || return 1
    if (( ${#node_version} > 256 || ${#npm_version} > 256 \
        || ${#os_name} > 256 || ${#architecture} > 256 )); then
        printf 'ERROR: frontend toolchain identity exceeds supported bounds\n' >&2
        return 1
    fi
    local toolchain_key=""
    toolchain_key="$({
        printf 'cache_version=%s\n' "${_MANYFORGE_FRONTEND_CACHE_VERSION}"
        printf 'node=%s\n' "${node_version}"
        printf 'npm=%s\n' "${npm_version}"
        printf 'os=%s\n' "${os_name}"
        printf 'arch=%s\n' "${architecture}"
    } | sha256sum | awk '{print $1}')" || return 1
    local dependency_cache_key=""
    dependency_cache_key="$({
        printf 'dependency_hash=%s\n' "${dependency_hash}"
        printf 'toolchain_key=%s\n' "${toolchain_key}"
    } | sha256sum | awk '{print $1}')" || return 1
    [[ "${toolchain_key}" =~ ^[0-9a-f]{64}$ \
        && "${dependency_cache_key}" =~ ^[0-9a-f]{64}$ ]] || return 1

    local -A manifest=()
    local manifest_state=1
    if _manyforge_frontend_load_manifest "${manifest_path}" manifest; then
        manifest_state=0
    else
        manifest_state=$?
    fi
    if (( manifest_state == 2 )); then
        printf 'ERROR: frontend cache manifest is unsafe or malformed\n' >&2
        return 1
    fi

    local force_rebuild="${MANYFORGE_FRONTEND_FORCE_REBUILD:-0}"
    if [[ "${force_rebuild}" != "0" && "${force_rebuild}" != "1" ]]; then
        printf 'ERROR: MANYFORGE_FRONTEND_FORCE_REBUILD must be 0 or 1\n' >&2
        return 1
    fi
    if (( manifest_state == 0 )) \
        && [[ "${manifest[cache_version]}" == "${_MANYFORGE_FRONTEND_CACHE_VERSION}" ]] \
        && [[ "${manifest[source_hash]}" =~ ^[0-9a-f]{64}$ ]] \
        && [[ "${manifest[dependency_hash]}" =~ ^[0-9a-f]{64}$ ]] \
        && [[ "${manifest[dist_hash]}" =~ ^[0-9a-f]{64}$ ]] \
        && [[ "${manifest[toolchain_key]}" =~ ^[0-9a-f]{64}$ ]] \
        && [[ "${manifest[dependency_cache_key]}" =~ ^[0-9a-f]{64}$ ]] \
        && [[ "${manifest[static_dir]}" == "${static_dir}" ]] \
        && [[ "${force_rebuild}" == "0" ]] \
        && [[ "${manifest[source_hash]}" == "${source_hash}" ]] \
        && [[ "${manifest[dependency_hash]}" == "${dependency_hash}" ]] \
        && [[ "${manifest[toolchain_key]}" == "${toolchain_key}" ]] \
        && [[ -d "${static_dir}" && ! -L "${static_dir}" ]] \
        && [[ -f "${static_dir}/index.html" && ! -L "${static_dir}/index.html" ]]; then
        local -A cached_tree=()
        if _manyforge_frontend_load_record \
            "${hash_script}" tree "${static_dir}" "${cache_root}" \
            cached_tree "" "${hash_script_identity}" \
            && [[ "${cached_tree[MANYFORGE_FRONTEND_TREE_HASH]}" == "${manifest[dist_hash]}" ]]; then
            output_static_dir="${static_dir}"
            printf '  (frontend cache hit: %s; dist %s)\n' \
                "${source_hash}" "${cached_tree[MANYFORGE_FRONTEND_TREE_HASH]}"
            return 0
        fi
        printf '  (cached frontend dist is invalid; rebuilding)\n'
    fi

    local temporary_workspace=""
    local build_log=""
    local temporary_manifest=""
    temporary_workspace="$(mktemp -d "${cache_root}/workspace.next.XXXXXXXX")" \
        || return 1
    build_log="$(mktemp "${cache_root}/build-log.XXXXXXXX")" \
        || { rm -rf -- "${temporary_workspace}"; return 1; }

    local copy_status=0
    if [[ "${source_mode}" == "git-head" ]]; then
        if ! (
            set -o pipefail
            timeout "${_MANYFORGE_FRONTEND_COPY_TIMEOUT_SECONDS}" \
                env -i \
                PATH="${PATH}" \
                LC_ALL=C \
                GIT_CONFIG_NOSYSTEM=1 \
                GIT_CONFIG_GLOBAL=/dev/null \
                GIT_OPTIONAL_LOCKS=0 \
                GIT_PAGER=cat \
                GIT_TERMINAL_PROMPT=0 \
                git \
                -c core.fsmonitor=false \
                -c core.untrackedCache=false \
                -c core.hooksPath=/dev/null \
                -c core.attributesFile=/dev/null \
                -c diff.external= \
                -c "safe.directory=${source_record[MANYFORGE_FRONTEND_GIT_ROOT]}" \
                -C "${source_record[MANYFORGE_FRONTEND_GIT_ROOT]}" \
                archive --format=tar \
                "${source_record[MANYFORGE_FRONTEND_GIT_TREE_HASH]}" \
                | timeout "${_MANYFORGE_FRONTEND_COPY_TIMEOUT_SECONDS}" \
                    tar --no-same-owner --no-same-permissions \
                    -C "${temporary_workspace}" -xf -
        ); then
            copy_status=1
        fi
    else
        if ! (
            set -o pipefail
            timeout "${_MANYFORGE_FRONTEND_COPY_TIMEOUT_SECONDS}" \
                python3 "${hash_script}" files "${source_dir}" \
                | timeout "${_MANYFORGE_FRONTEND_COPY_TIMEOUT_SECONDS}" \
                    tar -C "${source_dir}" --null --verbatim-files-from \
                    --no-recursion -T - -cf - \
                | timeout "${_MANYFORGE_FRONTEND_COPY_TIMEOUT_SECONDS}" \
                    tar --no-same-owner --no-same-permissions \
                    -C "${temporary_workspace}" -xf -
        ); then
            copy_status=1
        fi
        local -A source_after_copy=()
        if (( copy_status == 0 )) \
            && { ! _manyforge_frontend_load_record \
                "${hash_script}" source "${source_dir}" "${cache_root}" \
                source_after_copy "" "${hash_script_identity}" \
                || [[ "${source_after_copy[MANYFORGE_FRONTEND_SOURCE_HASH]}" != "${source_hash}" ]] \
                || [[ "${source_after_copy[MANYFORGE_FRONTEND_DEP_HASH]}" != "${dependency_hash}" ]] \
                || [[ "${source_after_copy[MANYFORGE_FRONTEND_SOURCE_FILE_COUNT]}" != "${source_count}" ]]; }; then
            copy_status=1
            printf 'ERROR: frontend worktree changed while snapshotting\n' >&2
        fi
    fi
    if (( copy_status != 0 )); then
        printf 'ERROR: failed to create bounded frontend source snapshot\n' >&2
        rm -f -- "${build_log}"
        rm -rf -- "${temporary_workspace}"
        return 1
    fi

    # The fingerprint/snapshot contract excludes generated dependency/build
    # directories in both modes. Git tree identity remains conservative and
    # still changes if such a path was accidentally committed.
    rm -rf -- \
        "${temporary_workspace}/.git" \
        "${temporary_workspace}/.pytest_cache" \
        "${temporary_workspace}/.vite" \
        "${temporary_workspace}/dist" \
        "${temporary_workspace}/dist-ssr" \
        "${temporary_workspace}/node_modules"

    local -A snapshot_record=()
    if ! _manyforge_frontend_load_record \
        "${hash_script}" source "${temporary_workspace}" "${cache_root}" \
        snapshot_record worktree "${hash_script_identity}"; then
        printf 'ERROR: copied frontend snapshot failed bounded validation\n' >&2
        rm -f -- "${build_log}"
        rm -rf -- "${temporary_workspace}"
        return 1
    fi
    if [[ "${snapshot_record[MANYFORGE_FRONTEND_SOURCE_FILE_COUNT]}" != "${source_count}" ]] \
        || [[ "${snapshot_record[MANYFORGE_FRONTEND_DEP_HASH]}" != "${dependency_hash}" ]] \
        || [[ "${snapshot_record[MANYFORGE_FRONTEND_SOURCE_HASH]}" \
            != "${source_record[MANYFORGE_FRONTEND_SNAPSHOT_HASH]}" ]]; then
        printf 'ERROR: copied frontend snapshot does not match its source identity\n' >&2
        rm -f -- "${build_log}"
        rm -rf -- "${temporary_workspace}"
        return 1
    fi

    local dependency_stamp="${workspace_dir}/.manyforge-deps.hash"
    local keep_node_modules=0
    if (( manifest_state == 0 )) \
        && [[ "${manifest[dependency_hash]}" == "${dependency_hash}" ]] \
        && [[ "${manifest[toolchain_key]}" == "${toolchain_key}" ]] \
        && [[ -d "${workspace_dir}/node_modules" \
            && ! -L "${workspace_dir}/node_modules" ]] \
        && [[ -f "${dependency_stamp}" && ! -L "${dependency_stamp}" ]] \
        && [[ "$(stat -c '%s' -- "${dependency_stamp}")" =~ ^[0-9]+$ ]] \
        && (( $(stat -c '%s' -- "${dependency_stamp}") <= 128 )) \
        && [[ "$(<"${dependency_stamp}")" == "${dependency_cache_key}" ]]; then
        keep_node_modules=1
        ln -s -- "${workspace_dir}/node_modules" \
            "${temporary_workspace}/node_modules" || keep_node_modules=0
    fi
    rm -f -- "${temporary_workspace}/.manyforge-deps.hash"
    if (( keep_node_modules == 1 )); then
        printf '%s\n' "${dependency_cache_key}" \
            >"${temporary_workspace}/.manyforge-deps.hash"
    fi

    if ! (
        set -euo pipefail
        ulimit -f "${_MANYFORGE_FRONTEND_MAX_BUILD_LOG_BLOCKS}"
        cd "${temporary_workspace}"
        if [[ ! -d node_modules ]] \
            || [[ ! -f .manyforge-deps.hash ]] \
            || [[ "$(<.manyforge-deps.hash)" != "${dependency_cache_key}" ]]; then
            timeout --signal=TERM --kill-after=10s \
                "${_MANYFORGE_FRONTEND_NPM_TIMEOUT_SECONDS}" \
                npm ci --cache "${cache_root}/npm-cache" \
                    --prefer-offline --no-audit --no-fund
            printf '%s\n' "${dependency_cache_key}" >.manyforge-deps.hash
        fi
        rm -rf -- dist
        timeout --signal=TERM --kill-after=10s \
            "${_MANYFORGE_FRONTEND_NPM_TIMEOUT_SECONDS}" npm run build
    ) >"${build_log}" 2>&1; then
        printf 'ERROR: frontend build failed for source hash %s\n' "${source_hash}" >&2
        tail -n 80 -- "${build_log}" >&2 || true
        rm -f -- "${build_log}"
        rm -rf -- "${temporary_workspace}"
        return 1
    fi
    rm -f -- "${build_log}"

    if [[ ! -f "${temporary_workspace}/dist/index.html" \
        || -L "${temporary_workspace}/dist/index.html" ]]; then
        printf 'ERROR: frontend build completed without a regular dist/index.html\n' >&2
        rm -rf -- "${temporary_workspace}"
        return 1
    fi
    local -A built_tree=()
    if ! _manyforge_frontend_load_record \
        "${hash_script}" tree "${temporary_workspace}/dist" \
        "${cache_root}" built_tree "" "${hash_script_identity}"; then
        printf 'ERROR: failed to fingerprint bounded frontend dist\n' >&2
        rm -rf -- "${temporary_workspace}"
        return 1
    fi

    temporary_manifest="$(mktemp "${cache_root}/manifest.next.XXXXXXXX")" \
        || { rm -rf -- "${temporary_workspace}"; return 1; }
    {
        printf 'cache_version=%s\n' "${_MANYFORGE_FRONTEND_CACHE_VERSION}"
        printf 'source_hash=%s\n' "${source_hash}"
        printf 'source_file_count=%s\n' "${source_count}"
        printf 'dependency_hash=%s\n' "${dependency_hash}"
        printf 'dependency_file_count=%s\n' "${dependency_count}"
        printf 'toolchain_key=%s\n' "${toolchain_key}"
        printf 'dependency_cache_key=%s\n' "${dependency_cache_key}"
        printf 'dist_hash=%s\n' "${built_tree[MANYFORGE_FRONTEND_TREE_HASH]}"
        printf 'dist_file_count=%s\n' "${built_tree[MANYFORGE_FRONTEND_TREE_FILE_COUNT]}"
        printf 'static_dir=%s\n' "${static_dir}"
        printf 'built_at_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } >"${temporary_manifest}" || {
        rm -f -- "${temporary_manifest}"
        rm -rf -- "${temporary_workspace}"
        return 1
    }

    if (( keep_node_modules == 1 )); then
        if [[ ! -L "${temporary_workspace}/node_modules" ]] \
            || [[ ! -d "${workspace_dir}/node_modules" \
                || -L "${workspace_dir}/node_modules" ]]; then
            rm -f -- "${temporary_manifest}"
            rm -rf -- "${temporary_workspace}"
            return 1
        fi
        rm -- "${temporary_workspace}/node_modules"
        mv -- "${workspace_dir}/node_modules" \
            "${temporary_workspace}/node_modules" || {
            rm -f -- "${temporary_manifest}"
            rm -rf -- "${temporary_workspace}"
            return 1
        }
    fi

    if [[ -e "${workspace_dir}" || -L "${workspace_dir}" ]]; then
        if [[ ! -d "${workspace_dir}" || -L "${workspace_dir}" ]]; then
            printf 'ERROR: frontend cache workspace is not a safe directory\n' >&2
            rm -f -- "${temporary_manifest}"
            rm -rf -- "${temporary_workspace}"
            return 1
        fi
        rm -rf -- "${workspace_dir}" || return 1
    fi
    mv -- "${temporary_workspace}" "${workspace_dir}" || {
        rm -f -- "${temporary_manifest}"
        return 1
    }
    mv -f -- "${temporary_manifest}" "${manifest_path}" || return 1

    output_static_dir="${static_dir}"
    printf '  (frontend dist ready at %s; source %s; dist %s)\n' \
        "${static_dir}" "${source_hash}" \
        "${built_tree[MANYFORGE_FRONTEND_TREE_HASH]}"
}

manyforge_build_frontend_dist() {
    if (( $# > 2 )); then
        printf 'ERROR: manyforge_build_frontend_dist expects [SOURCE_DIR] [BUILD_ROOT]\n' >&2
        return 2
    fi
    local source_dir="${1:-/workspace/manyforge_composer/frontend}"
    local build_root="${2:-${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}}"
    if ! _manyforge_frontend_safe_path_text "${source_dir}" \
        || ! _manyforge_frontend_safe_path_text "${build_root}"; then
        printf 'ERROR: frontend source/build paths must be bounded absolute paths\n' >&2
        return 2
    fi
    if [[ -L "${source_dir}" || ! -d "${source_dir}" ]]; then
        printf 'ERROR: frontend source must be an existing non-symlink directory\n' >&2
        return 1
    fi
    source_dir="$(realpath -e -- "${source_dir}")" || return 1

    if [[ ! -e "${build_root}" && ! -L "${build_root}" ]]; then
        local build_parent="${build_root%/*}"
        local build_name="${build_root##*/}"
        if [[ ! -d "${build_parent}" || -L "${build_parent}" ]] \
            || [[ ! "${build_name}" =~ ^[A-Za-z0-9._-]{1,128}$ ]] \
            || [[ "${build_name}" == "." || "${build_name}" == ".." ]]; then
            printf 'ERROR: frontend build root parent/name is unsafe\n' >&2
            return 1
        fi
        (umask 077; mkdir -- "${build_root}") || return 1
    fi
    if [[ -L "${build_root}" || ! -d "${build_root}" ]]; then
        printf 'ERROR: frontend build root must be a non-symlink directory\n' >&2
        return 1
    fi
    build_root="$(realpath -e -- "${build_root}")" || return 1

    local helper_dir=""
    helper_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" || return 1
    local hash_script="${helper_dir}/frontend_hash.py"
    if [[ ! -f "${hash_script}" || -L "${hash_script}" ]]; then
        printf 'ERROR: fixed frontend hash helper is unavailable\n' >&2
        return 1
    fi
    local tool
    for tool in python3 git node npm sha256sum awk tar flock uname cat \
        realpath stat timeout mktemp date env id; do
        if ! command -v "${tool}" >/dev/null 2>&1; then
            printf 'ERROR: required frontend build tool not found: %s\n' "${tool}" >&2
            return 1
        fi
    done
    local hash_script_identity=""
    if ! _manyforge_frontend_hash_script_identity \
        "${hash_script}" hash_script_identity; then
        printf 'ERROR: fixed frontend hash helper identity is unstable\n' >&2
        return 1
    fi

    local cache_root="${build_root}/frontend-cache"
    if [[ ! -e "${cache_root}" && ! -L "${cache_root}" ]]; then
        (umask 077; mkdir -- "${cache_root}") || return 1
    fi
    if [[ -L "${cache_root}" || ! -d "${cache_root}" ]]; then
        printf 'ERROR: frontend cache root must be a non-symlink directory\n' >&2
        return 1
    fi
    local cache_owner=""
    local current_owner=""
    local cache_mode=""
    cache_owner="$(stat -c '%u' -- "${cache_root}")" || return 1
    current_owner="$(id -u)" || return 1
    cache_mode="$(stat -c '%a' -- "${cache_root}")" || return 1
    if [[ "${cache_owner}" != "${current_owner}" ]] \
        || [[ ! "${cache_mode}" =~ ^[0-7]{3,4}$ ]] \
        || (( (8#${cache_mode} & 0022) != 0 )); then
        printf 'ERROR: frontend cache root must be current-owner and not group/world-writable\n' >&2
        return 1
    fi

    local workspace_dir="${cache_root}/workspace"
    local manifest_path="${cache_root}/manifest.env"
    local static_dir="${workspace_dir}/dist"
    local lock_fd=""
    exec {lock_fd}<"${cache_root}" || return 1
    if ! flock -x "${lock_fd}"; then
        exec {lock_fd}>&-
        printf 'ERROR: failed to acquire frontend cache lock\n' >&2
        return 1
    fi

    printf 'Building frontend (manyforge_composer/frontend)...\n'
    local resolved_static_dir=""
    local build_status=0
    _manyforge_frontend_locked_build \
        "${source_dir}" "${cache_root}" "${workspace_dir}" \
        "${manifest_path}" "${hash_script}" "${static_dir}" \
        resolved_static_dir "${hash_script_identity}" || build_status=$?
    flock -u "${lock_fd}" || build_status=1
    exec {lock_fd}>&-
    if (( build_status != 0 )); then
        return "${build_status}"
    fi
    MANYFORGE_FRONTEND_STATIC_DIR="${resolved_static_dir}"
    export MANYFORGE_FRONTEND_STATIC_DIR
}
