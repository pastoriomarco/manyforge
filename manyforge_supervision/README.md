# Historical Python supervision disposition

Status: **source-reconciled and unqualified**.

`manyforge_supervision` no longer ships a production Python package. The C++
`manyforge_component_manager` package is the selected I4 semantic owner for
runtime-unit observation and desired/observed process convergence. Keeping the
old Python reducers or fixed Docker supervisor importable would create a
second owner and an undeclared fallback.

The current Python sources under `verification/composable_cell_i4/` are
deliberately test-only, product-effect-free reference fixtures. They are retained
to state ownership and failure invariants; production code must not import
them. A future Python surface may return here only as a thin NRT Manager
client or process/status adapter over owner-issued facts. It must not restore
the reducers, v1 heartbeat/policy wire, execution pin, or two-container
teardown state machine recorded below.

No build or test has qualified this source reconciliation yet.

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
- `manyforge_component_manager/test/test_runtime_observation.cpp` and
  `testdata/runtime_observation_parity_v1.txt`;
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

| Historical case | Future owner/gate and evidence use |
| --- | --- |
| `test_supervisor_starts_fenced_and_arms_only_complete_domain` | I4 Manager staged readiness, dependency convergence, and command fencing; current process fixture proves only owner/effect separation. |
| `test_unexpected_composer_exit_tears_down_owner_without_restart` | I4 desired/observed convergence and external-bootstrap policy. Re-evaluate the old blanket teardown/no-restart outcome against explicit runtime-unit ownership. |
| `test_reduced_domain_health_loss_tears_down_complete_domain` | I4 source observation and process convergence. Do not preserve the old aggregate Composer-health authority. |
| `test_docker_process_control_observes_domain_in_one_bounded_call` | Future thin I4 process adapter: bounded observation, independent handles, and no robot/lifecycle authority. |
| `test_composer_probe_requires_progress_and_pins_epoch` | H0/I5 status source plus I4 receiver-local freshness/incarnation evidence; current C++/fixture observation tests carry the invariant. |
| `test_composer_probe_requires_persistent_reduced_unhealthy_state` | H0/I5 source facts and I4 convergence policy; the old Composer-reduced health endpoint is not restored as authority. |
| `test_explicit_shutdown_is_dependency_ordered` | I4 dependency-ordered process convergence and external-bootstrap teardown evidence. |
| `test_force_remove_is_recorded_as_unclean_not_physical_stop` | I4 effect reconciliation plus P5/I8 proof that process teardown never substitutes for Kernel Stop. |
| `test_runner_persists_one_terminal_epoch_and_restores_signals` | Future external-bootstrap/thin process adapter at I4; terminal process evidence remains separate from robot truth. |
| `test_runner_contains_domain_if_health_adapter_raises` | I4 fail-closed adapter/reconciliation evidence; no Python-owned containment state machine. |

### v1 component-health and manifest cohort — 9 cases

Source: `manyforge_composer/tests/test_component_supervision.py`, blob
`9ba5cf1ec6dd0dfe7b3b868f9ad79e3aa31f3a79`, case digest
`2acc35f3e3d56897b1af69803b12ae294a2e5f093acee1eb1ece06378feeb8e0`.

| Historical case | Future owner/gate and evidence use |
| --- | --- |
| `test_policy_rejects_unknown_dependency_and_cycle` | C++ I4 expectation-graph validation; carried by current C++ and fixture tests. |
| `test_manifest_parser_rejects_unknown_top_and_component_keys` | Strict negative parsing for final I2/I3 retained products and I4 adapter inputs; never restore the v1 policy schema. |
| `test_manifest_parser_accepts_runtime_supervision_metadata` | Port only metadata that exists in the final I2/I3 runtime-unit/product contracts. The old role vocabulary is not authoritative. |
| `test_external_heartbeat_reader_is_strict_and_identity_bound` | Future thin I4 status adapter over explicit runtime-unit/source/incarnation identities. |
| `test_missing_duplicate_regressed_and_conflicting_samples_fail_closed` | C++ I4 observation ingestion; carried by current C++ and fixture duplicate/conflict/regression tests. |
| `test_same_epoch_heartbeat_does_not_fake_critical_progress` | C++ I4 receiver-local progress freshness; carried by current C++ and fixture tests. |
| `test_dependency_epoch_mismatch_propagates_to_dependants` | C++ I4 causal dependency rejoin; carried by current C++ and fixture tests. |
| `test_generation_changes_only_for_semantics_not_freshness` | H0/I5 owner-issued projection generation and I4 snapshot semantics; still future, not evidence supplied by the deleted Python generation counter. |
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
definitions. Passing the retained tests later will qualify only their named
ROS-free observation/process-fixture claims; operational Apply, process
convergence, H0 status APIs, Program/lifecycle admission, and any product lane
remain unavailable until their respective plan gates pass.
