#!/usr/bin/env bash
# vllm-status-watch.sh — out-of-band liveness/crash watcher for the model-
# serving container. Writes an atomic JSON status file the Composer backend
# reads so it can distinguish "loading" from "crashed/failed". The HTTP probe
# alone cannot: a still-booting container and a dead container both refuse
# connections, which is why the UI otherwise sits on "waiting" forever after a
# launch crash.
#
# Lives in the stack (the reference interface), not in any launcher: it observes
# the container by name and depends on no launch-script internals, so it
# survives a launcher swap. The stack starts/stops it (compose.sh).
#
# Pure sidecar: read-only docker calls, out of the inference path. If it dies or
# misbehaves, serving is unaffected and the backend falls back to its existing
# HTTP-probe readiness (the status file just goes stale -> "unknown").
#
# Design constraints handled here:
#   * `docker run --rm`: a crashed container is auto-removed, so inspecting it
#     after exit returns nothing. We capture exitCode/OOM live via `docker
#     events` before removal, with a "vanished while running" fallback.
#   * Stop paths are scattered across several scripts, so intent is inferred
#     from exit code + OOM (+ an optional sentinel override) rather than
#     requiring every stop path to cooperate.
#   * Fail-closed: any genuine uncertainty -> state "unknown", never a false
#     "ready"/"failed". Every write stamps `updatedAt`; a stale file lets the
#     reader detect a dead watcher instead of trusting a frozen "running".
#
# Usage: vllm-status-watch.sh <container> [status-file] [sentinel-file]
set -uo pipefail

CONTAINER="${1:?container name required}"
STATUS_FILE="${2:-/tmp/manyforge-assistant-e2e/vllm-status.json}"
SENTINEL_FILE="${3:-/tmp/manyforge-assistant-e2e/vllm-intended-stop}"
POLL_INTERVAL="${VLLM_WATCH_INTERVAL:-2}"
# Override to "sudo -n docker" when the watcher's user needs sudo for docker.
DOCKER="${VLLM_WATCH_DOCKER:-docker}"
SCHEMA="manyforge.serving.vllm_status.v1"

STATE_DIR="$(dirname "${STATUS_FILE}")"
mkdir -p "${STATE_DIR}" 2>/dev/null || true
PIDFILE="${STATE_DIR}/vllm-status-watch.pid"
EVENT_EXIT_FILE="${STATE_DIR}/.vllm-watch-exitcode.$$"
EVENT_OOM_FILE="${STATE_DIR}/.vllm-watch-oom.$$"

now_ms() { date +%s%3N 2>/dev/null || echo "$(( $(date +%s) * 1000 ))"; }

# --- singleton: replace any prior watcher for this status file --------------
if [[ -f "${PIDFILE}" ]]; then
    oldpid="$(cat "${PIDFILE}" 2>/dev/null || true)"
    if [[ "${oldpid}" =~ ^[0-9]+$ ]] && kill -0 "${oldpid}" 2>/dev/null; then
        kill "${oldpid}" 2>/dev/null || true
        sleep 0.3
    fi
fi
echo "$$" > "${PIDFILE}"

EVENTS_EXIT_PID=""
EVENTS_OOM_PID=""
cleanup() {
    [[ -n "${EVENTS_EXIT_PID}" ]] && kill "${EVENTS_EXIT_PID}" 2>/dev/null || true
    [[ -n "${EVENTS_OOM_PID}" ]] && kill "${EVENTS_OOM_PID}" 2>/dev/null || true
    rm -f "${EVENT_EXIT_FILE}" "${EVENT_OOM_FILE}" 2>/dev/null || true
    [[ "$(cat "${PIDFILE}" 2>/dev/null)" == "$$" ]] && rm -f "${PIDFILE}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# --- atomic JSON writer (temp + rename) ------------------------------------
write_status() {
    local state="$1" phase="$2" exit_code="$3" oom="$4" reason="$5"
    local tmp="${STATUS_FILE}.tmp.$$"
    cat > "${tmp}" <<JSON
{
  "schema": "${SCHEMA}",
  "container": "${CONTAINER}",
  "state": "${state}",
  "phase": "${phase}",
  "exitCode": ${exit_code},
  "oomKilled": ${oom},
  "reason": "${reason}",
  "updatedAt": $(now_ms),
  "pollIntervalMs": $(( POLL_INTERVAL * 1000 ))
}
JSON
    mv -f "${tmp}" "${STATUS_FILE}" 2>/dev/null || rm -f "${tmp}" 2>/dev/null
}

# --- live death capture (wins the race against --rm cleanup) ----------------
( ${DOCKER} events --filter "container=${CONTAINER}" --filter "event=die" \
      --format '{{.Actor.Attributes.exitCode}}' 2>/dev/null \
      | head -n1 > "${EVENT_EXIT_FILE}" ) &
EVENTS_EXIT_PID=$!
( ${DOCKER} events --filter "container=${CONTAINER}" --filter "event=oom" \
      --format 'oom' 2>/dev/null \
      | head -n1 > "${EVENT_OOM_FILE}" ) &
EVENTS_OOM_PID=$!

classify_and_finalize() {
    local code="$1" oom="$2"
    if [[ -f "${SENTINEL_FILE}" ]]; then
        write_status "stopped" "stopped" "${code:-0}" "${oom:-false}" "intended stop (sentinel present)"
        return
    fi
    if [[ "${oom}" == "true" ]]; then
        write_status "failed" "crashed" "${code:-137}" "true" "out-of-memory (OOM-killed)"
        return
    fi
    case "${code}" in
        0|143)
            write_status "stopped" "stopped" "${code}" "false" "graceful exit (code ${code})" ;;
        137)
            write_status "stopped" "stopped" "137" "false" "killed (SIGKILL — likely intended teardown)" ;;
        ""|null)
            write_status "failed" "crashed" "-1" "false" "exited unexpectedly (no exit code captured)" ;;
        *)
            write_status "failed" "crashed" "${code}" "false" "non-zero exit (code ${code})" ;;
    esac
}

read_event_exit() { [[ -s "${EVENT_EXIT_FILE}" ]] && cat "${EVENT_EXIT_FILE}" 2>/dev/null | tr -dc '0-9-' ; }
read_event_oom()  { [[ -s "${EVENT_OOM_FILE}"  ]] && echo "true" || echo "false" ; }

# --- main poll loop --------------------------------------------------------
# "absent" = no serving container observed yet (e.g. composer-only deployment);
# the backend omits the component for this state so it isn't perpetual noise.
write_status "absent" "absent" "null" "false" "watcher started; no serving container yet"
seen_running=0

while true; do
    insp="$(${DOCKER} inspect -f '{{.State.Status}}|{{.State.OOMKilled}}|{{.State.ExitCode}}' "${CONTAINER}" 2>/dev/null)"
    if [[ -n "${insp}" ]]; then
        IFS='|' read -r dstate doom dcode <<< "${insp}"
        case "${dstate}" in
            running)
                seen_running=1
                write_status "running" "running" "null" "false" "container running" ;;
            created|restarting)
                write_status "starting" "starting" "null" "false" "container ${dstate}" ;;
            paused)
                write_status "unknown" "paused" "null" "false" "container paused" ;;
            exited|dead)
                classify_and_finalize "${dcode}" "${doom}"; break ;;
            *)
                write_status "unknown" "unknown" "null" "false" "docker state '${dstate}'" ;;
        esac
    else
        if [[ -f "${SENTINEL_FILE}" ]]; then
            write_status "stopped" "stopped" "0" "false" "intended stop (sentinel present)"; break
        fi
        ev_code="$(read_event_exit)"; ev_oom="$(read_event_oom)"
        if [[ -n "${ev_code}" || "${ev_oom}" == "true" ]]; then
            classify_and_finalize "${ev_code}" "${ev_oom}"; break
        fi
        if [[ "${seen_running}" == "1" ]]; then
            write_status "failed" "crashed" "-1" "false" "container vanished while running (crash + auto-remove)"; break
        fi
        write_status "absent" "absent" "null" "false" "no serving container present"
    fi
    sleep "${POLL_INTERVAL}"
done
