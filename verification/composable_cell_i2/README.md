# Composable Cell I2 Qualification

Status: executable I2 qualification contract under development. Its historical
first-add lineage is recorded separately as non-qualifying evidence under
`historical_packet_lineage`. Formal evidence instead binds the complete current
ManyForge and Specs trees through clean source identities, a detached source
snapshot, manifest/verifier digests, final identity rechecks, and review.

This directory contains the executable qualification contract for I2 of the
Composable Cell Runtime Implementation Plan. Its historical atomic first-add
froze the implementation base and initial contract inventory. Bounded packet
defects discovered during execution may be corrected in ordinary follow-up
commits. A formal run binds the resulting current contract, the
qualification-time candidate identity rule, the resolved AMD64
development image and toolchain, the sole production Python module, every
binding registration unit and generated output, the retained file-only
RoboPlan/Pinocchio regression, exact native/production/binding build closure,
identities affected by the `mf2` cutover, the ordered qualification lanes, and
the final absence proofs.

`baseline_test_inventory.v1.json` freezes the exact names of every existing
Core, Deployment, RoboPlan, focused C0.2-tail, retained-consumer, and
RoboPlan-scene sanitizer test that the final candidate must preserve. Additive
tests are allowed only in the lanes marked additive; deleting, renaming,
disabling, or silently skipping a frozen test fails qualification. The one
Release-only checked-golden case is an explicit Deployment-sanitizer
exclusion, not a skip, and its passing Release result is required in the same
cumulative run.

`native_target_contract.v1.json` binds every registered native GTest target to
its exact test-name set and source. `build_provenance_contract.v1.json` is the
single authority for production and test-only CMake targets, direct source
sets, lane target partitions, generated-file ownership, BUILD_TESTING=OFF
Release parity, and the four binding builders. The packet-owned provenance
validator authenticates CMake File API replies, cache/tool identities,
`compile_commands.json`, Ninja commands, queries and response files, archives,
ELF dependencies, and the resulting artifacts. Native and binding provenance
is recomputed after test execution, and the host rehashes every referenced
local file after the container stops. It then submits one deduplicated batch
of every exact build tool and configure interpreter, pinned command input,
compiler dependency, transitive link input, ELF dependency, and inspection tool
to a fresh mount-free run of the exact immutable image. That replay overrides
the product entrypoint, independently resolves and hashes the image files,
preserves the producer's full-chain versus endpoint-only link rules, keeps every
full library chain inside the allowed library roots, permits hardlinks only for
compiler-closure endpoints, and verifies normal archive or ELF magic for
explicit and transitive link inputs and ELF runtime dependencies. Complete runs
also enforce the frozen tool catalogs, nonempty joined ELF dependency closures,
and minimum claim-kind cardinalities. The deterministic result receipt is
replayed again during closure. A successful build or test process without that
closure is not qualification evidence.

The full-suite floors were derived from the implementation-base Git objects:
1,609 literal `TEST`/`TEST_F` registrations plus five literal CMake
`add_test` registrations yield the 1,101 Core, 457 Deployment, and 56 RoboPlan
names. The base contains no parameterized or typed-test registrations. The
three focused groups are exact name/filter subsets of those frozen full-suite
registries; group overlap is intentional.

The manifest is a gate, not evidence that I2 already passes. Development
snapshots may be dirty, but no formal lane may start that way. Final evidence
must resolve the contract and implementation from clean committed ManyForge
and Specs trees and bind every lane result to those same identities. The
recorded implementation base and historical first-add commit remain lineage
evidence; neither substitutes for the current candidate identity or the final
result-bound review and closure.

The contract intentionally qualifies only the pure ROS-free compiler and the
`manyforge_core_py` pybind11 projection. It does not retain a product, start a
process, restore a product lane, recreate `manyforge_roboplan_py`, select a
different RoboPlan revision, or implement I3 or a later plan step. Historical
E0 pybind11/nanobind proof modules and their two-module recipe are comparison
inputs only and cannot satisfy any lane in this manifest.

The determinism lane may mechanically generate the already-existing I3
canonical-empty and Lite6 test products twice as regression evidence. Those
bounded, ignored qualification outputs are consumers of the I2 compiler; they
do not add or exercise a production retention authority and make no I3 claim.

## Files

- `qualification_manifest.v1.json` is the packet-owned, duplicate-key-rejecting
  machine contract.
- `baseline_test_inventory.v1.json` is the hash-bound, exact pre-I2 test-name
  floor derived from the implementation-base Git objects.
- `native_target_contract.v1.json` binds those names to exact CTest targets and
  sources.
- `build_provenance_contract.v1.json` freezes exact native, production-only,
  and binding build ownership.
- `native_build_provenance.py` derives evidence from CMake, Ninja, archives and
  ELF artifacts rather than trusting a build-authored summary.
- `verify_qualification_manifest.py` validates the packet contract and emits
  qualification evidence below
  `.manyforge-var/qualification/composable-cell-i2`.
- `tests/test_composable_cell_i2_qualification_manifest.py` and
  `tests/test_composable_cell_i2_build_provenance.py` adversarially exercise
  manifest, path, result, and build-provenance rejection boundaries.
- `scripts/ci/run-composable-cell-i2-qualification.sh` is the repository-root
  entry point. It delegates to the verifier without duplicating policy.

Run the structural manifest check from the repository root:

```bash
./scripts/ci/run-composable-cell-i2-qualification.sh --check
```

Resolve and verify the pinned Docker image and in-container ABI/dependency
probe without building source:

```bash
./scripts/ci/run-composable-cell-i2-qualification.sh \
  --check --check-environment
```

List the frozen lanes without running them:

```bash
./scripts/ci/run-composable-cell-i2-qualification.sh --list
```

All compiler, binding, import, sanitizer, and RoboPlan qualification commands
recorded by the manifest execute in the resolved AMD64 Docker image. Host-side
Python is permitted only for validating and dispatching this manifest; it is
not I2 qualification evidence.

The three TSan lanes prefix the complete in-container executor with
`/usr/bin/setarch x86_64 -R`; the frozen Docker profile grants only the
required personality syscall. This covers configure-time GTest discovery and
binding tests launched by the verifier, not just the final test subprocess.
Native ordinary-test validation also retains CTest 3.25.1's exact implicit
working directory and requires canonical command/source paths.

Four lane entry points named by the manifest were intentionally absent at the
historical first-add: `run-core-bindings.sh`,
`run-core-bindings-sanitizer.sh`, `run-core-bindings-contract.sh`, and
`run-composable-cell-i2-determinism.sh`. This is deliberate. They are frozen
required red gates that I2 must implement; their absence is not evidence that
the current tree passes or that a lane may be omitted.

## Freeze and completion rules

1. Preserve the historical atomic first-add and implementation-base lineage;
   packet corrections may use ordinary follow-up commits and do not need to
   reconstruct that old commit topology.
2. During development, run the smallest affected diagnostics and record each
   material failure once. A dirty development snapshot is never qualification
   evidence.
3. Before any formal lane, commit the exact ManyForge and Specs candidates and
   require both repositories to be clean descendants of their recorded
   implementation bases.
4. Resolve and retain those exact ManyForge and Specs identities for the whole
   qualification run.
5. Run the lanes in manifest order. A missing, skipped, disabled, or
   substituted lane is a failure, not an unqualified success.
6. Preserve command stdout/stderr, exit status, fixture/IR digests, generated
   stubs/documentation, image identity, and source/API deletion results below
   the declared output root.
7. Close I2 only when the complete cumulative matrix passes against the one
   candidate state, the independent review is bound to that result, and the
   evidence bundle verifies. Partial C0.2m, C0.2n, C0.2o, C++, or Python
   success is not an I2 completion claim.

An automatic all-lane run always writes `complete: false`. After an independent
adversarial review has produced a hash-bound
`manyforge.composable_cell_i2.adversarial_review.v1` JSON record in the same
run directory, verify and close that exact result with:

```bash
./scripts/ci/run-composable-cell-i2-qualification.sh \
  --verify-result .manyforge-var/qualification/composable-cell-i2/RUN/qualification-result.json \
  --review .manyforge-var/qualification/composable-cell-i2/RUN/adversarial-review.json
```

Only the resulting `qualification-closure.json` may carry `complete: true`.
Permanent Specs evidence records its digest and claim boundary; raw build/test
artifacts remain ignored by repository policy.
