#!/usr/bin/env bash
# Repeat-under-load stability gate for the execution-kernel suite.
#
# This is the codified version of the loaded loop that exposed the 2026-07-06
# teardown/lifecycle races. It intentionally runs outside ctest so a failing
# iteration preserves the exact log/backtrace artifact.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${CORE_BUILD_DIR:-${MANYFORGE_CORE_BUILD_DIR:-/tmp/manyforge-build/core}}"
ITERATIONS="${MANYFORGE_STABILITY_ITERATIONS:-30}"
LOAD_WORKERS="${MANYFORGE_STABILITY_LOAD_WORKERS:-3}"
TIMEOUT_SECONDS="${MANYFORGE_STABILITY_TIMEOUT_SECONDS:-30}"
LOG_DIR="${MANYFORGE_STABILITY_LOG_DIR:-${TMPDIR:-/tmp}/manyforge-stability-gate-$(date +%Y%m%d-%H%M%S)}"
TEST_BINARY="${MANYFORGE_STABILITY_TEST_BINARY:-${BUILD_DIR}/test_execution_kernel}"
MANIFEST_PATH="${MANYFORGE_STABILITY_MANIFEST:-${LOG_DIR}/stability-gate-manifest.json}"
RUN_ID="${MANYFORGE_STABILITY_RUN_ID:-}"
PROCESS_INSTANCE_ID="${MANYFORGE_STABILITY_PROCESS_INSTANCE_ID:-}"

mkdir -p "${LOG_DIR}"

if [[ ! "${ITERATIONS}" =~ ^[1-9][0-9]*$ ||
      ! "${LOAD_WORKERS}" =~ ^[1-9][0-9]*$ ||
      ! "${TIMEOUT_SECONDS}" =~ ^[1-9][0-9]*$ ]]; then
  echo "iterations, load workers, and timeout must be positive integers" >&2
  exit 2
fi
if [[ -z "${RUN_ID}" ]]; then
  RUN_ID="$(date -u +%Y%m%dT%H%M%S%NZ)-loaded-kernel"
fi
if [[ -z "${PROCESS_INSTANCE_ID}" ]]; then
  if [[ -r /proc/sys/kernel/random/uuid ]]; then
    PROCESS_INSTANCE_ID="$(< /proc/sys/kernel/random/uuid)"
  else
    echo "MANYFORGE_STABILITY_PROCESS_INSTANCE_ID is required without /proc UUID support" >&2
    exit 2
  fi
fi
if [[ ! "${PROCESS_INSTANCE_ID}" =~ ^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$ ]]; then
  echo "MANYFORGE_STABILITY_PROCESS_INSTANCE_ID must be a canonical UUID" >&2
  exit 2
fi

LOG_DIR="$(realpath -m "${LOG_DIR}")"
MANIFEST_PATH="$(realpath -m "${MANIFEST_PATH}")"
case "${MANIFEST_PATH}" in
  "${LOG_DIR}/"*) ;;
  *)
    echo "stability manifest must be inside the stability log directory" >&2
    exit 2
    ;;
esac
LEDGER_PATH="${LOG_DIR}/stability-gate-ledger.tsv"
printf 'iteration\tstatus\tduration_ns\tlog_name\tlog_sha256\tlog_bytes\tbefore_workers_json\tafter_workers_json\n' >"${LEDGER_PATH}"

load_pids=()
cleanup() {
  for pid in "${load_pids[@]:-}"; do
    kill "${pid}" 2>/dev/null || true
  done
  for pid in "${load_pids[@]:-}"; do
    wait "${pid}" 2>/dev/null || true
  done
}
trap cleanup EXIT INT TERM

if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  cmake --build "${BUILD_DIR}" --target test_execution_kernel \
    -j"${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"
fi

if [[ ! -x "${TEST_BINARY}" ]]; then
  echo "test_execution_kernel not found at ${TEST_BINARY}" >&2
  echo "Set CORE_BUILD_DIR or MANYFORGE_STABILITY_TEST_BINARY." >&2
  exit 2
fi

TEST_BINARY="$(realpath "${TEST_BINARY}")"
TEST_BINARY_SHA256_BEFORE="$(sha256sum -- "${TEST_BINARY}" | awk '{print $1}')"
GATE_SCRIPT_SHA256_BEFORE="$(sha256sum -- "${BASH_SOURCE[0]}" | awk '{print $1}')"
TEST_INVENTORY_PATH="${LOG_DIR}/test_execution_kernel.inventory.txt"
"${TEST_BINARY}" --gtest_list_tests >"${TEST_INVENTORY_PATH}"
[[ -s "${TEST_INVENTORY_PATH}" ]] || {
  echo "execution-kernel test inventory is empty" >&2
  exit 1
}

# GoogleTest accepts filters, repeats, and sharding through environment
# variables.  A loaded-loop qualification must exercise the complete binary
# on every iteration, independently of the invoking shell or image defaults.
while IFS='=' read -r key _; do
  case "${key}" in
    GTEST_*|TEST_PREMATURE_EXIT_FILE)
      unset "${key}"
      ;;
  esac
done < <(env)

# A retained loaded-kernel result must execute the freshly built binary itself.
# Do not inherit or install a preload shim: even a nominal diagnostics library
# can interpose GoogleTest or the code under test and make the run dependent on
# image-local state that is not part of the source candidate.
unset LD_PRELOAD SEGFAULT_SIGNALS

echo "manyforge stability gate: ${ITERATIONS} iterations, ${LOAD_WORKERS} load workers"
echo "logs: ${LOG_DIR}"

for _ in $(seq 1 "${LOAD_WORKERS}"); do
  bash -c 'while :; do :; done' &
  load_pids+=("$!")
done

# Capture process identity from /proc rather than treating kill -0 as proof of
# a useful load worker.  Reading stat on both sides of cmdline closes the PID
# reuse window inside each observation.  The verifier also requires the same
# pid/starttime/command tuple before and after every test iteration.
snapshot_load_workers() {
  local phase="$1"
  local iteration="$2"
  python3 - "${phase}" "${iteration}" "${load_pids[@]}" <<'PY'
import json
import sys
from pathlib import Path

phase = sys.argv[1]
iteration = int(sys.argv[2])
pids = [int(value) for value in sys.argv[3:]]
expected_argv = ["bash", "-c", "while :; do :; done"]
allowed_states = {"R", "S", "D", "I"}

def read_stat(pid: int) -> tuple[str, int]:
    text = (Path("/proc") / str(pid) / "stat").read_text(encoding="utf-8")
    close = text.rfind(")")
    if close < 0:
        raise RuntimeError("malformed /proc stat")
    if int(text[: text.find(" ")]) != pid:
        raise RuntimeError("/proc stat PID differs")
    fields = text[close + 2 :].split()
    if len(fields) < 20:
        raise RuntimeError("truncated /proc stat")
    return fields[0], int(fields[19])

records = []
try:
    for ordinal, pid in enumerate(pids, start=1):
        state_one, starttime_one = read_stat(pid)
        command_bytes = (Path("/proc") / str(pid) / "cmdline").read_bytes()
        state_two, starttime_two = read_stat(pid)
        command_argv = [
            value.decode("utf-8", errors="strict")
            for value in command_bytes.rstrip(b"\0").split(b"\0")
            if value
        ]
        if starttime_one != starttime_two:
            raise RuntimeError(f"worker {ordinal} PID was reused during observation")
        if state_one not in allowed_states or state_two not in allowed_states:
            raise RuntimeError(
                f"worker {ordinal} has unusable state {state_one}/{state_two}"
            )
        if command_argv != expected_argv:
            raise RuntimeError(
                f"worker {ordinal} command differs: {command_argv!r}"
            )
        records.append(
            {
                "ordinal": ordinal,
                "pid": pid,
                "proc_starttime_ticks": starttime_two,
                "state": state_two,
                "state_observations": [state_one, state_two],
                "command_argv": command_argv,
            }
        )
except (OSError, UnicodeError, ValueError, RuntimeError) as error:
    raise SystemExit(
        f"load worker evidence failed {phase} iteration {iteration}: {error}"
    ) from error

if len(records) != len(pids) or len({record["pid"] for record in records}) != len(pids):
    raise SystemExit(
        f"load worker evidence failed {phase} iteration {iteration}: duplicate/missing PID"
    )
print(json.dumps(records, sort_keys=True, separators=(",", ":")))
PY
}

for iteration in $(seq 1 "${ITERATIONS}"); do
  log="${LOG_DIR}/test_execution_kernel.${iteration}.log"
  echo "[${iteration}/${ITERATIONS}] ${TEST_BINARY}"
  before_workers_json="$(snapshot_load_workers before "${iteration}")"
  started_ns="$(date +%s%N)"
  set +e
  timeout -s ABRT "${TIMEOUT_SECONDS}s" \
    "${TEST_BINARY}" "--gtest_filter=*" --gtest_repeat=1 --gtest_brief=1 \
    >"${log}" 2>&1
  status=$?
  set -e
  finished_ns="$(date +%s%N)"
  duration_ns=$((finished_ns - started_ns))
  after_workers_json="$(snapshot_load_workers after "${iteration}")"
  log_sha256="$(sha256sum -- "${log}" | awk '{print $1}')"
  log_bytes="$(stat -c '%s' -- "${log}")"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "${iteration}" "${status}" "${duration_ns}" "$(basename "${log}")" \
    "${log_sha256}" "${log_bytes}" "${before_workers_json}" "${after_workers_json}" \
    >>"${LEDGER_PATH}"
  if [[ "${status}" -ne 0 ]]; then
    echo "stability gate failed at iteration ${iteration} (status ${status})" >&2
    echo "failing log: ${log}" >&2
    tail -n 200 "${log}" >&2 || true
    exit "${status}"
  fi
done


TEST_BINARY_SHA256_AFTER="$(sha256sum -- "${TEST_BINARY}" | awk '{print $1}')"
GATE_SCRIPT_SHA256_AFTER="$(sha256sum -- "${BASH_SOURCE[0]}" | awk '{print $1}')"
if [[ "${TEST_BINARY_SHA256_BEFORE}" != "${TEST_BINARY_SHA256_AFTER}" ||
      "${GATE_SCRIPT_SHA256_BEFORE}" != "${GATE_SCRIPT_SHA256_AFTER}" ]]; then
  echo "stability binary or gate script changed during the run" >&2
  exit 1
fi

python3 - "${MANIFEST_PATH}" "${LEDGER_PATH}" "${LOG_DIR}" \
  "${RUN_ID}" "${PROCESS_INSTANCE_ID}" "${ITERATIONS}" "${LOAD_WORKERS}" \
  "${TIMEOUT_SECONDS}" "${TEST_BINARY}" "${TEST_BINARY_SHA256_BEFORE}" \
  "${BASH_SOURCE[0]}" "${GATE_SCRIPT_SHA256_BEFORE}" \
  "${TEST_INVENTORY_PATH}" <<'PY'
import csv
import hashlib
import json
import os
import sys
import time
from pathlib import Path

(
    manifest_raw,
    ledger_raw,
    log_dir_raw,
    run_id,
    process_instance_id,
    iterations_raw,
    workers_raw,
    timeout_raw,
    binary_raw,
    binary_sha256,
    gate_raw,
    gate_sha256,
    inventory_raw,
) = sys.argv[1:]
manifest_path = Path(manifest_raw)
ledger_path = Path(ledger_raw)
log_dir = Path(log_dir_raw)
inventory_path = Path(inventory_raw)
iterations = int(iterations_raw)
workers = int(workers_raw)

def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

records = []
with ledger_path.open("r", encoding="utf-8", newline="") as stream:
    for row in csv.DictReader(stream, delimiter="\t"):
        log_path = log_dir / row["log_name"]
        before = json.loads(row["before_workers_json"])
        after = json.loads(row["after_workers_json"])
        record = {
            "iteration": int(row["iteration"]),
            "exit_status": int(row["status"]),
            "duration_ns": int(row["duration_ns"]),
            "log_name": row["log_name"],
            "log_sha256": row["log_sha256"],
            "log_bytes": int(row["log_bytes"]),
            "load_worker_evidence_before": before,
            "load_worker_evidence_after": after,
        }
        if sha256(log_path) != record["log_sha256"]:
            raise SystemExit(f"log changed while writing manifest: {log_path}")
        if log_path.stat().st_size != record["log_bytes"]:
            raise SystemExit(f"log size changed while writing manifest: {log_path}")
        records.append(record)

allowed_list = ""
for line in Path("/proc/self/status").read_text(encoding="utf-8").splitlines():
    if line.startswith("Cpus_allowed_list:"):
        allowed_list = line.split(":", 1)[1].strip()
        break

expected_ordinals = list(range(1, workers + 1))
expected_command = ["bash", "-c", "while :; do :; done"]
allowed_states = {"R", "S", "D", "I"}

def valid_worker_snapshot(value: object) -> bool:
    if not isinstance(value, list) or len(value) != workers:
        return False
    if [record.get("ordinal") for record in value if isinstance(record, dict)] != expected_ordinals:
        return False
    identities = set()
    for record in value:
        if not isinstance(record, dict) or set(record) != {
            "ordinal", "pid", "proc_starttime_ticks", "state",
            "state_observations", "command_argv",
        }:
            return False
        if (
            not isinstance(record["pid"], int)
            or record["pid"] <= 0
            or not isinstance(record["proc_starttime_ticks"], int)
            or record["proc_starttime_ticks"] <= 0
            or record["state"] not in allowed_states
            or not isinstance(record["state_observations"], list)
            or len(record["state_observations"]) != 2
            or any(state not in allowed_states for state in record["state_observations"])
            or record["command_argv"] != expected_command
        ):
            return False
        identities.add(
            (record["pid"], record["proc_starttime_ticks"], tuple(record["command_argv"]))
        )
    return len(identities) == workers

worker_snapshots = [
    record[phase]
    for record in records
    for phase in ("load_worker_evidence_before", "load_worker_evidence_after")
]
reference_worker_identities = (
    [
        (record["pid"], record["proc_starttime_ticks"], record["command_argv"])
        for record in worker_snapshots[0]
    ]
    if worker_snapshots
    else []
)
passed = (
    len(records) == iterations
    and [record["iteration"] for record in records] == list(range(1, iterations + 1))
    and all(record["exit_status"] == 0 for record in records)
    and all(record["duration_ns"] > 0 for record in records)
    and all(valid_worker_snapshot(snapshot) for snapshot in worker_snapshots)
    and all(
        [
            (record["pid"], record["proc_starttime_ticks"], record["command_argv"])
            for record in snapshot
        ] == reference_worker_identities
        for snapshot in worker_snapshots
    )
)
manifest = {
    "schema_version": "manyforge.loaded_kernel_stability_raw.v1",
    "generated_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "run_id": run_id,
    "process_instance_id": process_instance_id,
    "configuration": {
        "iterations": iterations,
        "load_workers": workers,
        "timeout_seconds": int(timeout_raw),
        "cpus_allowed_list": allowed_list,
    },
    "command_argv": [
        binary_raw,
        "--gtest_filter=*",
        "--gtest_repeat=1",
        "--gtest_brief=1",
    ],
    "test_binary": {"path": binary_raw, "sha256": binary_sha256},
    "gate_script": {"path": gate_raw, "sha256": gate_sha256},
    "test_inventory": {
        "name": inventory_path.name,
        "sha256": sha256(inventory_path),
        "bytes": inventory_path.stat().st_size,
    },
    "environment": {
        "ld_preload": os.environ.get("LD_PRELOAD", ""),
        "segfault_signals": os.environ.get("SEGFAULT_SIGNALS", ""),
        "gtest_overrides_present": sorted(
            key
            for key in os.environ
            if key.startswith("GTEST_") or key == "TEST_PREMATURE_EXIT_FILE"
        ),
    },
    "ledger": {
        "name": ledger_path.name,
        "sha256": sha256(ledger_path),
        "bytes": ledger_path.stat().st_size,
    },
    "iterations": records,
    "passed": passed,
}
if not passed:
    raise SystemExit("loaded-kernel raw manifest did not satisfy its own invariants")
temporary = manifest_path.with_name(manifest_path.name + ".tmp")
temporary.write_text(
    json.dumps(manifest, indent=2, sort_keys=True, allow_nan=False) + "\n",
    encoding="utf-8",
)
temporary.replace(manifest_path)
PY

echo "stability gate passed; logs and manifest retained in ${LOG_DIR}"
