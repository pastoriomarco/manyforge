# Step 18 scheduled-runtime baseline lane

> **History-rewrite provenance (2026-07-22):** The executable pins below
> use exact-tree rewritten commits. Historical evidence remains attributed
> to the original identities:
>
> - `manyforge` `643fe9c399182419d38d4478750d7e6f692dc9ad` → `2bfe30eeb37952f1ddc36bfdbf89d471a9f6b909` (tree `c900d971cdcbcfb6ec5d7f9b465ff18befa1dd89`);
> - `manyforge_specs` `5e50a4872bbb9a2167c6bc14b9b5963061f2f09e` → `67c24447a117e14c8274b13d05d720766671aa7a` (tree `7529497b02758c14666f85bc9bf98e85abec075a`);

Status: **implemented; evidence must still be captured on the frozen final
candidate**. The current-candidate runner cannot manufacture the historical
`current_all_active_distance` arm.

## Frozen identities and environment

`prepare-runtime-collision-schedule-baseline.sh` creates two detached
ManyForge worktrees at
`2bfe30eeb37952f1ddc36bfdbf89d471a9f6b909` and one detached Specs worktree at
`67c24447a117e14c8274b13d05d720766671aa7a`. It archives exact clean
`manyforge.source_identity.v1` objects before applying any patch, then records
the distinct timing/observation overlay identities after patching and verifies
that those identities remain unchanged through compilation.

Every candidate and baseline row carries both the full Specs source identity
and `manyforge.specs_contract_identity.v1`. The contract identity excludes only
`docs/implementation/contact-release-evidence-modes-step18-evidence-2026-07-19.md`,
so that evidence record can be updated after the campaign without changing the
contract candidate. No other path is excluded.

Use one explicit four-CPU set for the complete before/after campaign, for
example `10-13`. Preparation and every run bind the exact Compose dev image,
Release build, `-Wno-error=maybe-uninitialized`, CPU affinity,
governor/frequency driver, host kernel, commands, binaries, runner/helper
hashes, and UUID process identity. A caller-supplied image ID is only an
expectation and must equal the image actually resolved from Compose.

Prepare and run from the frozen candidate checkout. Both the retained lane and
the evidence directory must be new paths. The lane is deliberately constrained
to ignored `build/`; typed evidence is deliberately constrained to ignored
`qualification/` (or another destination outside every checkout).

The following is the exact executable setup. Do not edit either checkout, the
runners, wrappers, verifier, harness, or overlay inputs after the identities
are captured. The one Specs-contract-excluded evidence record may be edited
only after all 26 measurement processes have finished.

```bash
cd /absolute/path/to/manyforge

CAMPAIGN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
export MANYFORGE_SPECS_SOURCE_ROOT="$(realpath ../manyforge_specs)"
export MANYFORGE_BASELINE_SPECS_REPOSITORY="${MANYFORGE_SPECS_SOURCE_ROOT}"
export MANYFORGE_BASELINE_LANE_DIR="${PWD}/build/step18-runtime-baseline-${CAMPAIGN_ID}"
export MANYFORGE_RUNTIME_SCHEDULE_CPUSET=10-13
export MANYFORGE_BUILD_JOBS=1

RUNTIME_EVIDENCE="${PWD}/qualification/step18-${CAMPAIGN_ID}/runtime"
mkdir -p "${RUNTIME_EVIDENCE}/probes" "${RUNTIME_EVIDENCE}/formal"

CANDIDATE_SOURCE_IDENTITY="$(
  python3 scripts/qualification/source_identity.py --repo-root "${PWD}"
)"
CANDIDATE_SPECS_IDENTITIES="$(
  python3 scripts/qualification/source_identity.py \
    --repo-root "${MANYFORGE_SPECS_SOURCE_ROOT}" \
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
export MANYFORGE_EXPECTED_SPECS_FINGERPRINT_SHA256="${CANDIDATE_FINGERPRINTS[1]}"
export MANYFORGE_EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256="${CANDIDATE_FINGERPRINTS[2]}"

./scripts/prepare-runtime-collision-schedule-baseline.sh
```

The lane archives:

- `artifacts/clean-checkout-manifest.json`;
- `artifacts/baseline-harness.patch`; and
- `artifacts/lane-metadata.json`.

The separately reviewed observation patch remains
`scripts/qualification/baseline/runtime_collision_schedule_observation.patch`.

## Exact campaign

Every process receives the exact same explicit sampling profile:

```bash
RUNTIME_PROFILE=(
  --workers 2
  --warmup-ticks 40
  --warmup-lookahead-cycles 5
  --measured-ticks 200
  --measured-lookahead-cycles 20
  --max-extra-ticks 200
  --campaign-timeout-s 180
  --bounded-threshold-m 2.0
)
```

The harness additionally fixes the 20 Hz monitor, 50 ms immediate budget,
200 ms lookahead-cycle budget, two-second horizon, 75 ms query budget, and
exactly 32 lookahead samples. The formal verifier rejects a mutually matched
but reduced sampling profile.

First run one baseline timing and one baseline observation process with
`--purpose parity-probe`, then generate the parity artifact. These are two
independent processes and are not formal performance inputs:

```bash
MANYFORGE_RUNTIME_SCHEDULE_JSON_OUT="${RUNTIME_EVIDENCE}/probes/baseline-timing.json" \
  ./scripts/run-runtime-collision-schedule-baseline.sh \
    --measurement-kind timing --purpose parity-probe \
    "${RUNTIME_PROFILE[@]}"

MANYFORGE_RUNTIME_SCHEDULE_JSON_OUT="${RUNTIME_EVIDENCE}/probes/baseline-observation.json" \
  ./scripts/run-runtime-collision-schedule-baseline.sh \
    --measurement-kind observation --purpose parity-probe \
    "${RUNTIME_PROFILE[@]}"

BASELINE_PARITY="${RUNTIME_EVIDENCE}/baseline-decision-parity.json"
python3 scripts/qualification/generate_runtime_collision_baseline_parity.py \
  "${RUNTIME_EVIDENCE}/probes/baseline-timing.json" \
  "${RUNTIME_EVIDENCE}/probes/baseline-observation.json" \
  --output "${BASELINE_PARITY}"
```

Run the six baseline and eighteen current formal processes. Every invocation
uses a fresh runner-generated UUID process identity and writes one file with
exclusive creation:

```bash
for repetition in 1 2 3; do
  MANYFORGE_RUNTIME_SCHEDULE_JSON_OUT="${RUNTIME_EVIDENCE}/formal/baseline-timing-${repetition}.json" \
    ./scripts/run-runtime-collision-schedule-baseline.sh \
      --measurement-kind timing --purpose formal \
      "${RUNTIME_PROFILE[@]}"

  MANYFORGE_BASELINE_PARITY_ARTIFACT="${BASELINE_PARITY}" \
  MANYFORGE_RUNTIME_SCHEDULE_JSON_OUT="${RUNTIME_EVIDENCE}/formal/baseline-observation-${repetition}.json" \
    ./scripts/run-runtime-collision-schedule-baseline.sh \
      --measurement-kind observation --purpose formal \
      "${RUNTIME_PROFILE[@]}"
done

for arm in \
  signed_penetration_selective \
  binary_contact \
  bounded_penetration; do
  for measurement_kind in timing observation; do
    for repetition in 1 2 3; do
      MANYFORGE_RUNTIME_SCHEDULE_JSON_OUT="${RUNTIME_EVIDENCE}/formal/current-${arm}-${measurement_kind}-${repetition}.json" \
        ./scripts/run-runtime-collision-schedule-qualification.sh \
          --measurement-kind "${measurement_kind}" \
          --arm "${arm}" \
          "${RUNTIME_PROFILE[@]}"
    done
  done
done
```

That produces exactly 24 formal JSON files: four arms, two measurement kinds,
and three process repetitions. Candidate runners require the frozen ManyForge
source fingerprint, full Specs fingerprint, stable Specs contract fingerprint,
and actual Specs root; all three identities are rechecked after measurement.

## Verification

Pass only the 24 formal records (never the two parity probes):

```bash
mapfile -t RUNTIME_RECORDS < <(
  find "${RUNTIME_EVIDENCE}/formal" -maxdepth 1 -type f -name '*.json' \
    -print | LC_ALL=C sort
)
[[ ${#RUNTIME_RECORDS[@]} -eq 24 ]]

python3 scripts/qualification/verify_runtime_collision_schedule_step18.py \
  --minimum-runs 3 \
  --baseline-clean-checkout-manifest "${MANYFORGE_BASELINE_LANE_DIR}/artifacts/clean-checkout-manifest.json" \
  --baseline-harness-patch "${MANYFORGE_BASELINE_LANE_DIR}/artifacts/baseline-harness.patch" \
  --baseline-observation-patch scripts/qualification/baseline/runtime_collision_schedule_observation.patch \
  --baseline-decision-parity-artifact "${BASELINE_PARITY}" \
  --summary-out "${RUNTIME_EVIDENCE}/runtime-verification.json" \
  "${RUNTIME_RECORDS[@]}"
```

Formal `--minimum-runs` must be exactly three. Within every group and globally,
run IDs, process UUIDs, artifact paths, and artifact hashes must be unique.
`--allow-missing-baseline` is a development-only check and never passes the
scheduled-runtime subgate.

This verifier closes only the isolated scheduled-runtime subgate. Its summary
explicitly requires the common-candidate Step 18 aggregator; controller,
viewer, Isaac/process-tree, mesh-heavy scenario, sanitizer, full-regression,
and projection-retry evidence remain separate required inputs.
