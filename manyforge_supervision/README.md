# Historical Python supervision disposition

Status: **the Python owners are deleted and relocation-qualified. The C++ I4
successor owns and qualifies the migrated M0 runtime fixture, and the C++
I5/H0 daemon/API candidate supplies the public owner-issued projection without
reviving this package.**

`manyforge_supervision` no longer ships a production Python package. The C++
`manyforge_component_manager` package is the selected I4 semantic owner for
runtime-unit observation and desired/observed process convergence. Keeping the
old Python reducers or fixed Docker supervisor importable would create a
second owner and an undeclared fallback.

The current Python sources under `verification/composable_cell_i4/` are
deliberately test-only, product-effect-free reference fixtures. They are retained
to state ownership and failure invariants; production code must not import
them. The thin Python surface now lives in `manyforge_headless_api`, over
owner-issued facts. This historical package must not return as a Manager,
process/status, or health owner, and no client may restore the reducers, v1
heartbeat/policy wire, execution pin, or two-container teardown state machine
recorded below.

The 2026-08-01 relocation evidence qualifies the deletion and exact historical
transfer ledger. Final I4 evidence binds the reconnectable retained-POSIX
adapter, active-runtime coordinator, external-process M0 proof, and cumulative
executable results; this ledger remains the historical behavior map rather
than a second qualification authority.

## Exact historical sources

The complete implementation remains available at ManyForge commit
`08222188293df8e12f90dc72e204c21f22f92ea1`. These blobs are the exact design
and acceptance inputs; they are not runnable fallbacks.

| Historical path | Blob | Disposition |
| --- | --- | --- |
| `manyforge_supervision/cell_supervisor.py` | `b8579fa633ced249191f563befe85cacb1915d43` | Delete the effectful fixed-Docker supervisor and its independent supervision/teardown state machine. Port bounded process-observation and effect-reconciliation invariants to the C++ Manager and thin adapters at I4; do not restore the exact two-container policy. |
| `manyforge_supervision/component_health.py` | `0a98b96a83283ace5728cd6d37896e168b3d560c` | Delete the competing Python health/dependency/admission semantic owner. Runtime-unit observation belongs to C++ I4; execution admission/pinning belongs to the C++ Program/Kernel/lifecycle owners at N/I6 and L0/I7. |
| `manyforge_supervision/manifest.py` | `ea62527ff99ad731376148ec936238df8d11c5b9` | Delete the exclusive `manyforge.cell_supervisor.{policy,heartbeat}.v1` parser/wire. Strict negative parsing remains an acceptance input for final I2/I3 products and I4 status adapters, not a compatibility contract. |
| `manyforge_supervision/runtime_observation.py` | `31fa317156c9b6a8a8a029f00fdbbba9c9e52240` | Delete the Python duplicate of the selected C++ runtime-observation owner. Keep the independent verification fixture and the C++ owner tests. |
| `manyforge_supervision/__init__.py` | `3fbdcb98389b9e14b734d7fad9e3e14524545691` | Delete exports that exposed the obsolete owners as a production package. |

The retained owner/reference evidence is:

- `manyforge_component_manager/{include,src}/.../runtime_observation.*`;
- `manyforge_component_manager/{include,src}/.../manager_active_runtime_coordinator.*`;
- `manyforge_component_manager/{include,src}/.../retained_posix_runtime_unit_adapter.*`;
- `manyforge_component_manager/test/test_runtime_observation.cpp` and
  `testdata/runtime_observation_parity_v1.txt`;
- `manyforge_component_manager/test/test_manager_active_runtime_coordinator.cpp`,
  `test_retained_posix_runtime_unit_adapter.cpp`, and
  `manyforge_scene/test/test_component_manager_m0_process.cpp`;
- `verification/composable_cell_i4/{runtime_observation_fixture.py,process_fixture.py}`;
- `tests/composable_cell_i4/{test_runtime_observation.py,test_process_fixture.py}`.

## Exact historical test-case assignment

For each deleted test file, the digest is SHA-256 over newline-delimited
`definition-line:test-name` records in source order. This makes the ledger
independent of prose reformatting while retaining every historical test
definition exactly.

### Fixed supervisor cohort — 10 cases

Source: `manyforge_composer/tests/test_cell_supervisor.py`, blob
`8295eefcd305e63c50efd368547244a2339ab054`, case digest
`dfeb5f3e0f3d15c7972286ef1c590c2f7a7270bde6401ec5cd1b66662956942d`.

| Historical case | Successor owner/gate and evidence use |
| --- | --- |
| `test_supervisor_starts_fenced_and_arms_only_complete_domain` | I4 durable staged readiness, dependency convergence, and `closed_no_active_effect` evidence are carried by the runtime-convergence and M0 tests. Manager never arms a robot domain. |
| `test_unexpected_composer_exit_tears_down_owner_without_restart` | The teardown outcome is intentionally superseded: spec 100 §5.8 makes Composer/browser loss inert, while Manager loss leaves the exact runtime/epoch unchanged. Adapter and M0 process-loss tests carry the successor behavior. |
| `test_reduced_domain_health_loss_tears_down_complete_domain` | I4 observes source loss and exposes nonconvergence without restoring Composer-owned aggregate health or containment. I5/H0 projects that owner-issued fact. |
| `test_docker_process_control_observes_domain_in_one_bounded_call` | The closed retained-POSIX adapter supplies bounded exact process observation and fixed lifecycle effects without robot/lifecycle authority; fixed Docker-domain policy is not restored. |
| `test_composer_probe_requires_progress_and_pins_epoch` | I5/H0 status projection plus I4 receiver-local freshness/incarnation evidence; current C++/fixture observation tests carry the invariant. |
| `test_composer_probe_requires_persistent_reduced_unhealthy_state` | I5/H0 source facts and I4 convergence policy; the old Composer-reduced health endpoint is not restored as authority. |
| `test_explicit_shutdown_is_dependency_ordered` | I4 active-runtime tests prove reverse-dependency explicit shutdown, exact absence, idempotency, and fresh epochs after restart. |
| `test_force_remove_is_recorded_as_unclean_not_physical_stop` | I4 intentionally exposes no force-remove or signal choice: uncertain fixed shutdown remains unavailable until observed. P5/I8 still proves that process teardown never substitutes for Kernel Stop. |
| `test_runner_persists_one_terminal_epoch_and_restores_signals` | The reconnectable controller and durable process identity carry bounded Manager-loss/replacement and terminal process evidence; I5/H0 supplies public bootstrap and thin-client process handling. |
| `test_runner_contains_domain_if_health_adapter_raises` | I4 fails closed on adapter/reconciliation errors and never restores the Python containment state machine. |

### v1 component-health and manifest cohort — 9 cases

Source: `manyforge_composer/tests/test_component_supervision.py`, blob
`9ba5cf1ec6dd0dfe7b3b868f9ad79e3aa31f3a79`, case digest
`2acc35f3e3d56897b1af69803b12ae294a2e5f093acee1eb1ece06378feeb8e0`.

| Historical case | Successor owner/gate and evidence use |
| --- | --- |
| `test_policy_rejects_unknown_dependency_and_cycle` | C++ I4 expectation-graph validation; carried by current C++ and fixture tests. |
| `test_manifest_parser_rejects_unknown_top_and_component_keys` | Strict negative parsing for final I2/I3 retained products and I4 adapter inputs; never restore the v1 policy schema. |
| `test_manifest_parser_accepts_runtime_supervision_metadata` | Port only metadata that exists in the final I2/I3 runtime-unit/product contracts. The old role vocabulary is not authoritative. |
| `test_external_heartbeat_reader_is_strict_and_identity_bound` | The retained-POSIX status adapter now validates exact runtime-unit/source/incarnation/product/configuration identities. A later external product still requires its own observation-only adapter. |
| `test_missing_duplicate_regressed_and_conflicting_samples_fail_closed` | C++ I4 observation ingestion; carried by current C++ and fixture duplicate/conflict/regression tests. |
| `test_same_epoch_heartbeat_does_not_fake_critical_progress` | C++ I4 receiver-local progress freshness; carried by current C++ and fixture tests. |
| `test_dependency_epoch_mismatch_propagates_to_dependants` | C++ I4 causal dependency rejoin; carried by current C++ and fixture tests. |
| `test_generation_changes_only_for_semantics_not_freshness` | I5/H0 owner-issued projection generation and I4 snapshot semantics; the H0 implementation replaces rather than revives the deleted Python generation counter. |
| `test_execution_pin_rejects_replacement_and_never_auto_repairs` | C++ Program Binding/Kernel/lifecycle admission at N/I6 and L0/I7, consuming I4 runtime epochs. The Python execution pin is not restored. |

### Python runtime-observation owner cohort — 23 cases

Source: `tests/manyforge_supervision/test_runtime_observation.py`, blob
`f69e4ce42b4e2feee707fc49807b9ce37f5b81dc`, case digest
`27f8563b15f07bf0d483996c09eb4c301bc42e514e3a1c7b03b4a5fd962f11dc`.

These cases transfer to the C++ I4 owner and the test-only I4 fixtures. The two
distinct owner-neutral cases that were not explicit in those retained suites
were ported as
`test_receiver_clock_is_the_only_freshness_timestamp_source` and
`test_maximum_length_dependency_id_keeps_precise_manager_reason` in
`tests/composable_cell_i4/test_runtime_observation.py`.

The exact transferred definitions are:

1. `test_missing_report_and_first_convergence_have_distinct_states`
1. `test_source_report_cannot_publish_manager_owned_fields`
1. `test_receiver_clock_is_the_only_freshness_timestamp_source`
1. `test_duplicate_conflict_and_regression_do_not_renew_report`
1. `test_report_freshness_does_not_fabricate_progress`
1. `test_progress_expiry_is_optional_manager_policy`
1. `test_observer_replacement_fences_retired_session_replay`
1. `test_presence_duplicate_conflict_and_regression_do_not_renew_freshness`
1. `test_absence_retires_epoch_and_reuse_fails_closed`
1. `test_unknown_presence_does_not_fabricate_a_restart`
1. `test_direct_incarnation_replacement_requires_new_source_report`
1. `test_off_presence_report_cannot_become_latent_evidence`
1. `test_dependency_epoch_change_requires_fresh_dependent_report`
1. `test_dependent_report_before_epoch_change_cannot_satisfy_new_join`
1. `test_exact_source_evidence_is_required_for_convergence`
1. `test_source_and_manager_reasons_remain_independent_and_immutable`
1. `test_expectation_graph_is_strict_bounded_and_acyclic`
1. `test_undeclared_ingress_and_unbounded_source_sets_fail_closed`
1. `test_maximum_length_dependency_id_keeps_precise_manager_reason`
1. `test_observer_and_incarnation_replay_ledgers_are_bounded`
1. `test_process_binding_separates_observation_and_manager_effects`
1. `test_monotonic_receiver_clock_regression_fails_closed`
1. `test_fixture_and_production_core_share_derived_state_semantics`

The last case no longer compares two production owners. Its intended parity is
split between the independent fixture tests and the C++ shared-golden test;
the C++ implementation is authoritative.

### Cross-language golden cohort — 1 case

Source:
`tests/manyforge_supervision/test_cpp_runtime_observation_parity_vectors.py`,
blob `654fe04909f9cf2f0cce1dc50e67e934819764c6`, case digest
`9a98e4f267bc245176dbd757cd4a3d71a9bca75f878f97037773a66d66d3b31b`.

`test_cpp_and_python_runtime_observation_share_golden_vectors` transfers to
the authoritative C++
`RuntimeObservationReducer.SharedGoldenMatchesPythonReference` test over the
retained `runtime_observation_parity_v1.txt` vector. It no longer executes a
second production Python reducer.

Together these four cohorts account for all **43** deleted historical test
definitions. The relocation evidence already qualifies their exact retention
and routing. Final I4 evidence must additionally bind the C++ observation,
process convergence, Apply recovery, active reconciliation, and M0 successor
tests before the ownership cutover closes. H0 status APIs, Program/lifecycle
admission, external products, and every product lane remain unavailable until
their later plan gates pass.
