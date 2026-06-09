# shellcheck shell=bash
# scripts/lib/wait.sh — progress-aware readiness wait helper.
#
# Single helper: wait_with_progress LABEL TIMEOUT_S HINT LOG_FN PROBE_FN
#
# Polls PROBE_FN every 1 s until it returns 0 or TIMEOUT_S elapses. While
# waiting, draws a live status block (TTY) or periodic reprints (non-TTY)
# that includes:
#   - the LABEL
#   - an optional HINT line (env-var override suggestion, etc.)
#   - elapsed seconds
#   - last 3 lines from LOG_FN (if non-empty)
#
# API contract — PROBE_FN and LOG_FN are **bash function names**, called
# directly via "$probe_fn" / "$log_fn". No eval, no command-string
# injection risk.
#
#   PROBE_FN: must be defined; returns 0 when ready, non-zero when not.
#   LOG_FN:   optional — pass "" to skip log tailing. Otherwise a bash
#             function that prints recent log lines to stdout.
#
# Lifted from scripts/demo-assistant-known-good.sh:wait_json as it
# existed at commit ab9af95^ (the original is gone — current file is a
# shim). API adapted from URL/container string-eval to function names
# per audit recommendation. TTY detection uses [[ -t 2 ]] (stderr) so
# shims passing --non-interactive --yes still get the live display
# when the human operator is at a terminal.

if [[ -n "${_MANYFORGE_LIB_WAIT:-}" ]]; then return 0; fi
_MANYFORGE_LIB_WAIT=1

source "$(dirname "${BASH_SOURCE[0]}")/runner.sh"

# print_truncated VALUE  — truncate to fit terminal width, escape CRs.
# Used by the wait helper to keep log lines from wrapping in the live
# redraw. Lifted from the original wait_json source.
print_truncated() {
    local value="${1//$'\r'/ }"
    local columns="${COLUMNS:-120}"
    if ! [[ "${columns}" =~ ^[0-9]+$ ]]; then
        columns=120
    fi
    local max=$((columns - 6))
    if (( max < 40 )); then
        max=40
    fi
    printf '%.*s' "${max}" "${value}"
}

# _wait_progress_is_tty — TTY decision for the wait helper.
#
# MANYFORGE_PROGRESS_MODE=plain forces non-TTY codepath (no ANSI, periodic
# reprints) even when stderr is a real terminal. Useful for CI and for
# capturing clean logs.
#
# Default ("auto" or unset) uses `[[ -t 2 ]]`. NOT is_non_interactive() —
# the shims always pass --non-interactive --yes which would suppress the
# live display even when the human operator is watching.
_wait_progress_is_tty() {
    case "${MANYFORGE_PROGRESS_MODE:-auto}" in
        plain) return 1 ;;
        *)     [[ -t 2 ]] ;;
    esac
}

# wait_with_progress LABEL TIMEOUT_S HINT LOG_FN PROBE_FN
#
# Returns 0 on success (PROBE_FN returned 0 within TIMEOUT_S),
# 1 on timeout. Status output goes to stderr; PROBE_FN / LOG_FN output
# is captured.
wait_with_progress() {
    local label="$1"
    local timeout_s="$2"
    local hint="$3"
    local log_fn="$4"
    local probe_fn="$5"

    if [[ -z "${probe_fn}" ]]; then
        fail "wait_with_progress: PROBE_FN is required"
        return 2
    fi
    if ! declare -F "${probe_fn}" >/dev/null 2>&1; then
        fail "wait_with_progress: probe function '${probe_fn}' is not defined"
        return 2
    fi
    if [[ -n "${log_fn}" ]] && ! declare -F "${log_fn}" >/dev/null 2>&1; then
        fail "wait_with_progress: log function '${log_fn}' is not defined"
        return 2
    fi

    local start=$SECONDS
    local deadline=$((start + timeout_s))
    local next_log_at=0
    local elapsed progress_line_drawn=0
    # Capture the most recent probe stdout/stderr so the timeout dump can
    # surface what the probe actually said (HTTP body, ros2 error, etc.).
    # Each poll overwrites the file; on success we delete it; on timeout
    # _wait_dump_failure tails it.
    local probe_output_file
    probe_output_file="$(mktemp -t manyforge-wait-probe-XXXXXX)"

    # Static header — printed ONCE. The label and hint do not change while
    # waiting, so re-printing them every tick is pure noise. Progress below
    # updates a single line in place (TTY) or a sparse heartbeat (non-TTY).
    printf '%s\n' "${label}" >&2
    if [[ -n "${hint}" ]]; then
        printf '  %s\n' "${hint}" >&2
    fi

    # _wait_render — update the live progress indicator.
    #
    # TTY: a single carriage-return line ("\r" + erase-to-EOL), so it
    # overwrites itself in place rather than scrolling. Avoids the old
    # multi-line cursor-up redraw, which corrupted (and visibly spammed) on
    # terminals where a wrapped hint line threw off the line count.
    #
    # Non-TTY (or MANYFORGE_PROGRESS_MODE=plain): a one-line heartbeat every
    # 30 s so CI logs show liveness without flooding.
    _wait_render() {
        local current_elapsed="$1"
        local snippet=""
        if [[ -n "${log_fn}" ]]; then
            snippet="$("${log_fn}" 2>&1 | tail -n 1 || true)"
        fi

        if _wait_progress_is_tty; then
            printf '\r\033[K  elapsed %ds (timeout %ds)' "${current_elapsed}" "${timeout_s}" >&2
            if [[ -n "${snippet}" ]]; then
                printf ' — ' >&2
                print_truncated "${snippet}" >&2
            fi
            progress_line_drawn=1
            return
        fi

        if (( current_elapsed < next_log_at )); then
            return
        fi
        printf '  …still waiting (%ds elapsed, timeout %ds)\n' "${current_elapsed}" "${timeout_s}" >&2
        next_log_at=$((current_elapsed + 30))
    }

    # _wait_end_progress_line — terminate the in-place TTY line cleanly
    # before printing a final pass/fail/dump on its own line.
    _wait_end_progress_line() {
        if _wait_progress_is_tty && (( progress_line_drawn > 0 )); then
            printf '\r\033[K' >&2
            progress_line_drawn=0
        fi
    }

    _wait_dump_failure() {
        # Final state dump on timeout. Always to stderr.
        _wait_end_progress_line
        elapsed=$((SECONDS - start))
        fail "${label} — timed out after ${elapsed}s (limit ${timeout_s}s)"
        if [[ -n "${hint}" ]]; then
            info "${hint}"
        fi
        # Last probe stdout/stderr — what the probe actually returned on
        # the final attempt. Often more diagnostic than the log tail.
        if [[ -s "${probe_output_file}" ]]; then
            info "last probe (${probe_fn}) stdout/stderr:"
            while IFS= read -r line; do
                printf '    ' >&2
                print_truncated "${line}" >&2
                printf '\n' >&2
            done < "${probe_output_file}"
        fi
        if [[ -n "${log_fn}" ]]; then
            info "last 40 lines from ${log_fn}:"
            "${log_fn}" 2>&1 | tail -n 40 | while IFS= read -r line; do
                printf '    ' >&2
                print_truncated "${line}" >&2
                printf '\n' >&2
            done
        fi
    }

    # Main poll loop. Probe stdout/stderr → probe_output_file (overwritten
    # each tick), so on timeout the dump shows the final attempt's output.
    until "${probe_fn}" >"${probe_output_file}" 2>&1; do
        if (( SECONDS >= deadline )); then
            _wait_dump_failure
            rm -f "${probe_output_file}"
            return 1
        fi
        elapsed=$((SECONDS - start))
        _wait_render "${elapsed}"
        sleep 1
    done

    elapsed=$((SECONDS - start))
    rm -f "${probe_output_file}"
    _wait_end_progress_line
    pass "${label} — ready after ${elapsed}s"
    return 0
}
