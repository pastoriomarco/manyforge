> **Historical qualification runbook only.** Preserved from `manyforge@08222188293df8e12f90dc72e204c21f22f92ea1` as design/evidence context. Its runtime targets were demolished and its commands must not be run or cited as current qualification until the named restoration gates pass.

# Step 18 offline proof campaign

> **History-rewrite provenance (2026-07-22):** The executable pins below
> use exact-tree rewritten commits. Historical evidence remains attributed
> to the original identities:
>
> - `manyforge` `643fe9c399182419d38d4478750d7e6f692dc9ad` → `2bfe30eeb37952f1ddc36bfdbf89d471a9f6b909` (tree `c900d971cdcbcfb6ec5d7f9b465ff18befa1dd89`);
> - `manyforge_specs` `5e50a4872bbb9a2167c6bc14b9b5963061f2f09e` → `67c24447a117e14c8274b13d05d720766671aa7a` (tree `7529497b02758c14666f85bc9bf98e85abec075a`);

Historical status at the pinned source: **implemented but not closed; formal
evidence still had to be captured from one frozen final candidate**. This
runbook covered the offline before/after performance and semantic-proof subgate
only; it did not close Step 18 by itself.

Current branch status: **the aggregate producer is unavailable until I6/N and
I8/P5**. Pure verifiers and focused retained Core/collision tests remain
current, but the campaign commands below are restoration input rather than a
current qualification lane.

## Fixed population

The formal campaign contains exactly 48 independent groups and three processes
per group:

| Cell | Groups | Records |
|---|---:|---:|
| Lightweight collision-only/binary-contact, 3 waypoint counts | 6 | 18 |
| Pinned all-active-distance policy baseline, 3 waypoint counts, 2 worker profiles | 6 | 18 |
| Four current policy arms, 3 waypoint counts, 2 worker profiles | 24 | 72 |
| Baseline/current contact-grasp, 2 waypoint counts, 2 worker profiles | 8 | 24 |
| Baseline/current nine-fixture free-space matrix | 2 | 6 |
| Baseline/current untimed precise-reference parity | 2 | 6 |
| **Total** | **48** | **144** |

The verifier retains the 144 typed records plus the pinned baseline clean
checkout manifest and qualification overlay, for exactly 146 retained
artifacts.

The six `precise-reference-parity` records are semantic evidence, not timing
samples. Each record covers the exact ten-case corpus at worker counts 1 through
5; the serial reference plus every one of the 1,024 five-waypoint sample-kind
patterns at worker counts 2 through 5; all six three-worker completion orders
plus serial; and deterministic pre-cancellation, expired-deadline, and worker-
exception outcomes. Distance evidence is audited only for colliding pairs.
Timing fields do not contribute to this parity gate.

## Freeze identities and prepare the baseline

Run from the candidate ManyForge checkout. Use a new baseline lane, evidence
directory, and summary path. Select one explicit set of exactly four CPUs and
keep it unchanged for the whole campaign. The runners bind and recheck source,
full Specs, Specs-contract, Compose image, compiler, Release flags, CPU affinity,
governor/driver, host kernel, binary, command, wrapper, and process identity.

Do not edit either checkout, any qualification tool or harness, the Compose dev
image, or the prepared lane after capturing the candidate fingerprints. Do not
run another workload on the selected CPUs during the performance campaign.

```bash
cd /absolute/path/to/manyforge

CAMPAIGN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
export MANYFORGE_SPECS_ROOT="$(realpath ../manyforge_specs)"
export MANYFORGE_BASELINE_SPECS_REPOSITORY="${MANYFORGE_SPECS_ROOT}"
export MANYFORGE_STEP18_NONREGRESSION_BASELINE_DIR="${PWD}/build/step18-offline-baseline-${CAMPAIGN_ID}"
export MANYFORGE_STEP18_PROOF_BUILD_DIR="${PWD}/build/step18-offline-current-${CAMPAIGN_ID}"
export MANYFORGE_STEP18_NONREGRESSION_CPUSET=10-13
export MANYFORGE_BUILD_JOBS=2

OFFLINE_EVIDENCE="${PWD}/qualification/step18-${CAMPAIGN_ID}/offline"
FORMAL_RECORDS="${OFFLINE_EVIDENCE}/formal"
mkdir -p "${FORMAL_RECORDS}"

CANDIDATE_SOURCE_IDENTITY="$(
  python3 scripts/qualification/source_identity.py --repo-root "${PWD}"
)"
CANDIDATE_SPECS_IDENTITIES="$(
  python3 scripts/qualification/source_identity.py \
    --repo-root "${MANYFORGE_SPECS_ROOT}" \
    --include-specs-contract-identity
)"
mapfile -t CANDIDATE_FINGERPRINTS < <(
  python3 - "${CANDIDATE_SOURCE_IDENTITY}" \
    "${CANDIDATE_SPECS_IDENTITIES}" <<'PY'
import json
import sys

source = json.loads(sys.argv[1])
specs = json.loads(sys.argv[2])
print(source["fingerprint_sha256"])
print(specs["source_identity"]["fingerprint_sha256"])
print(specs["specs_contract_identity"]["contract_fingerprint_sha256"])
PY
)
[[ ${#CANDIDATE_FINGERPRINTS[@]} -eq 3 ]]
export MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256="${CANDIDATE_FINGERPRINTS[0]}"
export MANYFORGE_EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256="${CANDIDATE_FINGERPRINTS[1]}"
export MANYFORGE_EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256="${CANDIDATE_FINGERPRINTS[2]}"

./scripts/prepare-step18-validation-nonregression-baseline.sh
```

The prepared lane is a detached clean ManyForge `2bfe30e` plus Specs
`67c2444` build with a hash-bound qualification-only overlay. Its pristine
binary is the timing authority for `current_all_active_distance`; the current
binary must never emulate that historical arm.

## Exact formal commands

These helpers give every process a unique run ID and exclusive output path. The
runner also authors a fresh UUID process identity. The verifier later assigns
the stable repetition index by sorting the three unique run IDs in each role;
there is no separate unauthenticated repetition field.

```bash
run_baseline() {
  local name="$1"
  local repetition="$2"
  shift 2
  MANYFORGE_QUAL_RUN_ID="${CAMPAIGN_ID}-baseline-${name}-r${repetition}" \
  MANYFORGE_STEP18_PROOF_JSON_OUT="${FORMAL_RECORDS}/baseline-${name}-r${repetition}.json" \
    ./scripts/run-step18-validation-nonregression-baseline.sh "$@" \
      >/dev/null
}

run_current() {
  local name="$1"
  local repetition="$2"
  shift 2
  MANYFORGE_QUAL_RUN_ID="${CAMPAIGN_ID}-current-${name}-r${repetition}" \
  MANYFORGE_STEP18_PROOF_JSON_OUT="${FORMAL_RECORDS}/current-${name}-r${repetition}.json" \
    ./scripts/run-step18-proof-cell-qualification.sh "$@" \
      >/dev/null
}
```

Capture the lightweight collision-only comparison:

```bash
for arm in collision_only binary_contact; do
  for waypoints in 96 500 1000; do
    for repetition in 1 2 3; do
      run_current "lightweight-${arm}-wp${waypoints}" "${repetition}" \
        --cell lightweight-collision-only \
        --arm "${arm}" --waypoints "${waypoints}" \
        --warmup 5 --measured 50
    done
  done
done
```

Capture the pinned before policy and all four current policy arms on identical
support/contact geometry:

```bash
for waypoints in 96 500 1000; do
  for workers in 1 4; do
    for repetition in 1 2 3; do
      run_baseline "policy-wp${waypoints}-w${workers}" "${repetition}" \
        --cell policy-support-contact \
        --arm current_all_active_distance \
        --waypoints "${waypoints}" --workers "${workers}" \
        --warmup-runs 5 --measured-runs 50
    done
    for arm in \
      signed_penetration_selective \
      binary_contact \
      bounded_penetration \
      collision_only; do
      for repetition in 1 2 3; do
        run_current "policy-${arm}-wp${waypoints}-w${workers}" "${repetition}" \
          --cell policy-support-contact \
          --arm "${arm}" --waypoints "${waypoints}" --workers "${workers}" \
          --warmup 5 --measured 50
      done
    done
  done
done
```

Capture baseline/current contact-grasp non-regression:

```bash
for waypoints in 96 500; do
  for workers in 1 4; do
    for repetition in 1 2 3; do
      run_baseline "contact-grasp-wp${waypoints}-w${workers}" "${repetition}" \
        --cell contact-grasp \
        --waypoints "${waypoints}" --workers "${workers}" \
        --warmup-runs 5 --measured-runs 50
      run_current "contact-grasp-wp${waypoints}-w${workers}" "${repetition}" \
        --cell contact-grasp \
        --waypoints "${waypoints}" --workers "${workers}" \
        --warmup 5 --measured 50
    done
  done
done
```

Capture the baseline/current nine-fixture matrix and the untimed precise
semantic reference:

```bash
for repetition in 1 2 3; do
  run_baseline free-space "${repetition}" --cell free-space
  run_current free-space "${repetition}" --cell free-space

  run_baseline precise-reference "${repetition}" \
    --cell precise-reference-parity
  run_current precise-reference "${repetition}" \
    --cell precise-reference-parity
done
```

The complete directory must now contain exactly 144 JSON records. Never replace
a failed/missing process by copying or editing another artifact; rerun the whole
campaign from new output paths if the frozen identity or environment changed.

## Verification

The formal verifier accepts exactly three processes in every one of the 48
groups and rejects extra, missing, duplicate, overwritten, or identity-drifted
records. It also verifies exact baseline/current normalized semantic parity for
the six untimed records, including the closed reviewed migration only.

```bash
mapfile -t OFFLINE_RECORDS < <(
  find "${FORMAL_RECORDS}" -maxdepth 1 -type f -name '*.json' \
    -print | LC_ALL=C sort
)
[[ ${#OFFLINE_RECORDS[@]} -eq 144 ]]

OFFLINE_SUMMARY="${OFFLINE_EVIDENCE}/offline-proof-verification.json"
python3 scripts/qualification/verify_step18_proof_cells.py \
  --minimum-runs 3 \
  --baseline-clean-checkout-manifest \
    "${MANYFORGE_STEP18_NONREGRESSION_BASELINE_DIR}/artifacts/clean-checkout-manifest.json" \
  --baseline-qualification-patch \
    "${MANYFORGE_STEP18_NONREGRESSION_BASELINE_DIR}/artifacts/qualification-overlay.patch" \
  --summary-out "${OFFLINE_SUMMARY}" \
  "${OFFLINE_RECORDS[@]}"
```

The summary must report 48 groups, 144 records, 146 retained artifacts, and
passing `precise_reference_parity.semantic_parity`. Raw qualification JSON stays
under the ignored evidence root; the committed evidence record carries hashes,
commands, measurements, and conclusions.

## Non-closure boundary

This campaign closes only the Step 18 offline proof/performance input. It does
not replace the scheduled-runtime campaign, loaded-kernel and live-service
qualification, controller/viewer load evidence, sanitizers, full native/ROS/
RoboPlan regression, release-boundary checks, or the exact common-candidate
closure manifest. `--development-smoke` and partial one-process checks are
diagnostic only and cannot satisfy this runbook. Step 18 remains open until the
top-level closure verifier binds every required artifact to the same frozen
candidate and passes.
