# Historical Composer interaction reference

Status: **design and reimplementation input only; no current availability or
qualification claim.**

The last committed tree containing the complete former Composer interaction
implementation is:

- commit: `08222188293df8e12f90dc72e204c21f22f92ea1`;
- tree: `bc749b9879d3c8f36fe7bf3036f9cc1f2ef1e60f`;
- parent: `c4b075fa6dfb69e463c61f3efa8cfce8de681e8a`.

That tree is not a runnable baseline for the current branch. Several semantic
dependencies had already entered demolition. Use it to understand workflows,
message shapes, UI intent, and component interaction—not to restore a
compatibility runtime or a second semantic owner.

The reference does **not** imply that every former Python or TypeScript file is
obsolete. The cutover is responsibility-based, not directory-based:

- runtime, scene, execution, lifecycle, persistence, and other authoritative
  state machines move to their selected headless owners;
- FastAPI routes, transport adapters, authoring logic, UI presentation, and
  assistant mediation remain same-language client candidates where their
  behavior still belongs to the target product; and
- a mixed file must be split or rewritten at its owning gate rather than being
  classified solely because it lived under `manyforge_composer`.

## Retained interaction records

The following documents remain in-tree with a historical/non-activatable
banner because they capture useful behavior and safety intent:

- [`MCP_RUNTIME_CONTRACT.md`](MCP_RUNTIME_CONTRACT.md);
- [`ASSISTANT_PROVIDER_CONTRACT.md`](ASSISTANT_PROVIDER_CONTRACT.md);
- [`../../agent-skills/manyforge-composer/SKILL.md`](../../agent-skills/manyforge-composer/SKILL.md);
- [`../../agent-skills/manyforge-composer/workspace-AGENTS.md`](../../agent-skills/manyforge-composer/workspace-AGENTS.md);
- [`../../scripts/templates/AGENTS.md`](../../scripts/templates/AGENTS.md);
- [`../../scripts/templates/SOUL.md`](../../scripts/templates/SOUL.md); and
- the five historical recovery/diagnosis/monitoring skill templates under
  `scripts/templates/skills/`.

Their former frontmatter is deliberately preceded by a historical banner, so
skill tooling cannot treat them as current installable payloads.

## Source checkpoint and porting map

The following source areas pin the last complete interaction among components.
Inspect them at the reference commit when auditing what is retained, moved, or
deleted. A Git reference is not by itself a decision that the whole area will
be recreated from scratch:

The companion
`composable-cell-relocation-responsibility-disposition-record-2026-07-31.md`
in the implementation-specification repository is authoritative for
non-Composer Behavior, ROS/Isaac, Medkit, assistant-tooling, script,
qualification, and xArm groups; this reference does not duplicate those
assignments.

| Former surface | Reference paths at `082221...` | What to recover conceptually |
| --- | --- | --- |
| Composer backend | `manyforge_composer/backend/` | Separate old semantic ownership from retained Python route, transport, security, and client responsibilities |
| Composer frontend | `manyforge_composer/frontend/src/` | Retain or port editor/viewer/operator interaction intent in TypeScript; remove only superseded runtime/client contracts |
| Assistant bridge | `manyforge_assistant_bridge/` | Audit the Python provider envelope, bounded turns, tool mediation, and clarification behavior as thin-client candidates |
| MCP server | `manyforge_composer/backend/mcp_server.py` | Tool vocabulary, read/draft separation, audit context |
| Viewer protocol | `docs/reference/VIEWER_DATA_PLANE_V1.*` and frontend generated types | Static/dynamic viewing requirements and preservation cases |
| Sandbox policy | `scripts/templates/manyforge-policy-fragment.yaml` | Historical connectivity assumptions, not a future policy default |
| Old qualification harnesses | `scripts/qualification/` and former Composer tests | Scenarios and failure cases worth translating onto new owners |

For example:

```bash
git show 08222188293df8e12f90dc72e204c21f22f92ea1:manyforge_composer/backend/state.py
git ls-tree -r --name-only 08222188293df8e12f90dc72e204c21f22f92ea1 -- manyforge_composer
```

These commands inspect history; they do not restore a file into the current
worktree.

## Gate-by-gate client port map

A path absent from the current worktree is not automatically a rejected
product behavior. The table below records where the former implementation is
consulted and what must replace its obsolete contract before the behavior can
be activated.

| Gate | Same-language interaction behavior to retain or port | Contract/ownership that must not return |
| --- | --- | --- |
| Current structural shell | Read-only catalog browsing, sandboxed artifact browsing, local camera/orbit controls, visual/collision geometry and authored-neutral model inspection | Live scene state, WebSocket pose truth, scene edits, inferred robot state or runtime readiness |
| H0 / I5 | Runtime-status presentation, alarm lifecycle/history UI, event-history presentation, node-environment diagnostics, and closed read-only assistant/MCP adapters | Composer-owned runtime/scene/alarm truth, old scene initialization owner, provider/authoring effects, and direct endpoint-state reduction |
| N / I6 | Behavior-tree/program/deployment authoring, validation, layout/draft mechanics, addressed component/target selection and manual scene-client workflows | Singleton robot/group assumptions, v1 factory/loading, old scene DTOs, path-derived runtime names and client-side capability unions |
| L0 / I7 | Lifecycle/controller request forms, status explanations and recovery guidance | Caller leases/sessions, browser-owned command authority, vendor policy or lifecycle truth in the UI |
| P5 / I8 | Execution/Stop controls, recovery choices and Kernel error presentation as thin requests | CycleManager/backend Stop settlement, browser-selected Stop fallback profiles and client-minted terminal truth |
| I / I9 | Isaac live viewer, point-cloud/snapshot presentation, ghost/reset/status interaction and observer performance budgets | Old singleton WebSocket data plane, pose-source writes and supervisor command ownership |
| R / I10 | Real-robot status, lifecycle and operator-client presentation reused from the generic gates | xArm command-gate/session policy or vendor-specific authority in Composer |

The exact former TypeScript components, stores, tests, Python routes, and
assistant bridge sources remain available at the pinned commit. Porting work
must cite the consulted paths and carry their still-required failure/safety
cases into focused tests; it must not copy the old endpoint registry or state
owner wholesale.

## Superseded bodies, split replacements, and acceptance-source mapping

These rows describe historical executable bodies, not a blanket list of paths
that must stay deleted. At this checkpoint, some paths are absent, some contain
deliberately narrowed structural or payload-neutral replacements, and some are
retained only as source material. "Does not return unchanged" applies to the
former owner and contract; it does not mean that the path or its required user
behavior must disappear. The exact former source is retained by the pinned
commit and must be consulted at the named gate.

| Historical path at `082221...` | Current checkpoint state | Why the former executable body is not current | Required destination/evidence |
| --- | --- | --- | --- |
| `manyforge_ros/manyforge_ros/ros2_control_kernel_factory.py` and its test | The factory and its exclusive test are absent | Python constructed the deleted RoboPlan/Kernel runtime and materialized the superseded Stop profile | I3 retained control/runtime products, I6/N runtime adapter construction, and I8/P5 Kernel-owned Stop evidence |
| `tests/test_integration.py`, `tests/test_pick_and_place.py`, and `tests/fixtures/pick_and_place.program.yaml` | The two executable tests are absent; the YAML fixture remains in-tree as legacy source material, not current acceptance evidence | The tests directly instantiate the v1 runner, mock Kernel client, singleton robot/group addressing, and old scene-edit DTOs | I5/H0 now carries independent scene-client cases; I6/N must carry addressed program/native acceptance and I8/P5 execution/Stop recovery cases |
| `manyforge_behavior/manyforge_behavior/nodes/scene.py`, `nodes/pick_planning.py`, `nodes/selection.py`, and their exclusive tests | The named nodes and their exclusive tests are absent | These nodes bind directly to the deleted scene/factory and singleton target contracts | I5/H0 now supplies the read-only scene client; I6 must supply component/target-addressed node implementations and N/P5 acceptance |
| `manyforge_composer/backend/state.py` | The path contains a structural catalog/deployment shell; H0 reads live in separate bounded headless clients | The former body also owned runtime clients, scene state, discovery, Runner/CycleManager construction, and robot control | I5/H0 now restores owner-issued runtime, scene, alarm, history, and node-environment reads without putting them back in `state.py`; I6 restores addressed authoring/execution clients |
| `manyforge_composer/backend/routes_scene.py` | The path remains absent; `routes_headless.py` exposes the H0 read projection | Its routes exposed the removed Composer-owned scene contract | I5/H0 now supplies scene access as a client of headless scene truth; I6/N must add addressed scene intent, and the old Composer scene owner does not return |
| `manyforge_composer/backend/viewer_data_plane.py` | The path contains payload-neutral demand, lease, and bounded-fanout primitives only | The former body also defined the Composer viewer schema, encoder, runtime sampler, and live route contract | I9 binds the retained transport mechanics to an observer-owned viewer/point-cloud contract; the old WebSocket protocol does not return |
| `manyforge_composer/backend/viewer_worker.py` | The path contains bounded envelope helpers and a typed-unavailable I9 construction fence | The former visual snapshot builder, versioned worker protocol, and process supervisor depended on Composer-owned runtime/scene state | I9 supplies an observer-owned snapshot adapter and restores qualified worker construction without restoring the old owner |
| `manyforge_composer/frontend/src/components/{ProgramPanel,PropertyPanel,DeploymentPanel}.tsx` and their stores/tests | The named v1 panels are absent; dependency-independent authoring validators and reducers are retained separately | Their interaction intent remains required, but their compiled types and stores are coupled to v1 program/deployment/runtime identities | I6/N TypeScript authoring clients on explicit component/target identities, reusing the historical UX and validation cases |
| `manyforge_composer/frontend/src/components/{RobotControlPanel,TransportControls,KernelErrorPanel}.tsx` and related stores/tests | N6-c1/c2 restore `RobotControlPanel` as a thin four-mode client; `TransportControls` remains absent; `KernelErrorPanel` is a generic diagnostic presentation shell | The replacement Robot Control surface keeps drafts and request-generation fencing locally, but preview, operation admission, progress, and Stop remain owner-issued; neither panel owns policy or command authority | N6-c4 owns the remaining live-viewer/manual-control composition, I7/L0 lifecycle requests, and I8/P5 final execution/Stop projection; no browser command authority or fallback selection |
| `manyforge_assistant_bridge/bridge.py` and its live HTTP tests | `bridge.py` remains a typed-unavailable, effect-free provider entrypoint; `headless.py` is the separate bounded H0 read client, and split protocol/supervisor tests remain | The former provider loop is coupled to removed Composer callback/tool contracts | I5/H0 now supplies read-only owner observation; I6/N must restore bounded provider mediation and Program tools against the new contracts |

This table is a source-and-acceptance map, not permission to treat every file
in the same directory alike. Dependency-independent utilities, reducers,
security logic, artifact clients, documentation, and focused tests stay
in-tree when they can retain their role without reviving the old authority.

## Exact deleted-backend source disposition

The backend deletion comprises exactly the following 18 Git blobs. Line
numbers in the cohort ledger below are one-based source lines in these blobs,
not line numbers in the current worktree.

| Historical path | Blob | Lines |
| --- | --- | ---: |
| `manyforge_composer/backend/assistant_provider.py` | `c6bf95a0968ffa2a6a72f2c39b9c410daa455309` | 1290 |
| `manyforge_composer/backend/assistant_tool_schemas.py` | `b6106df2abcc5837bd15430ed91ae5c6f5537bd6` | 2067 |
| `manyforge_composer/backend/assistant_tools.py` | `71d5409385988c86d207ae38dc3df540e02ef822` | 592 |
| `manyforge_composer/backend/intervention_validation.py` | `6278ed3a760b56366db090b60988efd0cec1aeaa` | 55 |
| `manyforge_composer/backend/mcp_server.py` | `d036dbe25d0536a47d53ab0ed887361c7bd5766c` | 1454 |
| `manyforge_composer/backend/routes_assistant.py` | `0dc3e7b93319f82da27fa34f72229719cc6ef581` | 5225 |
| `manyforge_composer/backend/routes_cycle.py` | `014859de855771944955a4afb0b613f67c39d453` | 417 |
| `manyforge_composer/backend/routes_debug.py` | `a6dacc723b253ca4751ffb813ac6e184d71ca045` | 566 |
| `manyforge_composer/backend/routes_direct.py` | `b624201c7aed2b40ccd81efa068f1d71a3e1bb39` | 726 |
| `manyforge_composer/backend/routes_execution.py` | `31cdcb6c281721428ced97a099d30134031b4804` | 721 |
| `manyforge_composer/backend/routes_infra.py` | `8e0c093511900673245413e59df298340f89b50c` | 529 |
| `manyforge_composer/backend/routes_kernel.py` | `581b277046d42724e8083bfc8076b77c12fe33c3` | 182 |
| `manyforge_composer/backend/routes_mcp.py` | `bbac34457b8f121b6403fb6f36311e6788a2d116` | 369 |
| `manyforge_composer/backend/routes_program.py` | `3c1de788e21c6e70d553f7e8e0b253b46f7cc2fc` | 1627 |
| `manyforge_composer/backend/routes_runtime.py` | `dace01aa5e4540cf6201fa687ea7b4b2d2e5b856` | 527 |
| `manyforge_composer/backend/routes_scene.py` | `831ac7bb59b133fc2cfc8646d9ee055792d8f786` | 2131 |
| `manyforge_composer/backend/routes_simulation.py` | `fb61a4f4f61327a2a473e6889ae7b20d8bc8570d` | 91 |
| `manyforge_composer/backend/routes_skills.py` | `13b57e6368968357dd557cc0dccefe5581c2d1d4` | 112 |

“Re-extract” below means reimplement or adapt the named, same-role client,
authoring, validation, transport, or presentation behavior in Python after the
new contract exists. It does not mean restore the historical module. “Stays
deleted” means the old semantic owner, in-process state access, singleton
addressing, compatibility alias, or exclusive wire shape is not a fallback.
The pinned tests named for each group are acceptance inputs; they are not
currently runnable qualification.

### Assistant provider, tools, and MCP

- `assistant_provider.py`: lines 1–132 (`_partition_runtime_noise`,
  `AssistantProviderError`, `AssistantProviderContext`, and
  `AssistantProviderResult`) preserve bounded warning/error/result-envelope
  intent for a future Python N provider client. The warning classifier is now
  retained, owner-neutrally, as
  `assistant_client_primitives.partition_runtime_warnings`; the old context
  and result fields must still be replaced by owner-issued facts. Lines
  133–381 (`AssistantProvider`,
  `MockAssistantProvider`, and `DeterministicSceneAuthoringProvider`) are old
  in-process/mock and scene-proposal contracts: their deterministic review
  behavior is acceptance input at N/I9, while the old provider and scene DTOs
  stay deleted. Lines 382–819 (`NemoClawAssistantProvider`,
  `_validate_lane_entry`, and `HermesAssistantProvider`) contain bounded HTTP
  request/cancel/response handling to re-extract at N; the old endpoint,
  lane-registry, callback, and catalog shapes do not return. Lines 820–995
  (`build_assistant_provider`, `_error_from_response_json`, and
  `assistant_provider_catalog`) supply selection/error/catalog UX cases, but
  the environment-driven Composer provider factory stays deleted. Lines
  996–1174 contain endpoint/timeout/message/tool-call/proposal normalizers.
  `_flatten_provider_message` is now retained as
  `assistant_client_primitives.flatten_provider_message`; the remaining
  contract-bound normalizers await the new authenticated envelope. Lines
  1175–1258 are the
  old mock proposal/message generator and remain test-vector input only. Lines
  1259–1290 contain scalar/list context readers that may be re-extracted with
  the new context type. Acceptance comes from the pinned
  `test_assistant_provider_http_fixture.py`,
  `test_assistant_lane_catalog.py`, and `test_assistant_routes.py`; H0 owns only
  the closed read adapter, while N owns provider mediation and Program/scene
  proposal effects.

- `assistant_tool_schemas.py`: lines 1–140 contain defensive schema copying,
  description-independent hashing, and field-policy narrowing.
  `schema_projection.strip_schema_descriptions` now retains the
  owner-neutral copy/description behavior; the historical hash and
  orientation-field narrowing remain unqualified contract input for
  owner-issued N schemas. The generic description/property helpers from
  lines 141 and 443–470 are likewise retained in `schema_projection` as
  `with_description`, `with_replaced_description`, `property_schema`, and
  `projected_property_or_fallback`. Lines 141–428
  are v1 parameter, blackboard-binding, catalog-entry, and tree-tool
  projections; catalog-driven authoring intent survives at N, but the v1
  parameter/blackboard DTOs and string-form compatibility fallback do not.
  Lines 429–606 are scene-schema projection helpers; schema projection and
  fail-closed field narrowing remain required at N, while the old world-frame,
  pose-alias, and scene-resource wire shapes stay deleted. Lines 607–1966 are
  the static old tool-schema constants and `ASSISTANT_TOOL_INPUT_SCHEMAS` map;
  their tool intents are requirements input, not a public contract. Lines
  1967–2067 (`assistant_tool_input_schema` and both
  `enrich_assistant_tool_descriptor*` functions) contain defensive descriptor
  enrichment to re-extract at N after the headless owners publish the exact
  schemas and identities. The old tool IDs, accepted aliases, and schema hash
  are not assumed compatible. Acceptance comes from the pinned
  `test_assistant_catalog_schema_projection.py`, `test_assistant_routes.py`,
  and `test_mcp_server.py`.

- `assistant_tools.py`: lines 1–144 (`AssistantToolError` and
  `assistant_tool_catalog`) retain error/catalog presentation intent, but the
  old built-in IDs and descriptors stay deleted. Lines 145–182
  (`inspect_isaac_scene_report`) are an I9 read-client requirement; the
  dynamic import of the old ROS inspector is not restored. Lines 183–310
  (`propose_scene_object_resources` and `propose_scene_object_nodes`) preserve
  deterministic, review-before-mutation proposal behavior for N/I9, but
  their old scene/tree outputs do not. Lines 311–592 contain that dynamic
  importer plus shape, pose, finite-number, required-string, ID sanitization,
  uniqueness, and proposal conversion helpers. The owner-neutral numeric and
  string validators are now retained as
  `assistant_client_primitives.{finite_number,finite_number_list,
  required_string}`. Shape/pose validation and ID-conflict behavior still
  require adaptation at N/I9 against the new frame, shape, and identity
  contracts; the import-by-path adapter and legacy `Pose` parser/wire stay
  deleted.
  Acceptance comes from the pinned `test_external_node_assistant.py` and
  assistant-route/tool cases.

- `intervention_validation.py`: all lines 1–55, including
  `merged_intervention_blackboard_keys` and nested `visit`, are inseparable
  from v1 `TreeNode`, `NodeCatalog`, implicit blackboard-key creation, and the
  old direct-intervention contract. That Python semantic validator stays
  deleted. Its local-writer and no-partial-validation cases must be translated
  to C++ Program Binding/intervention evidence at N/P5; a future Python client
  may only display the owner-issued validation result.

- `mcp_server.py`: lines 1–91 contain dispatch-outcome/audit classification,
  trigger-reason, and live-scene mutation-message helpers. Re-extract generic
  result/error classification at N, but do not reuse caller-minted principal
  context or old trigger/scene enums. Within `create_mcp_server` (lines
  92–1332), lines 116–173 are the audited wrapper to recreate with
  server-derived identity; lines 174–199 are the scene-read intent; lines
  200–616 are old scene/program/cycle mutations; lines 617–675 are
  capability/skill reads; lines 676–1001 are intervention/runtime-override
  commands; and lines 1002–1332 are execution/debug/catalog/snapshot/file/
  deployment/Kernel/session reads and commands. Recreate the still-required
  closed H0 read subset through `mcp_client_primitives` and recreate the
  remaining reads as authenticated thin N/P5/I9 clients and the still-required
  commands as bounded intent at their owner gate. The in-process
  `ComposerState`, old tool names/signatures, direct mutation, CycleManager
  settlement, runtime override, and session authority stay deleted. Lines
  1333–1454 contain response serializers and camel/snake normalization. The
  protocol-neutral part is now retained as
  `mcp_client_primitives.{camel_to_snake,snakecase_keys}`; serializers still
  adapt to owner-issued DTOs. Pinned `test_mcp_server.py` and
  `test_mcp_http.py` carry
  discovery, auth, audit, read, mutation-fence, intervention, and Stop cases.

- `routes_mcp.py`: lines 1–35 define the old transport/session constants.
  Lines 36–207 (`create_mcp_router`, JSON-RPC POST, and SSE GET) preserve
  bounded HTTP/SSE transport, lazy construction, eviction, and disconnect
  behavior to re-extract at N if a public MCP transport returns. Lines 208–246
  (`_McpSession`) preserve bounded
  queue/touch/disconnect mechanics, and lines 247–369 (`_handle_jsonrpc`)
  preserve JSON-RPC validation/error behavior. The in-process MCP server,
  fallback tool registry, unbounded old session identity, and principal
  propagation through `current_mcp_principal` stay deleted; the replacement
  uses server-configured authentication and attributed requests. Acceptance is
  the pinned `test_mcp_http.py`, `test_mcp_server.py`, and security cases.

### Assistant router and authoring algorithms

- `routes_assistant.py`: lines 1–335 contain environment bounds, catalog/read
  dedupe, hashes, fingerprints, and compaction. Re-extract the bounded-cache
  and repeated-read behavior at N after provider result DTOs are frozen; old scene and
  catalog fingerprints are not wire commitments. Within
  `create_assistant_router`, lines 336–707 are chat, tool/mode/provider/status,
  cancellation, and request-terminalization client behavior for N; the
  Composer-owned request ledger/provider factory stays deleted. Lines 708–736
  are caller principal-binding endpoints and stay deleted outright. Lines
  737–1173 are the old bridge callback dispatcher: authenticated catalog
  binding, schema checking, timeouts, and bounded execution remain acceptance
  requirements, but direct `ComposerState` mutation and the callback wire do
  not. Lines 1174–1331 cover Isaac inspection and proposal stage/list/discard/
  materialize interactions; restore those review workflows at N/I9 as
  clients of real owners, never as Composer scene authority.

- In the same `routes_assistant.py` blob, lines 1332–1731 contain catalog
  hashing, field-policy normalization/intersection, schema enforcement,
  catalog/skill/deployment/request payload projection, provider error mapping,
  and bridge context resolution. Re-extract their pure validation and
  presentation intent at N against new identities. Lines 1732–2302 contain
  read-tool dispatch, Program snapshot projection, schema selection/hash/diff,
  and argument normalization; preserve fail-closed schema validation and read
  result behavior, but replace every in-process state read and old tool schema.
  Lines 2303–2436 are the old tool/draft dispatcher and lock boundary; retain
  atomic request intent at N, not its callback registry or Composer lock.
  Lines 2437–3196 contain scene-resource/shape/pose argument parsing,
  orientation merging, frame normalization, edit fencing, and response
  serialization. Re-extract the authoring and validation algorithms at N only
  after the new frame/scene contracts exist; old aliases, world-frame
  normalization, and Composer scene mutation stay deleted. Lines 3197–3495
  apply scene/Program draft changes; atomic-draft behavior remains an N client
  requirement, while the old `Program`/scene objects and mutation owner do not.
  Lines 3496–4611 contain tree-node parsing, lookup, allowed-kind checks,
  deterministic insert/move/delete/replace/change-kind/wrap transforms, and
  override discard rules. Those dependency-independent authoring behaviors
  must be re-extracted at N; v1 node kinds, policy stubs, compatibility aliases,
  and direct state writes stay deleted. Lines 4612–4885 contain Program
  parameter and blackboard draft transforms; carry atomic authoring behavior
  into N's Program-Binding client, not the v1 blackboard schema. Lines
  4886–5121 contain provider context/resolution helpers to adapt at N. Lines
  5122–5225 contain proposal validation/materialization and capability lookup;
  retain review/materialization intent at N, with I9 supplying Isaac facts,
  but remove Composer-owned proposal/scene effects. Pinned
  `test_assistant_arg_normalizer.py`,
  `test_assistant_catalog_schema_projection.py`, `test_assistant_routes.py`,
  `test_assistant_lane_catalog.py`, and `test_external_node_assistant.py`
  provide the case inventory.

### Runtime, execution, lifecycle, and observability routes

- `routes_cycle.py`: lines 1–60 (`_structured_command_error`) preserve
  structured error presentation for an N/P5 thin client. Lines 61–144
  (`_require_no_blocking_operator_alarms`,
  `_refresh_visual_scene_after_reset`, `_cycle_command_payload`, and
  `_perform_cycle_reset`) combine useful response intent with Composer alarm,
  reset, and CycleManager ownership; only the response/error projection may be
  re-extracted. Lines 145–417 (`create_cycle_router` and status, run, Stop,
  full-Stop, reset, recovery, and velocity-scale handlers) define operator
  interactions to restore at N/P5, but their CycleManager calls, alarm
  backstop, reset ordering, Stop settlement, and singleton wire stay deleted.
  C++ Program/runtime owns execution at N and the C++ Kernel owns Stop at P5;
  pinned backend, bounded-async, cycle-fencing, and execution-observability
  cases are acceptance input.

- `routes_direct.py`: lines 1–112 contain capability lookup plus session,
  admission, and delegated-recovery response helpers. Re-extract only
  owner-issued response/error presentation; caller sessions, leases, arm
  admission, and parent-run inference stay deleted. Lines 113–234 are the old
  intervention execute path, lines 235–597 are joint/cartesian jog paths, and
  lines 598–726 are heartbeat, promote, status, pending-recovery, and abort
  paths. Manual/intervention request UX and fail-closed race cases return as
  addressed N/L0/P5 clients; direct runner/Kernel calls, artifact locks,
  renewable caller authority, and implicit move-group addressing do not.
  Acceptance comes from pinned direct/intervention, MCP, command-authority,
  and robot-control-candidate cases.

- `routes_execution.py`: lines 1–136 (`_iter_tree_names`, execution-history,
  channel, and alarm payload builders) contain read/presentation behavior. H0
  now carries the alarm/event subset; execution-specific projections remain
  N/P5 work. Lines 137–187 contain alarm error and query
  parsers. Their substantive wall-time/filter behavior is retained in
  `event_recorder.HistoryQuery` and `parse_wall_timestamp_ns`; only the future
  thin route's CSV/HTTP-error adaptation remains. Lines 188–508 build
  blackboard and execution digests from many in-process owners; their stable
  operator summaries remain requirements, while the cross-owner join and v1
  blackboard/runtime truth stay deleted. The behavior in lines 509–628 is now
  carried by the bounded H0 alarm snapshot and exact-revision acknowledgement
  clients, with alarm truth remaining headless. Of lines 629–721, H0 carries
  event history; run/channel/digest/blackboard/latency reads remain N/P5 work.
  Pinned `test_execution_observability.py`,
  `test_observability_routes.py`, alarm tests, and the frontend alarm ledger
  below carry filtering, pagination, race, and presentation cases.

- `routes_kernel.py`: lines 1–86 (`build_kernel_snapshot_payload` and
  `build_kernel_metrics_payload`) preserve structured error, progress,
  position-age, and metric presentation to adapt to a P5 owner snapshot.
  Lines 87–182 provide snapshot/metrics/event reads and the old Stop handler.
  Recreate reads and one thin Stop request at P5; the Composer coalescer,
  `run_kernel_stop_backstop`, direct Kernel access, and browser/backend Stop
  settlement stay deleted. Pinned execution-observability/backend tests and
  the 30 Stop-profile cases below are acceptance sources.

- `routes_runtime.py`: lines 1–225 (`_component`, model-serving file probe,
  execution-profile, and native-viewer helpers) preserve component/status
  presentation ideas, but their file/env/singleton truth sources do not.
  Lines 226–506 (`build_runtime_status_payload`) are the old composite
  Composer runtime owner and stay deleted; H0 now provides owner-issued status
  through `routes_headless.py` and reuses only presentation behavior. The GET
  interaction in lines 507–527 has returned as a thin H0 route; the POST
  external-provider update
  endpoint does not return because callers cannot write runtime truth. Pinned
  `test_model_serving_status.py`, `test_observability_routes.py`, and runtime
  frontend cases carry the acceptance behavior.

- `routes_infra.py`: lines 1–57 contain the bounded OpenAI-compatible model
  probe and endpoint-capability formatter. They are focused Python adapter
  behavior to re-extract at N if still required, not runtime truth. Lines
  58–127 synthesize safety and route-executor heartbeat state in Composer and
  stay deleted. Lines 128–475 build old composite infrastructure status and
  heartbeat payloads; operator detail and freshness behavior are acceptance
  inputs. H0 Manager/headless owners now issue their bounded runtime, scene,
  alarm, and historian facts; later provider-specific facts must come from N.
  Lines 476–529 are thin status/heartbeat/model-endpoint interaction sources,
  not permission to restore their composite payload. Pinned model-serving and
  observability route tests carry
  probe, timeout, stale, and failure cases.

- `routes_debug.py`: lines 1–229 build scene trace, export, Medkit, and
  collision-comparison read payloads. Re-extract useful serialization and
  query behavior at H0/N/P5/I9 against owner snapshots, not by joining
  `ComposerState`. Lines 230–406 contain scene-authority and planning-capture/
  cache qualification payload helpers; their scenarios remain qualification
  requirements, while Composer qualification ownership stays deleted. Lines
  407–538 expose the corresponding reads and effectful capture/cache/collision
  commands; future diagnostics are thin owner clients at their gates. Lines
  539–566 publish old debug signals and stay deleted as a runtime mutation
  path. Pinned observability, collision, scenario, and Medkit cases are
  acceptance input.

### Program, scene, simulator, and skill routes

- `routes_program.py`: lines 1–139 contain capability/mutation guards and
  sandboxed load/save path coercion. Re-extract path safety and clear error
  behavior at N; Composer deployment-idle authority does not return. Lines
  140–741 contain tree, Program, deployment, gripper, assistant, viewer,
  compatibility, and blackboard serializers/validators. Their authoring and
  omission behavior is porting input, but every v1 DTO, singleton robot/group,
  compatibility field, and cross-owner synthesis stays deleted. Lines
  742–999 provide Program load/read/tree/validation interactions for N. Lines
  1000–1186 are the old runtime-override owner and stay deleted; bounded
  intervention intent is reintroduced only through N/P5 owners. Lines
  1187–1329 provide saved Program/deployment reads for an N artifact client.
  Lines 1330–1512 provide tree/deployment update and save interactions whose
  atomic authoring and path-safety behavior must be re-extracted at N, without
  in-process runtime rebuilds. Lines 1513–1627 are the old Composer Apply
  session and stay deleted; durable Apply belongs to the C++ Component Manager
  at I4, with a thin client exposed at H0/N. Pinned backend, Program,
  assistant, artifact, and frontend Program-binding/store cases carry
  validation, atomicity, omission, and failure evidence.

- `routes_scene.py`: lines 1–106 (`augment_pose_orientation_rpy_deg`) contain
  pure presentation conversion to re-extract at N/I9 after the new pose/frame
  type is fixed. Lines 107–318 contain timing, freshness, visual-route, and FK
  diagnostic helpers to adapt for an I9 observer; their old state sources do
  not return. Lines 319–531 build canonical-world/frame-role projections from
  loaded singleton models and stay deleted as scene truth; H0/N replaces them
  with the logical-scene owner's addressed frame projection. Lines 532–752
  serialize shapes, objects, pose sources, robot metadata, and execution
  authority; preserve presentation intent but replace every old DTO and
  inferred authority. Lines 753–1181 build and publish visual state from
  Composer caches/FK and stay deleted; I9 owns qualified observer snapshots.
  Lines 1182–1498 build scene/gripper state and diagnostics; re-extract only
  thin presentation at H0/N/I9/I11. Lines 1499–1564 are authoring-preview FK
  behavior for an N/I9 client, not a scene owner.

- Within `routes_scene.py` lines 1565–2131, `get_scene_state` (1587–1590),
  `get_scene_diagnostics` (1847–1850), and `get_scene_frames` (1852–1855)
  return as thin H0/N reads; `preview_link_poses` (1777–1845) returns as an N
  authoring-preview request; and `get_scene_snapshot` plus
  `get_published_scene_snapshot` (2073–2131) return at I9. The
  `update_pose_source_snapshot` and `apply_pose_source_updates` bodies
  (1592–1775) stay deleted as Composer scene/pose-source ownership.
  `set_frame_visibility`, `set_frame_gizmo_size`,
  `set_object_frame_visibility`, and `set_object_axes_gizmo_size`
  (1857–1898 and 1947–1965) become local presentation state already split
  into frontend viewer-draft utilities, not scene persistence.
  `command_gripper` (1900–1945) returns as addressed N/I11 intent.
  `update_program_scene_resource` and `edit_scene` (1967–2071) return only as
  scene-owner transactions at N; their direct Composer mutation wire stays
  deleted. Pinned backend, pose-source, viewer, scene, gripper, and frontend
  object-discard/viewer cases are the acceptance sources.

- `routes_simulation.py`: all lines 1–91 (`create_simulation_router` and reset,
  state, step, and entity-state handlers) record required simulator operator
  interactions for I9. Recreate them as addressed, authenticated thin clients
  of the qualified simulator adapter. The singleton `ComposerState` control,
  old request/response wire, and Composer simulator authority stay deleted.

- `routes_skills.py`: lines 1–70 (`_derive_contact_tier` and
  `build_capabilities_payload`) hard-code capability categories, adapter IDs,
  and contact-tier inference from the v1 loaded deployment. That semantic
  synthesis stays deleted; N/I11 compiler products and command schemas own the
  facts. Preserve only the category/skill presentation intent. Lines 71–112
  expose capability and skill reads that return as thin H0/N clients of
  retained catalogs and owner-issued capabilities. Pinned backend, assistant
  catalog, skill, and support-contact cases carry acceptance.

These mappings deliberately leave no “restore the backend directory” action.
Each retained behavior names a future thin-client or authoring extraction;
each deleted owner names the headless gate that must exist first. The complete
historical blobs remain input only.

The immediately owner-neutral helper audit is source-complete but unqualified:
`backend/assistant_client_primitives.py`, `backend/schema_projection.py`, and
`backend/mcp_client_primitives.py` contain the exact extractions identified
above, with focused tests in `tests/test_assistant_client_primitives.py`,
`tests/test_schema_projection.py`, and
`tests/test_mcp_client_primitives.py`. The historical schema hash was
deliberately not copied. Builds and tests have not run, so these files are not
qualification evidence. The substantive history-query parsing already lives
in `event_recorder.HistoryQuery` and `parse_wall_timestamp_ns`; a future thin
route supplies only CSV and HTTP-error adaptation.

The following larger exact cohorts still have no current retained successor.
They require a new contract or a clean unavailable boundary, so this is an
explicit future extraction list rather than permission to leave them only in
Git history:

- bounded same-role clients/transports are
  `assistant_provider.NemoClawAssistantProvider`'s HTTP request, cancel,
  timeout, and response-normalization methods; `routes_mcp._McpSession`,
  `_handle_jsonrpc`, and the transport/session-eviction portion of
  `create_mcp_router`; and the catalog/read dedupe helpers at
  `routes_assistant.py:139-334`. Those provider/session entrypoints remain
  fenced before network or state effects until N; H0 implements only a
  separate closed read adapter, and the bounded mechanics are not semantic
  owners; and
- authoring algorithms awaiting a wire-neutral document type are
  `routes_assistant.{_find_tree_node_ref,_collect_tree_node_names,
  _collect_node_kinds,_tree_contains_name}` and the atomic
  `_apply_tree_draft_*` family at lines 3860–4611. The current
  `frontend/src/utils/authoringGraph.ts` retains topology/layout, not these
  insert/update/delete/move/replace/change-kind/wrap transforms. N must either
  extract their still-valid behavior or record an exact replacement test for
  every transform before retiring this marker.

By contrast, old orientation/tool-field schemas, scene/pose aliases,
Program/deployment serializers, caller identity/session fields, tool status
enums, and `_sanitize_object_id`/`_unique_id` are contract or identity policy,
not automatically reusable helpers. They remain acceptance input until their
new owner freezes the corresponding type.

## Current frontend disposition at the relocation checkpoint

The following tables enumerate every former frontend path that is still absent
at this checkpoint. "Absent" describes the current source layout; it does not
mean that the interaction is removed from the target product. Each row says
whether the old executable contract is permanently superseded, which behavior
remains required, and which current extraction prevents that behavior from
being lost while its final adapter is gated.

No test in this section was run during the disposition-only pass. The paths are
source/evidence mappings for the owning gate, not current qualification claims.

### Mechanical ledger of all 124 deleted frontend cases

This ledger covers every top-level `it(...)`/`test(...)` definition in the 17
deleted `*.test.ts`/`*.test.tsx` files. For a reproducible digest, each
definition is normalized as the UTF-8 record
`<repo-relative-path>\t<decimal-source-line>\t<exact-test-name>\n`, using the
first string-literal argument and source order from commit `082221...`.
Per-file digests hash that file's concatenated records; cohort digests hash
only the listed records without renumbering. The concatenation of all 124
records in path order has SHA-256
`176cb10ea3afbecc1ee902bee76d9108f9873edd63c847e000bf2ce649c72e15`.

| Historical test path under `manyforge_composer/frontend/src/` | Blob | Definitions | Definition-ledger SHA-256 |
| --- | --- | ---: | --- |
| `components/DeploymentPanel.robotControlLimits.test.ts` | `bfee633ad47ef372ca784ad1b0024b3b098773b8` | 14 | `ff82ffb5af65dc2555d0d94d166104df57f6b457b9b2411428d1413f4791f23b` |
| `components/HeaderBar.operatorAlarms.test.ts` | `72ff85b4ad679a2f3a1875bc2299324e8df76571` | 4 | `5c14317b15dc8b0e4ec0e9f2de40a6b3fe9c0bacfbfe97d019752c6fbba34a43` |
| `components/NodeDiscoveryDiagnostics.test.tsx` | `62b89f1ea6ada53e5664d8a63fafabe2119b77e1` | 1 | `1352cbd785cc88d7117478c3d7eb1525282b81eb77ed3baa885d4c0cbd0fb391` |
| `components/ProgramPanel.binding.test.ts` | `bab874f0a723323a13547b9d33eb51b2017f747e` | 6 | `639e08652eea5ac6894f176b60947d73e51c735e1425ef21a2d6aa4ebf5615e6` |
| `components/RobotControlPanel.test.tsx` | `9ab166fa5cdcb43544f529ee680e560bf8b5c78f` | 1 | `136c2a9f89169518d9f1642f89031e1ef2294b370f6935bc05169e0ca3a4953d` |
| `components/ScenePanel.supportContactEvidence.test.ts` | `d833db78cc57acdd701c623cb024a6168f9fd22c` | 3 | `f2353a15ed0355eca801bd31ca60917a6fe078f5c60e3abd1e98452e0ffdfee5` |
| `components/TransportControls.runtimeGate.test.ts` | `7662095ae746a0c0740ae003ce719f29de6be3fb` | 3 | `7da8ea54c998799b1c325f0267a5f413723aa288cc07d3de7bf690f3878dec3e` |
| `store/commandAuthority.test.ts` | `cb411cf9a50c911fa1daad198e4bf333a8b4a066` | 4 | `a5f2dfc172495410bab5c75fd99639037b99ed180bcd4cc26b33c7d722085a0a` |
| `store/cycleState.test.ts` | `1b5b9d71dc41a37e108c72cf2a2574adff0c7125` | 3 | `4804bdddafb9c7806249d0ef132bb81f8e7e791354f8671737a2f7ee433dc5d6` |
| `store/operatorAlarmStore.test.ts` | `ef3f6a3f8467a92556355e71593cf15373e3127b` | 9 | `b9da23370dc496f213fe81f618707423e4e131cd2b1f72a8d94447023ef1f77e` |
| `store/pointCloudStore.test.ts` | `1adc931cb97e20c8b10eb4d09e42eacfc7aae786` | 5 | `931a86af675761372a2563d32a08a683241ec244aa126832c0e14e5600cae543` |
| `store/programStore.test.ts` | `e7a3c2b3a9d9dde39040abdcaeac3969ec515319` | 6 | `82936fd2d725ff5ff0611fb7c27be7df5e0fc87f0210f7cb9a6f1c8e14ea7151` |
| `store/robotControlCandidateStore.test.ts` | `86740188323e3ab923d3b2035479821bfb62f639` | 17 | `c61b41edc8120513baa3d6d6ea99c0b0ad7d1b01448dc7bb691da3d1fed5a677` |
| `store/sceneStateStore.viewer.test.ts` | `75ae6c4bd20f58b4d2532ecf5684a1684feb4e46` | 7 | `2f9037ae6ccd0cbe5a7c8cb938ae3ec46dafa8655347b7bd7849fcd82f9f5c9c` |
| `store/stopProfile.test.ts` | `ed32140996428575c0b3170026fd20de00f643d0` | 30 | `02220ba6a9c2985871e0ba2084d0386f04274889a571e2fbc06a46619f05a8d6` |
| `store/viewerFrameStore.test.ts` | `6ea42533796a0078e7dac03ccd26e1f2281e1fa6` | 10 | `82dd3cb7ba73fd3bacaf6060cdc1e47d7592c9831cbaacc10a05401a86de8028` |
| `utils/objectMoveDiscard.test.ts` | `7901d15b46c9344e0cd099bb7b046de7a5626a2c` | 1 | `8dad9d761aef232da4e433c0f5b6b612b1d3b63d5b72de8c528333e3d1696943` |

The semantic cohorts below partition those 124 records exactly once. “Port”
means preserve the user-facing or client-local behavior in TypeScript;
“translate” means move the asserted semantic decision to the named headless
owner while keeping only its projection in the browser. Passing a cohort at a
future gate requires focused tests carrying every listed case, adapted to the
new contract; merely retaining this ledger is not acceptance.

#### Component test cohorts

- `DeploymentPanel.robotControlLimits.test.ts`, `joint_caps`, lines
  18, 24, 33, 43, and 51; 5 records; digest
  `b4c315426be00f57b093cfc5a47ad912b40996a20e5817ce0471bf9923abefe1`:
  `18 sets a jerk cap and preserves it alongside other dimensions`;
  `24 preserves an existing jerk cap when editing velocity`;
  `33 does NOT drop a joint that still has a jerk cap after clearing vel/accel`;
  `43 drops a joint only when velocity, acceleration AND jerk are all empty`;
  `51 rejects non-positive / non-finite input by returning the same reference unchanged`.
  Port the pure cap-editor/omission behavior to N's addressed deployment
  authoring client; C++ validation owns the accepted limit semantics.

- The same file, `controlled_stop_reducers`, lines 62, 92, 115, 132, 152,
  171, 187, 204, and 229; 9 records; digest
  `c0a76bb789d4f7e0a7e224df282e9f4cf8d9574504d3a047ba6f16ad323e173f`:
  `62 documents every controlled-stop field with defaults, limits, and consequences`;
  `92 derives the visible auto max-stop deadline from the active execution lane`;
  `115 edits the ros2_control auto deadline as a total time greater than the requested stop time`;
  `132 creates a complete soft_on_path operator profile with per-joint decel defaults`;
  `152 creates a complete spring_back_hold operator profile with per-joint decel defaults`;
  `171 removes the profile slot when the mode is cleared`;
  `187 keeps explicit max_stop_time_s at or after the requested decel window`;
  `204 removes soft-only natural-completion when switching to spring_back_hold but preserves path limits`;
  `229 edits positive per-joint controlled-stop records and rejects invalid values`.
  Port field documentation and draft editing at N, then translate defaults,
  supported modes, deadlines, and validation to the C++ Kernel/Program-Binding
  contract at P5. The v1 stop-profile fields are not compatibility promises.

- `HeaderBar.operatorAlarms.test.ts`, `alarm_stack`, lines 116, 139, 168,
  and 196; 4 records; digest
  `5c14317b15dc8b0e4ec0e9f2de40a6b3fe9c0bacfbfe97d019752c6fbba34a43`:
  `116 does not render alarm acknowledgement controls with no attention alarms`;
  `139 shows only individual acknowledgement for one attention alarm`;
  `168 shows bulk acknowledgement for two attention alarms`;
  `196 shows the newest three attention panels and a separate standing alarm`.
  I5/H0 now ports the presentation stack and exact-record confirmation
  controls; the headless alarm owner supplies ordering, lifecycle, and
  acknowledgement facts. Focused React and alarm-client tests carry all four
  cases.

- `NodeDiscoveryDiagnostics.test.tsx`, `discovery_presentation`, line 38; 1
  record; digest
  `1352cbd785cc88d7117478c3d7eb1525282b81eb77ed3baa885d4c0cbd0fb391`:
  `38 renders active provenance and structured rejection details`. I5/H0 now
  projects the independently verified immutable v2 environment, resolved
  implementation identity, and dynamic runtime availability; the old
  dynamic-loader/source report stays deleted.

- `ProgramPanel.binding.test.ts`, `binding_candidates`, lines 47, 55, and 64;
  3 records; digest
  `1c7f991a4b4212ae332a9bacdca2ca45a330612e71d3459f6fdb23ba9065fcec`:
  `47 offers parameterizable, type-matching, unbound fields`;
  `55 excludes a field already bound to a program parameter`;
  `64 excludes type-mismatched and non-parameterizable fields`. Port the
  schema-driven candidate filter to N's Program-Binding editor, with C++
  binding validation as the authority.

- The same file, `unbind_literal`, lines 76, 80, and 84; 3 records; digest
  `610059e2c2b85dbb6381f8d4d682b91d2a32855d2445a58a621dd784cfe714cd`:
  `76 reverts a bound field to the bound program parameter default`;
  `80 falls back to the catalog default when the bound parameter is gone`;
  `84 returns a literal value unchanged`. Port this draft-only revert UX at N
  against v2 catalog defaults; do not restore the v1 blackboard/binding wire.

- `RobotControlPanel.test.tsx`, `layout`, line 46; 1 record; digest
  `136c2a9f89169518d9f1642f89031e1ef2294b370f6935bc05169e0ca3a4953d`:
  `46 keeps a ready joint candidate footer outside the bounded scroll region`.
  Port this presentation behavior with the addressed N manual-control client;
  candidate readiness comes only from the execution owner.

- `ScenePanel.supportContactEvidence.test.ts`, `support_contact_authoring`,
  lines 10, 19, and 38; 3 records; digest
  `f2353a15ed0355eca801bd31ca60917a6fe078f5c60e3abd1e98452e0ffdfee5`:
  `10 defaults new contacts to precise evidence`;
  `19 removes metric-only fields when lightweight evidence is serialized`;
  `38 serializes bounded penetration with only its mode-specific threshold`.
  Preserve the form/omission intent, but translate the exact contact-policy
  schema and validation to P5, with tool/contact variants requalified at T.

- `TransportControls.runtimeGate.test.ts`, `runtime_admission`, lines 6, 10,
  and 14; 3 records; digest
  `7da8ea54c998799b1c325f0267a5f413723aa288cc07d3de7bf690f3878dec3e`:
  `6 disables motion whenever runtime admission is explicitly closed`;
  `10 fails closed until an authoritative runtime exists`;
  `14 allows motion after runtime admission opens`. H0 explicitly reports that
  robot readiness is unavailable; N/P5 must supply owner-issued admission.
  TypeScript retains only disabled-state/reason presentation, and the
  browser-side `runtimeAllowsMotion` owner does not return.

#### Store and utility test cohorts

- `store/commandAuthority.test.ts`, `command_admission`, lines 30, 47, and 59;
  3 records; digest
  `96d0cd480ec9130fef7d43366d7e29c4f405d530bc0ac2a945532ba77a09e753`:
  `30 blocks robot commands when editor drafts have not been applied`;
  `47 blocks robot commands only on explicit stale or disconnected authority`;
  `59 blocks robot commands while the cycle owns the robot`. Translate these
  cases to Component Manager Apply plus N/P5 admission evidence. TypeScript
  may render the owner verdict but must not recreate a browser command gate.

- The same file, `reason_presentation`, line 71; 1 record; digest
  `2b13a9b65a55b059cd269fd5b84154360c76daa3a3a0abc5386feb6e35aa760f`:
  `71 uses user-facing reason labels for scene readiness failures`. H0 now
  carries generic typed-unavailable presentation; N adds addressed scene
  readiness reasons without moving policy into the browser.

- `store/cycleState.test.ts`, `boundary_parse`, lines 5 and 13; 2 records;
  digest
  `2e7ff86a39dbd643242dab3dbccd60cb2ca3a8fafb788055f1ed1ae7d0bdd9a1`:
  `5 accepts every backend cycle state currently exposed to Composer`;
  `13 rejects unknown states instead of silently casting them`. Port strict
  parsing at N/P5 only after the new execution-state enum is frozen; the old
  enum itself is not retained.

- The same file, `legacy_fail_closed_coercion`, line 17; 1 record; digest
  `6f6004e39843601185df927127d0fea99a84da83943f7f12080640c16aeec1c2`:
  `17 coerces unknown states to running so editing fails closed`. Preserve the
  fail-closed outcome, but do not restore unknown-to-`running` as fabricated
  runtime truth. The N/P5 client disables affected controls and displays an
  unknown/invalid projection.

- `store/operatorAlarmStore.test.ts`, `activation_fences`, lines 152 and 162;
  2 records; digest
  `b03d34c19fa7358dca5251d5a399a588c21e7f0da3abcc497fce182f0ed92932`:
  `152 reannunciates only a genuinely new activation, not repeated samples`;
  `162 detects membership, recurrence, revision, and generation changes in a bulk fence`.
  These client-local comparison behaviors are split into
  `utils/alarmFences.ts`; focused H0 tests retain exact activation and
  bulk-membership semantics.

- The same file, `motion_ui_fence`, line 177; 1 record; digest
  `547a3281bee0e2dd3790a1b523555557a931b0d80b4de57437e61308cb6296a7`:
  `177 fails closed only for motion-producing controls`. H0 retains scoped
  disabled-state presentation and explicitly leaves robot readiness
  unavailable; non-motion inspection remains usable. N/P5 supplies actual
  motion admission.

- The same file, `projection_ack_races`, lines 187, 208, 232, 254, 273, and
  303; 6 records; digest
  `0b37b5ba86bfd9801b8d4ded52597e3f04c3a1fd6de4dacf51d7d91c8eb515cc`:
  `187 derives attention and standing records while retaining server totals`;
  `208 binds individual acknowledgement to exact activation and revision`;
  `232 binds acknowledge-all to the confirmation-time membership and generation`;
  `254 accepts a later observation from a restarted process with a reset generation`;
  `273 does not let a delayed acknowledgement overwrite a newer poll`;
  `303 refreshes state after a stale bulk acknowledgement`. I5/H0 now carries
  these through the alarm projection/intent client with exact revisions and
  latest-wins response ordering; the browser does not own alarm lifecycle or
  motion admission.

- `store/pointCloudStore.test.ts`, `mfpc_decoder`, line 59; 1 record; digest
  `74b112be79266f3e83c0153b7ad690959da4ba7085cfcf9228da0d6f11f17818`:
  `59 parses the packed float32 and normalized uint8 blocks without copies`.
  The MFPC/v1 bytes do not return. Translate zero-copy decoding, normalized
  color, size bounds, and malformed-frame rejection to the exact I9 observer
  protocol and its performance tests.

- The same file, `bounded_frame_slot`, lines 73, 97, 113, and 127; 4 records;
  digest
  `da5b73272ba6e206b04272527061698a19ae1237a794a230c4d3aa9aa20783a4`:
  `73 rejects callbacks from a replaced point-cloud socket generation`;
  `97 keeps only the newest sequence and applies it once per render wake`;
  `113 accepts a fresh stream epoch and represents clear as an empty slot`;
  `127 rejects malformed or oversized payloads before changing sequence state`.
  The protocol-neutral newest-frame behavior is already split into
  `state/latestFrameSlot.ts`; I9 must carry these cases into the bound
  point-cloud client before declaring the viewer restored.

- `store/programStore.test.ts`, `pick_macro`, line 199; 1 record; digest
  `ee42dfa9ec5b65b58a98158a97c7380066a2112ea8c6dd83957b1fc3f0a44f42`:
  `199 atomically expands to visible portable nodes and one declared namespace`.
  Preserve atomic portable-workflow authoring at N and requalify tool-specific
  pick semantics at T; old node IDs, namespace DTO, and hidden macro state do
  not return.

- The same file, `viewer_draft`, line 258; 1 record; digest
  `a9f1d07c26e6d2c5df5ec8c7d5185c4ae63587cf12fbb1b42ae7718c1cc7d8d8`:
  `258 keeps viewer presentation as a draft until apply or save`. This
  client-local rule is already split into `state/viewerPresentationDraft.ts`;
  N tests must prove it never writes live scene/runtime truth.

- The same file, `deterministic_names`, line 284; 1 record; digest
  `3a60d71a0c9b6b7a9fab5d78586f7f49a0822d68485f7f4c6b7078829b996076`:
  `284 uses one deterministic suffix across repeated insertions`. Port
  deterministic draft naming at N against explicit Program node identities;
  it remains authoring behavior, not runtime identity minting.

- The same file, `atomic_capability_fences`, lines 301, 316, and 343; 3
  records; digest
  `e47431ed66fc3f4b8fc2c59c0456a88468a3d073da346b49324234984fe5ccc4`:
  `301 fails without partial graph changes when gripper context is ambiguous`;
  `316 fails atomically when the deployment lacks a required portable capability`;
  `343 does not author a catalog default whose capability is unavailable`.
  Port atomic draft reduction and catalog filtering at N, but take capability
  and binding facts only from compiled products; T requalifies gripper/tool
  variants.

- `store/robotControlCandidateStore.test.ts`, `draft_debounce`, lines 97 and
  110; 2 records; digest
  `9417a68bcb270c94391922dc554b9e2ffbce7c1ab79089bb07cfd5058d1c6da9`:
  `97 authors a candidate after debounce and never moves the robot`;
  `110 is latest-wins: rapid edits coalesce to the last draft`. Port bounded
  draft/debounce behavior at N using explicit component/target identities;
  authoring alone never dispatches motion.

- The same file, `dispatch_settlement`, lines 129, 147, 182, 237, and 284; 5
  records; digest
  `c34483bee86c9715b81ae17f676af82e7166cb296f3c01baf85ba6cf61f1b5d3`:
  `129 approve dispatches a ready candidate and marks it executing`;
  `147 ignores new authoring requests while an arm candidate is executing`;
  `182 maps an executing arm candidate to the server terminal outcome`;
  `237 polls a sessionless gripper execution to its server terminal outcome`;
  `284 approve fails closed (no executing) on a freshness rejection`. Port
  request/progress presentation at N/P5 and bind it to owner-minted operation
  identity and terminal state. The old candidate endpoint, shared Approve
  owner, sessionless gripper special case, and client-minted `executing` truth
  do not return.

- The same file, `invalidation_races`, lines 303, 329, 365, 401, 413, 434,
  446, 467, 486, and 502; 10 records; digest
  `2c39557e0bb3c0252bf42c5d04b81b47308e94264e66ceedddc5bc08e66da64c`:
  `303 author() makes the prior ready candidate non-approvable before the debounce`;
  `329 authoring a different move group best-effort reverts the prior server candidate`;
  `365 after the new create resolves, approve targets the NEW candidateId only`;
  `401 revert clears the candidate and never moves the robot`;
  `413 rejectDraft makes a prior ready candidate non-approvable and best-effort reverts it`;
  `434 approve() does NOT execute after rejectDraft (fail closed at the action)`;
  `446 keeps the prior ghost on the planning placeholder (no overlay flicker) while non-approvable`;
  `467 approve() does NOT execute during the debounce planning placeholder`;
  `486 revert skips backend revert for a blank-id / terminal candidate`;
  `502 revert does NOT cancel an executing candidate (no server revert, not cleared)`.
  Port the client race, no-motion, ghost-continuity, and exact-operation-target
  behavior at N/P5. Revert becomes cancellation/rejection of an exact addressed
  draft or operation; the old move-group and candidate wire stay deleted.

- `store/sceneStateStore.viewer.test.ts`, `demand`, line 30; 1 record; digest
  `8bd09104f93164f8334696fd188191186efff29815099bdc216fae2381151b25`:
  `30 keeps collapsed demand passive unless another authority needs frames`.
  Port demand/lease presentation at I9 using the retained payload-neutral
  viewer fanout; the browser is not a frame or scene authority.

- The same file, `socket_generation`, line 37; 1 record; digest
  `fe405d35ef20c6c32667746dcdaa6ca7cbb8df1e8ebfec9a2101ad7583b5d086`:
  `37 rejects callbacks from a replaced viewer socket generation`. Carry this
  stale-connection case into the I9 observer client.

- The same file, `viewer_v1_blocks`, lines 51, 62, and 74; 3 records; digest
  `cc9de6a5d37058a6dde7d37e1fe2d8fa63879d1f280b8017f2603e364bb1d1f8`:
  `51 materializes the generated ordered generation exactly`;
  `62 rejects a frame whose ordered entity block is incomplete`;
  `74 stores diagnostics without changing frame freshness or drop accounting`.
  The generated viewer-v1 wire stays deleted. Translate ordered completeness,
  diagnostics isolation, and drop accounting to the frozen I9 protocol.

- The same file, `health_liveness`, lines 103 and 131; 2 records; digest
  `250e8ac574a219eabaafe85738a1e9dc74f3800903f3d0f155f376b776f57fc5`:
  `103 marks retained visual evidence stale when producer health fails`;
  `131 keeps unchanged frames authoritative while transport messages remain live`.
  I9's observer owns freshness/producer health; the TypeScript client preserves
  evidence status and never derives scene truth from animation activity.

- `store/stopProfile.test.ts`, `view`, lines 49, 57, 74, 89, 102, 116, 132,
  149, 162, 174, 188, and 201; 12 records; digest
  `1a5821c738876c00f9412b449eb7586b9bf7d2666c4dc8fef60e0732580d90cd`:
  `49 returns an unavailable, empty view when there is no runtime status`;
  `57 extracts configured operator/fault modes and marks supported modes selectable`;
  `74 disables modes the backend does not advertise and attaches the backend reason`;
  `89 makes hard_deceleration selectable when advertised by the backend`;
  `102 falls back to the structured stopProfileUnsupportedReasons entry when no per-mode reason exists`;
  `116 fail-closes every mode to disabled when the capability query failed`;
  `132 never hides a configured or advertised mode outside the known set`;
  `149 reports unsupported when the backend flags the configured profile unsupported`;
  `162 is unavailable with no options when nothing is configured or advertised`;
  `174 fail-closes the configured mode to null on malformed (empty / non-string) slot values`;
  `188 ignores malformed configured stop deceleration values`;
  `201 always keeps the disabled === !supported invariant`. Port the complete
  option/reason presentation at P5, using owner-issued supported/configured
  modes. The browser profile table and fallback selection do not return.

- The same file, `run_gate`, lines 213, 217, 223, 230, 245, 252, 266, and 283;
  8 records; digest
  `282d19dc444e23fdf71c6bba515a96202aa66821cfca4c6d02699877c5990ec5`:
  `213 does not block when runtime status is not yet known (backend stays authoritative)`;
  `217 does not block a healthy, supported stop profile`;
  `223 blocks with fault severity when the capability query failed`;
  `230 blocks with degraded severity and the offending mode + reason when the profile is unsupported`;
  `245 prioritises the query-failed fault over the unsupported-profile degraded state`;
  `252 blocks without naming a phantom mode when unsupported is flagged but no slot is configured`;
  `266 explains when a required operator or fault stop profile is missing`;
  `283 explains when the mandatory hard-stop floor is unavailable`. Translate
  all admission/severity decisions to C++ Program Binding/Kernel evidence at
  P5; TypeScript displays the typed verdict. The historical client-side run
  gate is not restored.

- The same file, `execution_warning`, lines 302, 309, 319, and 326; 4 records;
  digest
  `9e197654c908ba370f632ab486615b91e33f4b08c4c5cd4fb63c8eb8285b2d4c`:
  `302 returns null when the stop executed as requested`;
  `309 warns when the backend executed a fallback stop mode`;
  `319 warns when a controlled stop transitioned to hard stop`;
  `326 includes the hard-stop floor when reported`. Port this P5 outcome
  presentation against the one Kernel Stop result; no browser fallback is
  selected.

- The same file, `hard_stop_label`, line 339; 1 record; digest
  `e722ab113a34367b12ef37a55cdf2ef46d584343a4b49c2760da902526c21085`:
  `339 labels the profiled and immediate hard-stop levels`. Retain this pure
  P5 presentation mapping, keyed by the final Kernel enum.

- The same file, `clear_latch`, lines 348 and 354; 2 records; digest
  `28da261041e3c69df6c6e2b97796a3982a836310a9d72bd14e6574a86b352a60`:
  `348 keeps the latch while running, stopping, or jogging`;
  `354 clears the latch once the cycle has settled to stopped or idle`. Port
  only the request-progress UI latch at P5; terminal settlement remains
  Kernel-owned.

- The same file, `velocity_slider`, lines 362, 367, and 372; 3 records; digest
  `37a4d5a13d458e7b733a2bf02e78bec14ea371f0cbd6762216a07fa74483f2ca`:
  `362 freezes while a stop is in progress regardless of cycle state`;
  `367 freezes while stopping but releases after the cycle is stopped`;
  `372 keeps the slider live while running, jogging, or idle`. Port scoped UI
  freezing at P5 from owner-issued operation state; it cannot gate or settle
  Stop.

- `store/viewerFrameStore.test.ts`, `frame_identity`, lines 25, 61, 79, 90,
  101, 115, and 129; 7 records; digest
  `3c11f229333aecf9c5d7a5cefb7acbf00561f23843fd78b39febf03d2229a7ea`:
  `25 keeps only the latest self-contained frame and applies it once per sequence`;
  `61 retains time-only authority frames without invalidating an identical scene`;
  `79 rejects mismatched ordered blocks before replacing the latest frame`;
  `90 invalidates a frame when a new manifest generation is published`;
  `101 makes manifest and frame resync duplicates idempotent`;
  `115 rejects manifest regression and conflicting reused identities`;
  `129 rejects a frame sequence reused with a conflicting render revision`.
  Carry newest-only, identity, generation, idempotence, and conflict behavior
  into I9, using `state/latestFrameSlot.ts` where protocol-neutral. The v1
  manifest/frame schema does not return.

- The same file, `snapshot_barrier`, line 139; 1 record; digest
  `439b15397a7c7cd74bc5fd36f8f22eaa58dee26d4154d1431a38a5889c409c03`:
  `139 resolves a snapshot barrier only after the required generation arrives`.
  Bind this at I9 to the retained capacity-one snapshot broker and the exact
  observer generation.

- The same file, `interpolation`, line 163; 1 record; digest
  `cf9400354c9d63b6735e035a905eb88512baeb040530504efc910d790de518c7`:
  `163 interpolates only between authoritative frames and never extrapolates`.
  Port this renderer-local algorithm at I9; it cannot manufacture owner state.

- The same file, `stale_evidence`, line 204; 1 record; digest
  `163ca542b97480dba834e3b90f333e3d700e176ef8a8057ac06339a4d5e93965`:
  `204 applies stale evidence immediately and rejects sequence regression`.
  Carry the case into I9 observer/client tests.

- `utils/objectMoveDiscard.test.ts`, `discard_intent`, line 13; 1 record;
  digest
  `8dad9d761aef232da4e433c0f5b6b612b1d3b63d5b72de8c528333e3d1696943`:
  `13 restores the live object pose to the program pose via scene/edit update_pose`.
  Preserve the user's discard/revert intent at N as reversion of an exact
  addressed authoring draft or Apply candidate. The `/api/scene/edit`
  mutation and Composer Scene Authority stay deleted.

The file counts sum to 124, and the cohort counts also sum to 124. There is no
unassigned frontend definition and no case whose old browser semantic owner is
treated as a future TypeScript authority.

### Required component surfaces awaiting qualified adapters

| Historical frontend path at `082221...` | Current disposition and restoration gate | Retained/extracted TypeScript responsibility |
| --- | --- | --- |
| `src/components/AssistantOverlay.tsx` | Read-only assistant observation is available through the I5/H0 headless client. The interactive provider/conversation overlay returns at I6/N; do not restore the old deployment flag/callback contract. | `src/components/assistantRequestRecovery.ts` and its test retain bounded authoritative-request reconciliation without an old assistant DTO. |
| `src/components/DeploymentPanel.tsx`; `src/components/DeploymentPanel.robotControlLimits.test.ts` | Deployment authoring returns at I6/N on explicit component/target artifacts. The singleton `robot_config`, adapter capability union, and old mutation endpoint do not return. | `src/utils/deploymentCapabilities.ts`, `src/utils/parameterAuthoring.ts`, `src/components/property/parameterCapValidation.test.ts`, and the v2 catalog/schema helpers retain capability filtering, optional omission, and cap behavior. |
| `src/components/FileBrowserDialog.tsx` | Read navigation is available now. Mutation-aware open/save selection reconnects at I6; the old save endpoint shape is not retained. | `src/components/ArtifactBrowser.tsx`, `src/api/client.ts`, and their parser/client tests retain sandbox-root navigation and explicit selection. |
| `src/components/HeaderBar.tsx`; `src/components/HeaderBar.operatorAlarms.test.ts` | The structural header plus H0 alarm, status, and history presentation are available now. Lifecycle and command integrations return through I7/I8 projections; the old all-stores monolith does not return. | `src/App.tsx`, `src/components/OperatorAlarmPanel.tsx`, `RuntimeStatusOverlay.tsx`, `RobotLifecycleCard.tsx`, `ExecutionHistoryPanel.tsx`, and `src/utils/alarmFences.ts` retain presentation, exact-record confirmation fencing, and reannunciation behavior. |
| `src/components/Layout.tsx` | The current structural layout is available. The authoring dock composition returns at I6/N; the old store-coupled root component is not copied. | `src/App.tsx`, `src/components/CollapsedRail.tsx`, and `src/index.css` retain the shell, collapse affordance, and semantic styling. |
| `src/components/NodeDiscoveryDiagnostics.tsx`; `src/components/NodeDiscoveryDiagnostics.test.tsx` | Permanently superseded as a v1 dynamic-loader/source report. I5/H0 now provides a verified retained-environment identity and runtime-availability view rather than recreating this payload; execution remains explicitly unavailable until I6/N. | `useHeadlessState.ts`, `HeadlessRuntimeSurfaces.tsx`, the immutable-v2 `CatalogPanel`, and strict parsers present only owner-issued facts. |
| `src/components/ObjectEditPanel.tsx` | Object editing/discard interaction remains required at I6/N. Direct mutation of Composer Scene Authority does not return. | `src/components/common/DraftInputs.tsx`, `src/utils/poseTransforms.ts`, `moveTo.ts`, `gizmoAuthoring.ts`, and `src/hooks/usePersistentDraft.ts` retain draft/pose interaction without an effectful endpoint. |
| `src/components/ProgramPanel.tsx`; `src/components/ProgramPanel.binding.test.ts` | Program and Program-Binding authoring return at I6/N. V1 blackboard DTOs, singleton robot/group fields, and Pick-Plan artifact shapes do not return. | V2 `catalogSchema.ts`, `catalogParameterValidation.ts`, `parameterAuthoring.ts`, and the graph/draft extractions below retain schema-driven authoring mechanics. |
| `src/components/PropertyPanel.tsx` | The property surface remains required at I6/N after exact Program-Binding and command-schema types exist. | `src/components/common/DraftInputs.tsx`, `src/components/property/utils.ts`, catalog schema/parameter validation, and cap tests retain dependency-independent editing behavior. |
| `src/components/RobotControlPanel.tsx`; `src/components/RobotControlPanel.test.tsx` | N6-c1/c2 restore addressed, unit-tagged joint-goal, pose-goal, Cartesian-path, and gripper draft/preview/approval interaction with a fixed action footer. N6-c4 owns the remaining 3D/live-viewer composition. The old candidate endpoint, group ID, angular-only gripper payload, and browser command gate do not return. | The active `RobotControlPanel.tsx`, generation-fenced `useRobotControl.ts`/`robotControl.ts` state machine, strict parsers, and translated tests preserve the historical authoring, freshness, approval, terminal, and Revert meanings while preview and execution remain server-owned. Target-safe pose/move/gizmo/joint/gripper utilities remain reusable inputs. |
| `src/components/SceneControls.tsx` | Viewer authoring controls reconnect at I6/N to the qualified Program presentation projection. Live scene state is not a second persistence owner. | `src/state/viewerPresentationDraft.ts`, `src/utils/viewerCamera.ts`, and `viewerPerformance.ts` retain local view preferences, camera pins, renderer selection, and render metrics. |
| `src/components/ScenePanel.tsx`; `src/components/ScenePanel.supportContactEvidence.test.ts` | Ordinary addressed scene authoring returns at I6/N. The former inline support-contact payload is deliberately not copied: exact `contact_policy` qualification belongs to I8/P5 and tool/contact fixtures are requalified at I11/T. | Generic draft, schema, pose, and graph utilities remain. The historical support-contact test at `082221...` is explicit reimplementation evidence, not an active schema contract. |
| `src/components/SceneViewer.tsx` | Authored-neutral structural viewing and H0 scene/initialization facts are available now. Addressed object authoring returns at I6/N; Isaac point cloud, ghost, snapshot, and simulator interaction pass I9/I. The viewer-v1 data plane does not return. | `src/components/StructuralViewer.tsx`, `SnapshotResponder.tsx`, `src/utils/{structuralTransforms,viewerCamera,viewerPerformance,snapshotCapture}.ts`, and `src/state/latestFrameSlot.ts` retain renderer, camera, capture, and protocol-independent newest-frame behavior. |
| `src/components/Sidebar.tsx` | Catalog/palette and authoring tabs remain target behavior. The read-only catalog is available now; Program/scene/deployment tabs reconnect at I6/N. | `src/components/CatalogPanel.tsx`, `NodePaletteItem.tsx`, the v2 catalog client, and profile/capability filters retain catalog browsing and drag identity. |
| `src/components/TransportControls.tsx`; `src/components/TransportControls.runtimeGate.test.ts` | Stop/Pause/Resume/Reset interaction returns on the final resource-addressed public contract at I6/N and is requalified against the one Kernel Stop envelope at I8/P5. Old cycle endpoints and browser-side runtime gates never return. | `src/components/RecoveryActionControls.tsx`, `KernelErrorPanel.tsx`, and generic request reconciliation retain presentation, exact-offer confirmation binding, and readable failure behavior without command authority. |
| `src/components/TreeCanvas.tsx` | The interactive graph adapter returns at I6/N after exact Program node/binding types exist. The former React-Flow/store wiring is not a compatibility surface. | `src/utils/authoringGraph.ts` and tests retain tree validation, single-parent/cycle/cardinality checks, ordering, and deterministic layout; `BtNode.tsx`, `StatusEdge.tsx`, `CanvasViewControls.tsx`, and pure `store/composerStore.ts` retain graph presentation state. |
| `src/components/Viewport.tsx` | Resizable authoring panels and viewer composition return at I6/N. The structural shell remains truthful while those clients are unavailable. | The active `App.tsx` layout, `CollapsedRail.tsx`, structural viewer, catalog, artifact browser, and CSS preserve the non-authoritative shell responsibilities. |
| `src/components/property/BlackboardBindingEditors.tsx`; `src/components/property/MotionEditors.tsx`; `src/components/property/PrimitiveParameterEditors.tsx`; `src/components/property/ProgramParameterBindingEditor.tsx` | These editor families return at I6/N against exact Program-Binding, component/target, coordinate-unit, and command-schema types. Former compatibility DTOs and singleton defaults do not return. | Draft inputs, primitive-list equality, schema/parameter validation, numeric caps, pose transforms, and unit-tagged joint/gripper drafts are retained with focused tests. |

### Exclusive old stores, protocols, and mixed clients

| Historical frontend path at `082221...` | Executable disposition | Required behavior and retained evidence |
| --- | --- | --- |
| `src/api/websocket.ts`; `src/types/viewerProtocol.generated.ts`; `src/types/viewerProtocol.fixture.generated.ts` | Permanently superseded old cycle/scene/viewer WebSocket and viewer-v1 wire contract. | I5-I9 create qualified thin clients. `src/state/latestFrameSlot.ts`, viewer performance, camera, snapshot, and structural rendering retain wire-independent behavior. |
| `src/store/catalogStore.ts` | Superseded v1 source/generation store; it does not return. | The active immutable-v2 `api/client.ts`, `api/parsers.ts`, `useStructuralResource`, and `CatalogPanel` replace the same-role client/view. |
| `src/store/commandAuthority.ts`; `src/store/commandAuthority.test.ts` | Permanently deleted browser command-admission semantic owner. | I7/L0 and I8/P5 server/Kernel owners prove authority. The focused historical negative cases remain at the pinned commit for new contract tests. |
| `src/store/cycleState.ts`; `src/store/cycleState.test.ts`; `src/store/cycleStore.ts` | The old cycle enum, WebSocket reduction, and unknown-to-`running` compatibility coercion do not return. | I6/N supplies resource-addressed execution projection and I8/P5 the final envelope. Recovery/status/history presentation shells are retained without inferring runtime truth. |
| `src/store/deploymentStore.ts` | Singleton deployment artifact/store and mutation API do not return. | I6/N supplies the component-instance deployment client; catalog capability filtering and authoring controls are retained independently. |
| `src/store/operatorAlarmStore.ts`; `src/store/operatorAlarmStore.test.ts` | Old polling endpoint/store shape is permanently superseded. Browser motion admission from alarm state is not restored. | I5/H0 now uses `useHeadlessState.ts`, the bounded owner-issued WebSocket projection, `HeadlessRuntimeSurfaces.tsx`, `alarmFences.ts`, `OperatorAlarmPanel.tsx`, and `ExecutionHistoryPanel.tsx`; the C++ owner retains lifecycle and complete membership truth. |
| `src/store/pointCloudStore.ts`; `src/store/pointCloudStore.test.ts` | The MFPC/v1 socket decoder and connection owner do not return by assumption. | I9/I owns the exact future point-cloud client. `src/state/latestFrameSlot.ts` retains epoch/sequence ordering, newest-only materialization, clear, invalidation, and stale-connection rejection without a wire schema. |
| `src/store/programStore.ts`; `src/store/programStore.test.ts` | The v1 Program serializer, singleton identities, Pick-Plan macro payload, scene/store joins, and mutation endpoints do not return. | I6/N implements the exact Program-Binding store. `authoringGraph.ts`, pure `composerStore.ts`, catalog/schema/parameter helpers, viewer-presentation reducer, and focused tests retain independent graph/draft behavior. |
| `src/store/robotControlCandidateStore.ts`; `src/store/robotControlCandidateStore.test.ts` | The old candidate DTO/endpoint, shared Approve owner, and singleton move groups do not return. N6-c1/c2 replace them with a generation-fenced local reducer/hook over no-effect owner preview and ordinary operation submission. | All 17 historical debounce, freshness, approval, rejection, terminal, and Revert cases are translated into the active reducer/component/native acceptance set. Pose/gizmo/move/joint/gripper/ghost/persistent-draft utilities retain authoring and fail-closed behavior. |
| `src/store/robotLifecycleStore.ts` | The former endpoint/poll/command store remains absent until I7/L0 freezes resource-addressed lifecycle requests and observed status. | `RobotLifecycleCard.tsx` retains observation-only presentation. I7 must reconnect enable/disable/recovery/reconcile confirmations without browser lifecycle authority. |
| `src/store/sceneStateStore.ts`; `src/store/sceneStateStore.viewer.test.ts` | Composer Scene Authority, old scene WebSocket reduction, live placement truth, and viewer-v1 joins do not return. | I5/H0 supplies headless scene truth and I6/N addressed scene/manual clients. Structural viewing, pose transforms, view drafts, and latest-frame mechanics remain. |
| `src/store/statusStore.ts` | The old runtime-status endpoint/poll store is permanently superseded by the I5/H0 owner-issued projection. | `useHeadlessState.ts` and `HeadlessRuntimeSurfaces.tsx` feed `RuntimeStatusOverlay.tsx` as a prop-driven observation view; none may infer robot readiness or gate commands. |
| `src/store/stopProfile.ts`; `src/store/stopProfile.test.ts` | Permanently superseded browser Stop fallback/profile table. | I8/P5 owns one Kernel Stop envelope; historical cases translate into Kernel/transport acceptance, not another client policy. |
| `src/store/viewerFrameStore.ts`; `src/store/viewerFrameStore.test.ts` | Viewer-v1 manifest/frame state and live wire owner do not return. | I5/I9 own exact projections; `latestFrameSlot.ts`, structural transforms, camera, performance, and snapshot clients retain protocol-independent mechanics. |
| `src/store/viewerSelectionStore.ts` | The mixed Zustand store remains absent. Its camera, collision, view-draft, move/gizmo, and selection concerns are split rather than discarded. | Camera parsing is in `viewerCamera.ts`; collision is local to `StructuralViewer`; presentation drafts are in `viewerPresentationDraft.ts`; addressed move/gizmo drafts reconnect at I6/N. |
| `src/utils/objectMoveDiscard.ts`; `src/utils/objectMoveDiscard.test.ts` | Direct `/api/scene/edit update_pose` discard is permanently superseded because it mutates a second scene owner. | I6/N must implement discard as revert of the exact addressed authoring draft/Apply candidate. Pose and draft helpers are retained; the historical test records the no-program-mutation user intent. |

### Retained extraction inventory

The current worktree therefore keeps these dependency-independent or
prop-driven responsibilities in their normal TypeScript package rather than in
a legacy graveyard:

| Responsibility | Current retained paths |
| --- | --- |
| Structural clients and exact parsing | `src/api/{client,parsers}.ts`, their tests, `src/types/api.ts`, `src/state/structuralResource.ts`, `src/components/{ArtifactBrowser,CatalogPanel,StructuralViewer,UnavailableBanner}.tsx` |
| Catalog/parameter authoring | `src/utils/{catalogSchema,catalogParameterValidation,profiles,deploymentCapabilities,parameterAuthoring}.ts` and focused tests |
| Graph authoring and presentation | `src/utils/authoringGraph.ts`, `src/store/composerStore.ts`, `src/components/{BtNode,StatusEdge,CanvasViewControls,NodePaletteItem,CollapsedRail}.tsx`, `src/components/common/DraftInputs.tsx`, `src/components/property/utils.ts`, and focused tests |
| Pose/manual-control drafts | `src/utils/{poseTransforms,moveTo,gizmoAuthoring,jointAuthoring,gripperAuthoring,gripperGhost,gripperSemantics,robotModelAuthority}.ts`, `src/hooks/usePersistentDraft.ts`, and focused tests |
| Viewer/capture mechanics | `src/utils/{structuralTransforms,viewerCamera,viewerPerformance,snapshotCapture}.ts`, `src/state/{latestFrameSlot,viewerPresentationDraft}.ts`, `src/components/{StructuralViewer,SnapshotResponder}.tsx`, and focused tests |
| Status, recovery, alarm, and history presentation | `src/components/{assistantRequestRecovery,RecoveryActionControls,KernelErrorPanel,RuntimeStatusOverlay,RobotLifecycleCard,OperatorAlarmPanel,ExecutionHistoryPanel}.*` and `src/utils/alarmFences.ts` |

The gate/ownership comments in these ports are part of the reconnection marker:
they identify the exact adapter still missing and state what the retained code
cannot own. Removing those comments requires either a replacement traceability
record or completion of the named gate.

## Reimplementation rule

Future work must separate interaction intent from semantic ownership:

- headless C++ owners hold runtime, scene, deployment, execution, persistence,
  identity, and lifecycle semantics;
- Python/FastAPI and TypeScript are thin authoring, inspection, and operator
  clients;
- assistant/MCP tooling is another client and cannot mint parallel runtime
  truth;
- same-role Python/TypeScript clients should be adapted rather than discarded
  merely because their current owner endpoint is unavailable;
- old tool names, payloads, and workflows are candidates for requirement
  audit, not automatic compatibility requirements; and
- no surface becomes available until its named restoration and qualification
  gate passes.

When a later slice recreates one workflow, record which historical files were
consulted, which intent was retained, which behavior was deliberately dropped,
and which new headless owner proves the resulting semantics.

## Mixed-file traceability convention

The pinned commit prevents source loss, but it is not the disposition record.
Every mixed Composer file handled by this relocation must leave an auditable
row containing:

- its historical path and affected symbols (or an exact mechanically
  enumerable path group), plus the last containing commit/tree;
- the dependency-independent code retained in Python or TypeScript;
- the old semantic authority or exclusive wire contract removed;
- the future owner, implementation language, and restoration gate; and
- the historical or focused test cases that carry the required interaction,
  failure, safety, and performance behavior.

Retained port targets remain in their normal package and language. Their
effectful entry points fail with a stable typed-unavailable reason before
imports, network/ROS activity, process launch, state mutation, evidence writes,
or hardware access, and name the restoration gate. A commented-out body,
unchecked source graveyard, copied `legacy` tree, or prose saying only “see Git
history” is not an acceptable marker.

This convention also applies retrospectively to scene/Core/RoboPlan and other
relocation work already present in the pinned cumulative range. Those rows are
derived from exact Git objects and the active demolition/evidence records, not
reconstructed from recollection. The completed retrospective check and current
Composer disposition are retained permanently in the companion
`composable-cell-relocation-responsibility-disposition-record-2026-07-31.md`
in the implementation-specification repository; their cumulative dynamic
closure is retained in the 2026-08-01 relocation-reconciliation qualification
evidence. The temporary relocation handoff is not a continuing authority or
retained fallback.
