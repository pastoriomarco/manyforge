import { beforeEach, expect, it, vi } from 'vitest';
import type { CarriedObjectAttachment, LogicalSceneObject, LogicalSceneProjection, ManagerActiveProjection,
  ProgramCycleStatus, SceneTransactionRequest } from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';

const apiMock = vi.hoisted(() => ({ manager: { status: vi.fn(), canonicalEmptySelection: vi.fn(),
  prepareCanonicalEmpty: vi.fn(), previewCandidate: vi.fn(), applyCandidate: vi.fn(), applyStatus: vi.fn() },
  execution: { identity: vi.fn() }, cclc: { targetRetirementOffer: vi.fn(), retireTargets: vi.fn(),
    ownerSettlement: vi.fn(), owner: vi.fn(), component: vi.fn(), submitLifecycle: vi.fn(), lifecycleStatus: vi.fn(),
    componentProcessStopOffer: vi.fn(), stopComponentProcess: vi.fn(), componentProcessStopResult: vi.fn() },
  scene: { commit: vi.fn() } }));
vi.mock('../api/client', () => ({ api: apiMock }));
import { closeScenario, prepareSceneForClose, resetCloseSnapshot, type ClosePreparationInputs } from './scenarioClose';
import { parseComponentProcessStopOffer } from '../api/processStopParsers';
import { errorMessage } from './structuralResource';

const scene = { scene_id: 'scene', revision_id: 1, content_hash: 'hash' };
const active: ManagerActiveProjection = { canonicalEmpty: false, retainedBundleId: 'bundle.current',
  graphRevision: 'graph.current', publication: { publication_generation: 2,
    durable_public_pointer_content_digest: 'pointer', projection_revision: {
      retained_bundle_id: 'bundle.current', graph_revision: 'graph.current', scene_revision: scene },
    transaction: { kind: 'named_apply', apply_id: 'apply.current', apply_generation: 1 } },
  cellGraph: { controllable_components: [{ controllable_component_id: 'arm' }] } };
const empty: ManagerActiveProjection = { ...active, canonicalEmpty: true, retainedBundleId: 'bundle.empty', graphRevision: 'graph.empty',
  publication: { publication_generation: 3, durable_public_pointer_content_digest: 'pointer.empty', projection_revision: {
    retained_bundle_id: 'bundle.empty', graph_revision: 'graph.empty', scene_revision: scene },
  transaction: { kind: 'named_apply', apply_id: 'apply.empty', apply_generation: 4 } } };
const managerOwnerEpoch = 'manager-7b22a4ec9837415a8f52d7c349fcaa91';
const cclcOwnerEpoch = `sha256:${'8'.repeat(64)}`;
const source = { available: true, freshness: 'fresh', sourceOwnerEpoch: managerOwnerEpoch, sourceOwnerId: 'manager.cell' };
const projection = (value: ManagerActiveProjection, metadata = source) => ({ source: metadata, status: { managerReady: true, active: value } });
const scope = { request_owner_epoch: cclcOwnerEpoch, publication_generation: 2, active_public_pointer_digest: 'pointer' };
const component = { controllableComponentId: 'arm', retainedBundleId: active.retainedBundleId, graphRevision: active.graphRevision,
  publicationGeneration: 2, lifecycleGeneration: 3, lifecycleState: 'active', readiness: { requestOwnerEpoch: cclcOwnerEpoch },
  lifecycleProgress: null, domains: [] };
const processScope = { predecessor_projection_revision: { retained_bundle_id: active.retainedBundleId,
  graph_revision: active.graphRevision, scene_revision: { scene_id: 'scene', revision_id: 1, content_hash: 'hash' } },
  expected_predecessor_publication_generation: 2, expected_predecessor_public_pointer_digest: 'pointer',
  runtime_unit_id: 'runtime', runtime_epoch: 'host', manager_runtime_epoch: 'manager' };
const prepared = { schema_version: 'manyforge.prepared_activation.v6', prepared_activation_id: 'actual-prepared',
  deployment_artifact_id: 'actual-empty-artifact', deployment_revision: 'actual-revision',
  retained_bundle_id: empty.retainedBundleId, graph_revision: empty.graphRevision };
const receipt = { prepared_activation_id: prepared.prepared_activation_id, impact_digest: 'actual-impact' };
const inputs = () => ({ expected: active, signal: new AbortController().signal, admissionGeneration: () => 8,
  waitForObservation: vi.fn(async () => {}), trackApply: vi.fn(), progress: vi.fn() });
beforeEach(() => {
  vi.resetAllMocks();
  apiMock.manager.status.mockResolvedValueOnce({ source, status: { managerReady: true, active } })
    .mockResolvedValue({ source, status: { managerReady: true, active: empty } });
  apiMock.cclc.targetRetirementOffer.mockResolvedValue({ scope, targets: [] });
  apiMock.cclc.retireTargets.mockResolvedValue({ replay_status: 'accepted_new', scope, retired_targets: [] });
  apiMock.cclc.ownerSettlement.mockResolvedValue({ scope, settled: true });
  apiMock.cclc.owner.mockResolvedValue({ requestOwnerEpoch: cclcOwnerEpoch });
  apiMock.execution.identity.mockResolvedValue({ retainedBundleId: active.retainedBundleId, graphRevision: active.graphRevision, activePublicationGeneration: 2 });
  apiMock.cclc.component.mockResolvedValue(component);
  apiMock.cclc.submitLifecycle.mockResolvedValue({ transactionId: 'disable.1', status: { terminalResult: 'succeeded', reconciliationRequired: false } });
  apiMock.cclc.componentProcessStopOffer.mockResolvedValue({ controllable_component_id: 'arm', process_inventories: [{ request: processScope,
    process_occurrences: [{ controllable_component_id: 'arm', launch_identity: 'actual-launch', leader_pid: 42,
      process_presence: 'present', cleanup_state: 'not_requested', receipt: null, installed_runtime_identity: null }], error: null }] });
  apiMock.cclc.stopComponentProcess.mockResolvedValue({ state: 'pending', receipt: null, error: null });
  apiMock.cclc.componentProcessStopResult.mockResolvedValue({ state: 'drained', receipt: { leader_pid: 42 }, error: null });
  apiMock.manager.canonicalEmptySelection.mockResolvedValue({ request_owner_epoch: managerOwnerEpoch, cell_id: 'actual-cell',
    deployment_artifact_id: prepared.deployment_artifact_id, deployment_revision: prepared.deployment_revision,
    candidate_retained_bundle_id: empty.retainedBundleId, candidate_graph_revision: empty.graphRevision, admission_digest: 'actual-admission' });
  apiMock.manager.prepareCanonicalEmpty.mockResolvedValue(prepared);
  apiMock.manager.previewCandidate.mockResolvedValue({ candidateReceipt: receipt });
  apiMock.manager.applyCandidate.mockResolvedValue({ applyId: 'apply.empty', applyGeneration: 4, state: 'pending' });
  apiMock.manager.applyStatus.mockResolvedValueOnce({ state: 'pending' }).mockResolvedValue({ state: 'succeeded' });
});
it('composes existing owner commands in order and waits for drain and terminal empty Apply', async () => {
  const request = inputs();
  expect(await closeScenario(request)).toEqual(empty);
  const ordered = [apiMock.cclc.retireTargets, apiMock.cclc.ownerSettlement, apiMock.cclc.submitLifecycle,
    apiMock.cclc.stopComponentProcess, apiMock.cclc.componentProcessStopResult, apiMock.manager.prepareCanonicalEmpty,
    apiMock.manager.previewCandidate, apiMock.manager.applyCandidate];
  expect(ordered.map((call) => call.mock.invocationCallOrder[0])).toEqual([...ordered.map((call) => call.mock.invocationCallOrder[0])].sort((a,b) => a-b));
  expect(apiMock.cclc.submitLifecycle.mock.calls[0][0].intent).toBe('disable');
  expect(apiMock.cclc.stopComponentProcess.mock.calls[0][0]).toMatchObject({ ...processScope, launch_identity: 'actual-launch', leader_pid: 42 });
  expect(apiMock.manager.applyCandidate.mock.calls[0][0]).toMatchObject({ scene_initialization: 'preserve', prepared_activation: prepared,
    candidate_receipt: receipt, accepted_impact_digest: receipt.impact_digest, expected_current: {
      retained_bundle_id: active.retainedBundleId, graph_revision: active.graphRevision } });
  expect(request.trackApply).toHaveBeenCalledWith({ applyId: 'apply.empty', applyGeneration: 4 });
  expect(request.waitForObservation).toHaveBeenCalledTimes(2);
});
it('unknown process Stop leaves replacement unrequested without resending Stop', async () => {
  apiMock.cclc.stopComponentProcess.mockResolvedValue({ state: 'unknown', error: { message: 'Old host drain unknown' }, receipt: null });
  await expect(closeScenario(inputs())).rejects.toThrow('Old host drain unknown');
  expect(apiMock.cclc.stopComponentProcess).toHaveBeenCalledTimes(1);
  expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
});
const stopUnknown = (code: string) => ({ state: 'unknown', receipt: null,
  error: { code, message: `Stop ${code}`, details: {} } });
it('response_unavailable Stop reads the exact result of the same request until drained', async () => {
  apiMock.cclc.stopComponentProcess.mockResolvedValue(stopUnknown('component_process_stop_response_unavailable'));
  const request = inputs();
  expect(await closeScenario(request)).toEqual(empty);
  expect(apiMock.cclc.stopComponentProcess).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.componentProcessStopResult).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.componentProcessStopResult.mock.calls[0][0]).toEqual(apiMock.cclc.stopComponentProcess.mock.calls[0][0]);
  expect(request.progress).toHaveBeenCalledWith('Waiting for the exact Stop result of arm…');
  expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
});
it('response_unavailable then stop_pending keeps reading the same Stop until drained', async () => {
  apiMock.cclc.stopComponentProcess.mockResolvedValue(stopUnknown('component_process_stop_response_unavailable'));
  apiMock.cclc.componentProcessStopResult.mockResolvedValueOnce(stopUnknown('component_process_stop_pending'))
    .mockResolvedValue({ state: 'drained', receipt: { leader_pid: 42 }, error: null });
  expect(await closeScenario(inputs())).toEqual(empty);
  expect(apiMock.cclc.stopComponentProcess).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.componentProcessStopResult).toHaveBeenCalledTimes(2);
  expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
});
it('a result_unavailable read is terminal after one read', async () => {
  apiMock.cclc.stopComponentProcess.mockResolvedValue(stopUnknown('component_process_stop_response_unavailable'));
  apiMock.cclc.componentProcessStopResult.mockResolvedValue(stopUnknown('component_process_stop_result_unavailable'));
  await expect(closeScenario(inputs())).rejects.toThrow('Stop component_process_stop_result_unavailable');
  expect(apiMock.cclc.stopComponentProcess).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.componentProcessStopResult).toHaveBeenCalledTimes(1);
  expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
});
it('an abort while reading the exact Stop result rejects without Apply', async () => {
  apiMock.cclc.stopComponentProcess.mockResolvedValue(stopUnknown('component_process_stop_response_unavailable'));
  apiMock.cclc.componentProcessStopResult.mockResolvedValue(stopUnknown('component_process_stop_pending'));
  const controller = new AbortController();
  const request = { ...inputs(), signal: controller.signal };
  request.waitForObservation.mockImplementation(async () => {
    if (apiMock.cclc.componentProcessStopResult.mock.calls.length === 2) controller.abort();
  });
  await expect(closeScenario(request)).rejects.toThrow();
  expect(apiMock.cclc.stopComponentProcess).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.componentProcessStopResult).toHaveBeenCalledTimes(2);
  expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
  expect(apiMock.manager.applyCandidate).not.toHaveBeenCalled();
});
it.each(['component_process_stop_owner_unavailable', 'component_process_stop_outcome_unknown'])(
  'a %s Stop stays terminal without result reads', async (code) => {
    apiMock.cclc.stopComponentProcess.mockResolvedValue(stopUnknown(code));
    await expect(closeScenario(inputs())).rejects.toThrow(`Stop ${code}`);
    expect(apiMock.cclc.stopComponentProcess).toHaveBeenCalledTimes(1);
    expect(apiMock.cclc.componentProcessStopResult).not.toHaveBeenCalled();
    expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
  },
);

function noneCloseFixture() {
  const exactScene = { scene_id: `sha256:${'3'.repeat(64)}`, revision_id: 1,
    content_hash: `sha256:${'4'.repeat(64)}` };
  const pointer = `sha256:${'5'.repeat(64)}`;
  const predecessor: ManagerActiveProjection = { ...active,
    cellGraph: { controllable_components: [{ controllable_component_id: 'gripper' }] },
    publication: { ...active.publication, durable_public_pointer_content_digest: pointer,
      projection_revision: { retained_bundle_id: active.retainedBundleId,
        graph_revision: active.graphRevision, scene_revision: exactScene } } };
  const successor: ManagerActiveProjection = { ...empty, publication: { ...empty.publication,
    projection_revision: { retained_bundle_id: empty.retainedBundleId,
      graph_revision: empty.graphRevision, scene_revision: exactScene } } };
  const ownerScope = { ...scope, active_public_pointer_digest: pointer };
  const offer = { controllable_component_id: 'gripper', process_inventories: [{
    request: { ...processScope, expected_predecessor_public_pointer_digest: pointer,
      predecessor_projection_revision: { retained_bundle_id: predecessor.retainedBundleId,
        graph_revision: predecessor.graphRevision, scene_revision: { ...exactScene } } },
    process_occurrences: [{ controllable_component_id: 'gripper', process_owner_kind: 'pneumatic_none_local',
      pneumatic_none_owner: { command_target_id: 'gripper', control_binding_id: 'binding.gripper',
        control_binding_revision: 'binding.revision.1' },
      installed_runtime_identity: `sha256:${'6'.repeat(64)}`, process_presence: 'absent' }], error: null,
  }] };
  apiMock.manager.status.mockReset().mockResolvedValueOnce(projection(predecessor))
    .mockResolvedValue(projection(successor));
  apiMock.cclc.targetRetirementOffer.mockResolvedValue({ scope: ownerScope, targets: [] });
  apiMock.cclc.retireTargets.mockResolvedValue({ replay_status: 'accepted_new', scope: ownerScope, retired_targets: [] });
  apiMock.cclc.ownerSettlement.mockResolvedValue({ scope: ownerScope, settled: true });
  apiMock.cclc.component.mockResolvedValue({ ...component, controllableComponentId: 'gripper', activePublicPointerDigest: pointer });
  apiMock.cclc.componentProcessStopOffer.mockImplementation(async () => parseComponentProcessStopOffer(offer, 'gripper'));
  return { request: { ...inputs(), expected: predecessor }, offer, successor };
}

it('retires and disables a verified None owner without a physical Stop or drain receipt', async () => {
  const fixture = noneCloseFixture();
  expect(await closeScenario(fixture.request)).toEqual(fixture.successor);
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.ownerSettlement).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.submitLifecycle.mock.calls[0][0]).toMatchObject({ controllable_component_id: 'gripper', intent: 'disable' });
  expect(apiMock.cclc.stopComponentProcess).not.toHaveBeenCalled();
  expect(apiMock.cclc.componentProcessStopResult).not.toHaveBeenCalled();
  expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
  expect(fixture.request.waitForObservation).toHaveBeenCalledTimes(1);
});

it('still stops and drains every physical occurrence beside a verified None owner', async () => {
  const fixture = noneCloseFixture();
  const inventory = fixture.offer.process_inventories[0];
  const mixed = { ...fixture.offer, process_inventories: [{ ...inventory,
    process_occurrences: [...inventory.process_occurrences, {
      controllable_component_id: 'gripper', launch_identity: 'actual-launch', leader_pid: 42,
      process_presence: 'absent', cleanup_state: 'not_requested', receipt: null, installed_runtime_identity: null,
    }] }] };
  apiMock.cclc.componentProcessStopOffer.mockImplementation(async () => parseComponentProcessStopOffer(mixed, 'gripper'));
  expect(await closeScenario(fixture.request)).toEqual(fixture.successor);
  expect(apiMock.cclc.stopComponentProcess).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.stopComponentProcess.mock.calls[0][0]).toMatchObject({
    controllable_component_id: 'gripper', launch_identity: 'actual-launch', leader_pid: 42,
  });
  expect(apiMock.cclc.componentProcessStopResult).toHaveBeenCalledTimes(1);
});

it.each(['bundle', 'graph', 'publication', 'pointer', 'scene'])(
  'refuses a foreign None %s scope before requesting empty Apply', async (field) => {
    const fixture = noneCloseFixture();
    const scope = fixture.offer.process_inventories[0].request;
    if (field === 'bundle') scope.predecessor_projection_revision.retained_bundle_id = 'other.bundle';
    if (field === 'graph') scope.predecessor_projection_revision.graph_revision = 'other.graph';
    if (field === 'publication') scope.expected_predecessor_publication_generation += 1;
    if (field === 'pointer') scope.expected_predecessor_public_pointer_digest = `sha256:${'7'.repeat(64)}`;
    if (field === 'scene') scope.predecessor_projection_revision.scene_revision.revision_id += 1;
    await expect(closeScenario(fixture.request)).rejects.toThrow('Applied configuration changed during Close.');
    expect(apiMock.cclc.stopComponentProcess).not.toHaveBeenCalled();
    expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
  },
);

it('does not infer no-output from a malformed or unverified owner occurrence', async () => {
  const fixture = noneCloseFixture();
  fixture.offer.process_inventories[0].process_occurrences[0].installed_runtime_identity = 'unknown';
  await expect(closeScenario(fixture.request)).rejects.toThrow('exact owner digest');
  expect(apiMock.cclc.stopComponentProcess).not.toHaveBeenCalled();
  expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
});
it.each(['failed', 'unknown_effect'])('a %s empty Apply cannot complete Close', async (state) => {
  apiMock.manager.applyStatus.mockReset().mockResolvedValue({ state, failure: { message: 'Apply incomplete' } });
  await expect(closeScenario(inputs())).rejects.toThrow('Apply incomplete');
  expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
  expect(apiMock.manager.status).toHaveBeenCalledTimes(1);
});
it('stale initial owner state has no effects', async () => {
  apiMock.manager.status.mockReset().mockResolvedValue({ source: { ...source, freshness: 'stale' }, status: { managerReady: true, active } });
  await expect(closeScenario(inputs())).rejects.toThrow('Close was not started');
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
});
it('terminal success with a different current publication does not complete Close', async () => {
  apiMock.manager.status.mockReset().mockResolvedValueOnce(projection(active))
    .mockResolvedValue(projection({ ...active, publication: { ...active.publication, publication_generation: 4 } }));
  await expect(closeScenario(inputs())).rejects.toThrow('current publication cannot be verified');
});
it('Declared components do not receive an invented lifecycle transition', async () => {
  apiMock.cclc.component.mockResolvedValue({ ...component, lifecycleState: 'declared' });
  await closeScenario(inputs());
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
});

it('observes several exact predecessor projections before the one accepted empty Apply', async () => {
  const request = inputs();
  apiMock.manager.status.mockReset().mockResolvedValueOnce(projection(active))
    .mockResolvedValueOnce(projection(active)).mockResolvedValueOnce(projection(active))
    .mockResolvedValueOnce(projection(active)).mockResolvedValueOnce(projection(empty));
  expect(await closeScenario(request)).toEqual(empty);
  expect(request.waitForObservation).toHaveBeenCalledTimes(5);
  expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.submitLifecycle).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.stopComponentProcess).toHaveBeenCalledTimes(1);
  expect(apiMock.manager.applyStatus).toHaveBeenCalledTimes(2);
});

it('compares the exact predecessor publication independently of object key order', async () => {
  const request = inputs();
  const reordered = { ...active, publication: Object.fromEntries(Object.entries(active.publication).reverse()) };
  apiMock.manager.status.mockReset().mockResolvedValueOnce(projection(active))
    .mockResolvedValueOnce(projection(reordered)).mockResolvedValueOnce(projection(empty));
  expect(await closeScenario(request)).toEqual(empty);
  expect(request.waitForObservation).toHaveBeenCalledTimes(3);
});

it.each([
  ['stale predecessor', projection(active, { ...source, freshness: 'stale' })],
  ['unavailable predecessor', projection(active, { ...source, available: false, freshness: 'unavailable' })],
  ['foreign predecessor owner', projection(active, { ...source, sourceOwnerEpoch: 'other.owner' })],
  ['foreign candidate owner', projection(empty, { ...source, sourceOwnerEpoch: 'other.owner' })],
  ['foreign source identity', projection(active, { ...source, sourceOwnerId: 'other.manager' })],
  ['changed predecessor pointer', projection({ ...active, publication: { ...active.publication, durable_public_pointer_content_digest: 'other.pointer' } })],
  ['changed predecessor scene', projection({ ...active, publication: { ...active.publication, projection_revision: {
    retained_bundle_id: active.retainedBundleId, graph_revision: active.graphRevision, scene_revision: { ...scene, revision_id: 2 } } } })],
  ['foreign Apply transaction', projection({ ...empty, publication: { ...empty.publication,
    transaction: { kind: 'named_apply', apply_id: 'another.apply', apply_generation: 4 } } })],
  ['foreign Apply generation', projection({ ...empty, publication: { ...empty.publication,
    transaction: { kind: 'named_apply', apply_id: 'apply.empty', apply_generation: 5 } } })],
  ['malformed publication', projection({ ...active, publication: {} })],
  ['contradictory candidate tuple', projection({ ...empty, publication: { ...empty.publication,
    projection_revision: { retained_bundle_id: active.retainedBundleId, graph_revision: active.graphRevision, scene_revision: scene } } })],
])('refuses %s after terminal Apply without waiting or repeating effects', async (_name, final) => {
  const request = inputs();
  apiMock.manager.status.mockReset().mockResolvedValueOnce(projection(active)).mockResolvedValue(final);
  await expect(closeScenario(request)).rejects.toThrow('current publication cannot be verified');
  expect(request.waitForObservation).toHaveBeenCalledTimes(2);
  expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
  expect(request.progress).not.toHaveBeenCalledWith('Scenario closed.');
});

it('propagates cancellation during final predecessor observation without another effect or read', async () => {
  const controller = new AbortController();
  const request = { ...inputs(), signal: controller.signal };
  apiMock.manager.status.mockReset().mockResolvedValueOnce(projection(active)).mockResolvedValue(projection(active));
  request.waitForObservation.mockImplementationOnce(async () => {})
    .mockImplementationOnce(async () => {}).mockImplementationOnce(async () => { controller.abort(new Error('Close cancelled')); });
  await expect(closeScenario(request)).rejects.toThrow('Close cancelled');
  expect(apiMock.manager.status).toHaveBeenCalledTimes(2);
  expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.stopComponentProcess).toHaveBeenCalledTimes(1);
});

// Actual C06 013/014 observations and the later passive Headless observation.
// Keep every publication/source value consulted by Close; component membership
// was genuinely empty in both observed graphs. No catch-up timing is inferred.
it('C06 succeeded second Close waits on fresh generation 4 New before exact generation 5 Close', async () => {
  const predecessor: ManagerActiveProjection = {
  "canonicalEmpty": false,
  "retainedBundleId": "sha256:64cd485f547b5b4fb6fa18d5b4b4c47d03e50f0ca227c6cfa2b802d7e724e1f8",
  "graphRevision": "sha256:2ae82c864ff20fcf7ca50085e2a52fccb9b3b70c1f8c60acccfb5f2a668c0d39",
  "publication": {
    "durable_public_pointer_content_digest": "sha256:845f015a2c6f0f0c19186c07396eca954c142bd0840996d165ecd4daa2d246fb",
    "projection_revision": {
      "graph_revision": "sha256:2ae82c864ff20fcf7ca50085e2a52fccb9b3b70c1f8c60acccfb5f2a668c0d39",
      "retained_bundle_id": "sha256:64cd485f547b5b4fb6fa18d5b4b4c47d03e50f0ca227c6cfa2b802d7e724e1f8",
      "scene_revision": {
        "content_hash": "sha256:a78d97e97a8a21e144ab8b78de1e5be6a985908c928a11ad8b925464d91ea23f",
        "revision_id": 3,
        "scene_id": "sha256:bdaea683e59e72d0b24035019b5d2f377e4af7bd5c6ed0ef54e40948b08ef801"
      }
    },
    "publication_generation": 4,
    "transaction": {
      "apply_generation": 1,
      "apply_id": "apply-7c69fe21b74bfff315fe93f60b6c699e",
      "kind": "named_apply"
    }
  },
  "cellGraph": {
    "controllable_components": []
  }
};
  const candidate: ManagerActiveProjection = {
  "canonicalEmpty": true,
  "retainedBundleId": "sha256:e42be21788712cf9227f8304f8e1d405370b57ac05f2534c4c32719d90ce7266",
  "graphRevision": "sha256:f528d4acf2fed2bea4794c767141c2083356bd0730a20a98060a9959534d466d",
  "publication": {
    "durable_public_pointer_content_digest": "sha256:6abf2e3b88da61e620587a89acf7802f66b42173bb9e5b840f720b55df79b3a3",
    "projection_revision": {
      "graph_revision": "sha256:f528d4acf2fed2bea4794c767141c2083356bd0730a20a98060a9959534d466d",
      "retained_bundle_id": "sha256:e42be21788712cf9227f8304f8e1d405370b57ac05f2534c4c32719d90ce7266",
      "scene_revision": {
        "content_hash": "sha256:a78d97e97a8a21e144ab8b78de1e5be6a985908c928a11ad8b925464d91ea23f",
        "revision_id": 3,
        "scene_id": "sha256:bdaea683e59e72d0b24035019b5d2f377e4af7bd5c6ed0ef54e40948b08ef801"
      }
    },
    "publication_generation": 5,
    "transaction": {
      "apply_generation": 1,
      "apply_id": "apply-7e297034567de3cd73fb4bc8e3ca7fa8",
      "kind": "named_apply"
    }
  },
  "cellGraph": {
    "controllable_components": []
  }
};
  const predecessorSource = {
  "available": true,
  "freshness": "fresh",
  "sourceOwnerEpoch": "manager-ed9253566515333b26f1a2850b65a4a2",
  "sourceOwnerId": "component-manager.ui6i_ur5e_lite6_wall_cell",
  "sourceGeneration": 3881,
  "receiverAgeNs": 342763581
};
  const candidateSource = {
  "available": true,
  "freshness": "fresh",
  "sourceOwnerEpoch": "manager-ed9253566515333b26f1a2850b65a4a2",
  "sourceOwnerId": "component-manager.ui6i_ur5e_lite6_wall_cell",
  "sourceGeneration": 13614,
  "receiverAgeNs": 10804422
};
  const accepted = {"applyId": "apply-7e297034567de3cd73fb4bc8e3ca7fa8", "applyGeneration": 1};
  const request = { ...inputs(), expected: predecessor };
  const actualPrepared = { ...prepared, deployment_artifact_id: 'ur5e_lite6_wall.canonical_empty',
    deployment_revision: 'sha256:a054aaab64456ed3e895b4b60ec3f3391553040e1204e9cf6c2be737ca70a2de',
    retained_bundle_id: candidate.retainedBundleId, graph_revision: candidate.graphRevision };
  apiMock.manager.status.mockReset().mockResolvedValueOnce(projection(predecessor, predecessorSource))
    .mockResolvedValueOnce(projection(predecessor, predecessorSource))
    .mockResolvedValueOnce(projection(candidate, candidateSource));
  apiMock.manager.canonicalEmptySelection.mockResolvedValue({ request_owner_epoch: predecessorSource.sourceOwnerEpoch,
    cell_id: 'ui6i_ur5e_lite6_wall_cell', deployment_artifact_id: actualPrepared.deployment_artifact_id,
    deployment_revision: actualPrepared.deployment_revision, candidate_retained_bundle_id: candidate.retainedBundleId,
    candidate_graph_revision: candidate.graphRevision, admission_digest: 'sha256:a21efe1461d8c6fa3ad31b0827d1796ee915e9e06b2fb68eda714349fa461204' });
  apiMock.manager.prepareCanonicalEmpty.mockResolvedValue(actualPrepared);
  apiMock.manager.applyCandidate.mockResolvedValue({ ...accepted, state: 'pending' });
  apiMock.manager.applyStatus.mockReset().mockResolvedValue({ ...accepted, state: 'succeeded' });
  expect(await closeScenario(request)).toEqual(candidate);
  expect(request.waitForObservation).toHaveBeenCalledTimes(1);
  expect(apiMock.manager.status).toHaveBeenCalledTimes(3);
  expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
  expect(apiMock.manager.applyStatus).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  expect(apiMock.cclc.stopComponentProcess).not.toHaveBeenCalled();
  expect(request.trackApply).toHaveBeenCalledWith(accepted);
});

function standaloneFixture(state: ProgramCycleStatus['state'] = 'idle_after_reset', carried: string[] = []) {
  const workspace = { workspaceEpoch: 'workspace.1', workspaceRevision: 1,
    programDraftId: 'draft.1', programDraftRevision: 3, deploymentDraftId: 'deployment.1', deploymentDraftRevision: 4 };
  const initialCycle: ProgramCycleStatus = { programRunnerOwnerEpoch: 'runner.1', cycleGeneration: 7, state,
    programRunId: state === 'idle_after_reset' ? null : 'run.1', cycleClaimGeneration: state === 'idle_after_reset' ? null : 5,
    terminalOrSettlementReference: state === 'terminal_awaiting_reset' ? 'terminal.1' : null,
    runRevision: null, nodeStatusRevision: null, runtimeControlRevision: null, executionControlRevision: 1,
    causalCorrelationId: null, causalSessionId: null, causalClientSequence: null, currentCommandKind: null,
    failure: null, resetPhase: null, resetOptions: null,
    resetRobotPosition: { available: false, reason: 'unsupported' }, allowedCommands: [], run: null, runtimeControls: null };
  function sceneView(revision: number, attached = carried): LogicalSceneProjection {
    const attachmentRevision = 'sha256:' + 'a'.repeat(64);
    const pose = { position_m: { x: 4, y: 5, z: 6 }, orientation_xyzw: [0, 0, 0, 1] as [number, number, number, number] };
    const currentScene = { scene_id: 'scene', revision_id: revision, content_hash: revision === 1 ? 'hash' : `hash.${revision}` };
    return { api_version: 'manyforge.logical_scene_source.v1', access: {
      active_projection: { retained_bundle_id: active.retainedBundleId, graph_revision: active.graphRevision, scene_revision: currentScene },
      phase: 'active', scene_writes_open: true, projection_leases_open: true }, snapshot: {
      cell_id: 'cell', revision: currentScene, configured_collision_policy: {}, policy_epoch: 1, topology_epoch: 1,
      transaction_sequence: revision, objects: carried.map<LogicalSceneObject>((id) => ({ scene_resource_id: id,
        collision_shapes: [{ type: 'box', dimensions_m: { x: 1, y: 1, z: 1 } }], provider_evidence: null, support_contacts: [],
        placement: attached.includes(id) ? { kind: 'carried', carried_object_attachment_id: `attachment.${id}`,
          carried_object_attachment_revision: attachmentRevision } : { kind: 'cell_fixed', cell_id: 'cell', pose_in_cell: pose } })),
      carried_object_attachments: attached.map<CarriedObjectAttachment>((id) => ({ carried_object_attachment_id: `attachment.${id}`,
        carried_object_attachment_revision: attachmentRevision, scene_resource_id: id,
        controllable_component_id: 'arm', command_target_id: 'tool', carrier_frame_ref: {
          kind: 'component', controllable_component_id: 'arm', model_instance_id: 'robot', frame_role_id: 'tool' },
        pose_in_carrier_frame: pose, touch_body_roles: [] })) } };
  }
  const section = { available: true, freshness: 'fresh' as const, knownGaps: [], sourceOwnerId: 'scene.owner',
    sourceOwnerEpoch: 'scene.epoch', sourceGeneration: 1, receiverAgeNs: 0, payload: { ...sceneView(1) } };
  let current = { receiverEpoch: 'receiver.1', authoringWorkspace: workspace, authoringWorkspaceContainment: null,
    authoringAdmission: { authoringAllowed: true, admissionGeneration: 8 }, programCycle: initialCycle,
    componentOverview: { readiness: 'ready', userMessage: 'Current', components: [], maintenanceOperation: null, basis: {} },
    runtime: { sections: { logicalScene: section, manager: { available: true, freshness: 'fresh', knownGaps: [],
      sourceOwnerId: source.sourceOwnerId, sourceOwnerEpoch: source.sourceOwnerEpoch,
      payload: { active: { canonical_empty: active.canonicalEmpty, retained_bundle_id: active.retainedBundleId,
        graph_revision: active.graphRevision, publication: active.publication } } } } } } as unknown as ComposerHeadlessState;
  const reset = vi.fn<ClosePreparationInputs['reset']>(async (options) => ({ status: 'sent', reset: {
    requestId: 'reset.1', correlationId: options.correlationId, ...options.expected } }));
  const preparation: ClosePreparationInputs = { confirmed: { authoring: workspace,
    receiverEpoch: current.receiverEpoch, managerOwnerId: source.sourceOwnerId, managerOwnerEpoch: source.sourceOwnerEpoch,
    managerActive: current.runtime.sections.manager.payload!.active, closeCycle: initialCycle, closeScene: section },
    readSnapshot: () => current, reset };
  const abort = new AbortController();
  const request = { ...inputs(), signal: abort.signal, preparation };
  apiMock.manager.applyStatus.mockReset().mockResolvedValue({ state: 'succeeded' });
  apiMock.scene.commit.mockImplementation(async (transaction: SceneTransactionRequest) => ({
    request_id: transaction.request_context.request_id, request_owner_epoch: transaction.request_context.request_owner_epoch,
    committed_projection_revision: { ...transaction.expected_projection_revision,
      scene_revision: { ...transaction.expected_projection_revision.scene_revision,
        revision_id: transaction.expected_projection_revision.scene_revision.revision_id + 1,
        content_hash: `hash.${transaction.expected_projection_revision.scene_revision.revision_id + 1}` } } }));
  return { request, reset, abort, workspace, sceneView,
    get current() { return current; },
    change: (next: ComposerHeadlessState) => { current = next; },
    cycle: (patch: Partial<ProgramCycleStatus>) => { current = { ...current, programCycle: { ...current.programCycle, ...patch } }; },
    publishScene: (revision: number, attached: string[]) => { current = { ...current,
      runtime: { ...current.runtime, sections: { ...current.runtime.sections,
        logicalScene: { ...current.runtime.sections.logicalScene, sourceGeneration: revision, payload: { ...sceneView(revision, attached) } } } } }; },
  };
}

it('standalone terminal Close sends one ordinary Reset and waits for its correlated newer idle before retiring', async () => {
  const test = standaloneFixture('terminal_awaiting_reset');
  test.request.waitForObservation.mockImplementationOnce(async () => {
    expect(test.reset).toHaveBeenCalledTimes(1);
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    expect(apiMock.scene.commit).not.toHaveBeenCalled();
    test.cycle({ state: 'resetting' });
  }).mockImplementationOnce(async () => {
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    test.cycle({ state: 'idle_after_reset', cycleGeneration: 8, programRunId: null, cycleClaimGeneration: null,
      causalCorrelationId: test.reset.mock.calls[0][0].correlationId });
  });
  expect(await closeScenario(test.request)).toEqual(empty);
  expect(test.reset).toHaveBeenCalledExactlyOnceWith({ correlationId: expect.any(String), expected: {
    programRunId: 'run.1', programRunnerOwnerEpoch: 'runner.1', cycleGeneration: 7 } });
  // Count only Reset settlement observations: the ordinary Close can also
  // observe later process-drain/Apply completion through this shared callback.
  const retirement = apiMock.cclc.retireTargets.mock.invocationCallOrder[0];
  expect(test.request.waitForObservation.mock.invocationCallOrder.filter((call) => call < retirement)).toHaveLength(2);
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
});

it.each(['running', 'stopping', 'starting', 'resetting', 'recovery_required'] as const)(
  'standalone %s cycle cannot Reset, detach, or begin Close', async (state) => {
    const test = standaloneFixture(state, ['held.1']);
    await expect(closeScenario(test.request)).rejects.toThrow('Stop motion before closing the Scenario');
    expect(test.reset).not.toHaveBeenCalled();
    expect(apiMock.scene.commit).not.toHaveBeenCalled();
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
  },
);

it('already-idle standalone Close with no carried objects sends no Reset or Scene mutation', async () => {
  const test = standaloneFixture();
  expect(await closeScenario(test.request)).toEqual(empty);
  expect(test.reset).not.toHaveBeenCalled();
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
});

it('standalone Close joins distinct Manager and CCLC namespaces through the captured publication', async () => {
  const test = standaloneFixture();
  expect(managerOwnerEpoch).not.toBe(cclcOwnerEpoch);
  expect(await closeScenario(test.request)).toEqual(empty);
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledExactlyOnceWith(expect.objectContaining({
    request_context: expect.objectContaining({ request_owner_epoch: cclcOwnerEpoch }), scope,
  }), test.request.signal);
  expect(apiMock.cclc.submitLifecycle.mock.calls[0][0].request_context.request_owner_epoch).toBe(cclcOwnerEpoch);
  expect(apiMock.manager.applyCandidate.mock.calls[0][0].request_owner_epoch).toBe(managerOwnerEpoch);
});

it.each(['owner', 'publication', 'pointer'])(
  'standalone Close refuses a changed retirement %s before sending an effect', async (field) => {
    const test = standaloneFixture();
    apiMock.cclc.targetRetirementOffer.mockResolvedValue({ scope: { ...scope,
      ...(field === 'owner' ? { request_owner_epoch: `sha256:${'9'.repeat(64)}` } : {}),
      ...(field === 'publication' ? { publication_generation: scope.publication_generation + 1 } : {}),
      ...(field === 'pointer' ? { active_public_pointer_digest: `sha256:${'a'.repeat(64)}` } : {}),
    }, targets: [] });
    await expect(closeScenario(test.request)).rejects.toThrow('The lifecycle retirement owner or publication changed.');
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
    expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
  },
);

it('standalone Close refuses a coherent replacement CCLC owner after retirement without resending', async () => {
  const test = standaloneFixture();
  const changed = `sha256:${'9'.repeat(64)}`;
  apiMock.cclc.owner.mockResolvedValueOnce({ requestOwnerEpoch: cclcOwnerEpoch })
    .mockResolvedValue({ requestOwnerEpoch: changed });
  apiMock.cclc.component.mockResolvedValue({ ...component, readiness: { requestOwnerEpoch: changed } });
  await expect(closeScenario(test.request)).rejects.toThrow('The lifecycle owner changed during Close.');
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
});

it('idle manual detaches are sequential exact Scene successors without an authored admission bump or invented pose', async () => {
  const test = standaloneFixture('idle_after_reset', ['held.1', 'held.2']);
  test.request.waitForObservation.mockImplementationOnce(async () => {
    expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    test.publishScene(2, ['held.2']);
  }).mockImplementationOnce(async () => {
    expect(apiMock.scene.commit).toHaveBeenCalledTimes(2);
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    test.publishScene(3, []);
  });
  expect(await closeScenario(test.request)).toEqual(empty);
  expect(test.reset).not.toHaveBeenCalled();
  expect(test.current.authoringAdmission.admissionGeneration).toBe(8);
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(2);
  for (const [index, [transaction]] of apiMock.scene.commit.mock.calls.entries()) {
    expect(transaction).toMatchObject({ expected_authoring_workspace: test.workspace,
      request_context: { request_owner_epoch: 'scene.epoch', source_label: 'composer_ui' },
      expected_projection_revision: { retained_bundle_id: active.retainedBundleId, graph_revision: active.graphRevision,
        scene_revision: { scene_id: 'scene', revision_id: index + 1, content_hash: index === 0 ? 'hash' : 'hash.2' } },
      operation: { kind: 'detach_carried_object_at_current_pose', carried_object_attachment_id: `attachment.held.${index + 1}`,
        expected_scene_resource_id: `held.${index + 1}`, expected_controllable_component_id: 'arm', expected_command_target_id: 'tool' } });
    expect(transaction).not.toHaveProperty('expected_admission_generation');
    expect(transaction).not.toHaveProperty('expected_draft_id');
    expect(transaction).not.toHaveProperty('expected_draft_revision');
    expect(transaction.operation).not.toHaveProperty('pose_in_cell');
  }
  expect(apiMock.scene.commit.mock.invocationCallOrder[1]).toBeLessThan(apiMock.cclc.retireTargets.mock.invocationCallOrder[0]);
});

it.each(['refused', 'unconfirmed'] as const)('Reset %s stops without any Close continuation or resend', async (status) => {
  const test = standaloneFixture('terminal_awaiting_reset');
  test.reset.mockImplementationOnce(async (options) => status === 'refused'
    ? { status, message: 'Reset refused by current owner' }
    : { status, message: 'Reset delivery unknown', reset: { requestId: 'reset.1', correlationId: options.correlationId, ...options.expected } });
  await expect(closeScenario(test.request)).rejects.toThrow(status === 'refused' ? 'Reset refused' : 'Reset delivery unknown');
  expect(test.reset).toHaveBeenCalledTimes(1);
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
});

it.each(['foreign_owner', 'other_idle', 'recovery', 'workspace'])(
  'Reset settlement %s cannot continue Close or resend Reset', async (change) => {
    const test = standaloneFixture('terminal_awaiting_reset');
    test.request.waitForObservation.mockImplementationOnce(async () => {
      const correlation = test.reset.mock.calls[0][0].correlationId;
      test.cycle({ state: 'idle_after_reset', cycleGeneration: 8, programRunId: null, causalCorrelationId: correlation,
        ...(change === 'foreign_owner' ? { programRunnerOwnerEpoch: 'runner.2' } : {}),
        ...(change === 'other_idle' ? { causalCorrelationId: 'another.request' } : {}),
        ...(change === 'recovery' ? { state: 'recovery_required' } : {}) });
      if (change === 'workspace') test.change({ ...test.current, authoringWorkspace: { ...test.workspace, workspaceRevision: 2 } });
    });
    await expect(closeScenario(test.request)).rejects.toThrow(change === 'workspace' ? 'Reset done; Close not completed' : 'Close not completed');
    expect(test.reset).toHaveBeenCalledTimes(1);
    expect(apiMock.scene.commit).not.toHaveBeenCalled();
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  },
);

it('pins correlated Reset idle and refuses a foreign idle observed during the retirement offer', async () => {
  const test = standaloneFixture('terminal_awaiting_reset');
  test.request.waitForObservation.mockImplementationOnce(async () => {
    test.cycle({ state: 'idle_after_reset', cycleGeneration: 8, programRunId: null, cycleClaimGeneration: null,
      causalCorrelationId: test.reset.mock.calls[0][0].correlationId });
    test.change({ ...test.current, authoringAdmission: { ...test.current.authoringAdmission, authoringAllowed: false } });
  });
  apiMock.cclc.targetRetirementOffer.mockImplementationOnce(async () => {
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    expect(apiMock.scene.commit).not.toHaveBeenCalled();
    test.cycle({ cycleGeneration: 9, causalCorrelationId: 'another.reset' });
    test.change({ ...test.current, authoringAdmission: { ...test.current.authoringAdmission, authoringAllowed: true } });
    return { scope, targets: [] };
  });
  const completion = closeScenario(test.request);
  await expect(completion).rejects.toThrow('Reset done; Close not completed');
  expect(test.reset).toHaveBeenCalledTimes(1);
  expect(test.request.waitForObservation).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.targetRetirementOffer).toHaveBeenCalledTimes(1);
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
});

it('idle no-carry Close retains ordinary incident recovery with generic authoring and Scene writes closed', async () => {
  const test = standaloneFixture();
  const closedScene = test.sceneView(1);
  closedScene.access.scene_writes_open = false;
  closedScene.access.projection_leases_open = false;
  test.change({ ...test.current, authoringAdmission: { ...test.current.authoringAdmission, authoringAllowed: false },
    runtime: { ...test.current.runtime, sections: { ...test.current.runtime.sections,
      logicalScene: { ...test.current.runtime.sections.logicalScene, payload: { ...closedScene } } } } });
  test.request.preparation.confirmed.closeScene = test.current.runtime.sections.logicalScene;
  apiMock.cclc.component.mockResolvedValue({ ...component,
    lifecycleProgress: { incident: { incidentId: 'incident.1' } } });
  apiMock.cclc.submitLifecycle.mockImplementation(async (request) => request.intent === 'recover'
    ? { incidentRecovery: { closureReason: 'cell_closed_by_operator' } }
    : { transactionId: 'disable.1', status: { terminalResult: 'succeeded', reconciliationRequired: false } });
  expect(await closeScenario(test.request)).toEqual(empty);
  expect(test.reset).not.toHaveBeenCalled();
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.submitLifecycle.mock.calls[0][0]).toMatchObject({ intent: 'recover',
    close_incident_recovery: { expected_incident_id: 'incident.1' }, reason: 'cell closed by operator' });
});

it('terminal Reset continues from exact correlated idle without generic authoring readiness', async () => {
  const test = standaloneFixture('terminal_awaiting_reset');
  test.request.waitForObservation.mockImplementationOnce(async () => {
    test.cycle({ state: 'idle_after_reset', cycleGeneration: 8, programRunId: null, cycleClaimGeneration: null,
      causalCorrelationId: test.reset.mock.calls[0][0].correlationId });
    test.change({ ...test.current, authoringAdmission: { ...test.current.authoringAdmission, authoringAllowed: false } });
  });
  expect(await closeScenario(test.request)).toEqual(empty);
  expect(test.reset).toHaveBeenCalledTimes(1);
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
  expect(test.current.authoringAdmission.authoringAllowed).toBe(false);
});

it('a new carried object during the retirement offer blocks all lifecycle effects under otherwise unchanged identities', async () => {
  const test = standaloneFixture();
  const attachmentRevision = 'sha256:' + 'a'.repeat(64);
  const changedScene = test.sceneView(2);
  changedScene.snapshot!.objects = [{ scene_resource_id: 'held.new', collision_shapes: [], provider_evidence: null,
    support_contacts: [], placement: { kind: 'carried', carried_object_attachment_id: 'attachment.new',
      carried_object_attachment_revision: attachmentRevision } }];
  changedScene.snapshot!.carried_object_attachments = [{ carried_object_attachment_id: 'attachment.new',
    carried_object_attachment_revision: attachmentRevision, scene_resource_id: 'held.new',
    controllable_component_id: 'arm', command_target_id: 'tool', carrier_frame_ref: {
      kind: 'component', controllable_component_id: 'arm', model_instance_id: 'robot', frame_role_id: 'tool' },
    pose_in_carrier_frame: { position_m: { x: 4, y: 5, z: 6 }, orientation_xyzw: [0, 0, 0, 1] }, touch_body_roles: [] }];
  apiMock.cclc.targetRetirementOffer.mockImplementationOnce(async () => {
    test.change({ ...test.current, runtime: { ...test.current.runtime, sections: { ...test.current.runtime.sections,
      logicalScene: { ...test.current.runtime.sections.logicalScene, sourceGeneration: 2, payload: { ...changedScene } } } } });
    return { scope, targets: [] };
  });
  await expect(closeScenario(test.request)).rejects.toThrow('The logical Scene changed before Close completed');
  expect(test.reset).not.toHaveBeenCalled();
  expect(apiMock.cclc.targetRetirementOffer).toHaveBeenCalledTimes(1);
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(apiMock.cclc.ownerSettlement).not.toHaveBeenCalled();
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
});

it('a Deployment edit during the retirement offer stops Close after Reset without retiring anything', async () => {
  const test = standaloneFixture('terminal_awaiting_reset');
  test.request.waitForObservation.mockImplementationOnce(async () => {
    test.cycle({ state: 'idle_after_reset', cycleGeneration: 8, programRunId: null, cycleClaimGeneration: null,
      causalCorrelationId: test.reset.mock.calls[0][0].correlationId });
  });
  apiMock.cclc.targetRetirementOffer.mockImplementationOnce(async () => {
    test.change({ ...test.current, authoringWorkspace: { ...test.workspace,
      workspaceRevision: 2, deploymentDraftRevision: test.workspace.deploymentDraftRevision + 1 } });
    return { scope, targets: [] };
  });
  await expect(closeScenario(test.request)).rejects.toThrow('Reset done; Close not completed');
  expect(test.reset).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.targetRetirementOffer).toHaveBeenCalledTimes(1);
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(apiMock.cclc.ownerSettlement).not.toHaveBeenCalled();
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
});

it.each(['failure', 'unknown_receipt'])(
  'second detach %s preserves the first confirmed object and never closes or retries', async (condition) => {
    const test = standaloneFixture('idle_after_reset', ['held.1', 'held.2']);
    test.request.waitForObservation.mockImplementationOnce(async () => { test.publishScene(2, ['held.2']); });
    const normal = apiMock.scene.commit.getMockImplementation()!;
    apiMock.scene.commit.mockImplementationOnce(normal).mockImplementationOnce(async (transaction: SceneTransactionRequest) => {
      if (condition === 'failure') throw new Error('carrier pose unavailable');
      return { request_id: 'another.request', request_owner_epoch: transaction.request_context.request_owner_epoch,
        committed_projection_revision: transaction.expected_projection_revision };
    });
    await expect(closeScenario(test.request)).rejects.toThrow('Detached objects confirmed: held.1. Detach of held.2 was refused or is unconfirmed');
    expect(apiMock.scene.commit).toHaveBeenCalledTimes(2);
    expect(test.reset).not.toHaveBeenCalled();
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
  },
);

it('cancel after one detach preserves its confirmed ID and sends no second detach or Close', async () => {
  const test = standaloneFixture('idle_after_reset', ['held.1', 'held.2']);
  test.request.waitForObservation.mockImplementationOnce(async () => {
    test.publishScene(2, ['held.2']); test.abort.abort(new Error('operator cancelled Close'));
  });
  await expect(closeScenario(test.request)).rejects.toThrow('Detached objects confirmed: held.1');
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
});

it('the unconfirmed Close primitive does not invent a Reset or manual detach', async () => {
  expect(await prepareSceneForClose(inputs())).toEqual({ resetDone: false, detached: [] });
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(await closeScenario(inputs())).toEqual(empty);
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
});


it('confirmed replacement Resets a terminal Program once and observes its own idle before any target retirement', async () => {
  const test = standaloneFixture('terminal_awaiting_reset');
  test.request.waitForObservation.mockImplementationOnce(async () => {
    expect(test.reset).toHaveBeenCalledTimes(1);
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    test.cycle({ state: 'resetting' });
  }).mockImplementationOnce(async () => {
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    test.cycle({ state: 'idle_after_reset', cycleGeneration: 8, programRunId: null, cycleClaimGeneration: null,
      causalCorrelationId: test.reset.mock.calls[0][0].correlationId });
  });
  expect(await closeScenario(test.request)).toEqual(empty);
  expect(test.reset).toHaveBeenCalledExactlyOnceWith({ correlationId: expect.any(String), expected: {
    programRunId: 'run.1', programRunnerOwnerEpoch: 'runner.1', cycleGeneration: 7 } });
  const retire = apiMock.cclc.retireTargets.mock.invocationCallOrder[0];
  expect(test.request.waitForObservation.mock.invocationCallOrder.filter((call) => call < retire)).toHaveLength(2);
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
});

it('confirmed idle replacement detaches carried objects while live before retiring, with no Reset', async () => {
  const test = standaloneFixture('idle_after_reset', ['held.1', 'held.2']);
  test.request.waitForObservation.mockImplementationOnce(async () => {
    expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    test.publishScene(2, ['held.2']);
  }).mockImplementationOnce(async () => {
    expect(apiMock.scene.commit).toHaveBeenCalledTimes(2);
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    test.publishScene(3, []);
  });
  expect(await closeScenario(test.request)).toEqual(empty);
  expect(test.reset).not.toHaveBeenCalled();
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(2);
  expect(apiMock.scene.commit.mock.calls.map(([request]) => request.expected_projection_revision.scene_revision.revision_id)).toEqual([1, 2]);
  expect(test.current.authoringAdmission.admissionGeneration).toBe(8);
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
});

it('shared Close detaches carried objects retained by ordinary Reset before unloading', async () => {
  const test = standaloneFixture('terminal_awaiting_reset', ['held.1']);
  test.request.waitForObservation.mockImplementationOnce(async () => {
    expect(apiMock.scene.commit).not.toHaveBeenCalled();
    test.cycle({ state: 'idle_after_reset', cycleGeneration: 8, programRunId: null, cycleClaimGeneration: null,
      causalCorrelationId: test.reset.mock.calls[0][0].correlationId });
  }).mockImplementationOnce(async () => {
    expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    test.publishScene(2, []);
  });
  expect(await closeScenario(test.request)).toEqual(empty);
  expect(test.reset).toHaveBeenCalledTimes(1);
  expect(apiMock.scene.commit).toHaveBeenCalledExactlyOnceWith(expect.objectContaining({
    expected_authoring_workspace: test.workspace,
    operation: expect.objectContaining({ kind: 'detach_carried_object_at_current_pose', expected_scene_resource_id: 'held.1' }),
  }), test.request.signal);
  expect(apiMock.scene.commit.mock.invocationCallOrder[0]).toBeLessThan(apiMock.cclc.retireTargets.mock.invocationCallOrder[0]);
});

it.each(['running', 'stopping', 'starting', 'resetting', 'recovery_required'] as const)(
  'confirmed replacement in %s sends no Reset or Close effect', async (state) => {
    const test = standaloneFixture(state);
    await expect(closeScenario(test.request)).rejects.toThrow('Stop motion before closing the Scenario');
    expect(test.reset).not.toHaveBeenCalled();
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
    expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
    expect(apiMock.scene.commit).not.toHaveBeenCalled();
  },
);

it.each(['refused', 'unconfirmed'] as const)('confirmed replacement stops after Reset %s without retirement or resend', async (status) => {
  const test = standaloneFixture('terminal_awaiting_reset');
  test.reset.mockImplementationOnce(async (options) => status === 'refused'
    ? { status, message: 'Reset refused by current owner' }
    : { status, message: 'Reset delivery unknown', reset: { requestId: 'reset.1', correlationId: options.correlationId, ...options.expected } });
  await expect(closeScenario(test.request)).rejects.toThrow(status === 'refused' ? 'Reset refused' : 'Reset delivery unknown');
  expect(test.reset).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
});

it.each(['workspace', 'foreign_idle'])(
  'confirmed replacement refuses observed %s during the post-Reset retirement offer', async (change) => {
    const test = standaloneFixture('terminal_awaiting_reset');
    test.request.waitForObservation.mockImplementationOnce(async () => {
      test.cycle({ state: 'idle_after_reset', cycleGeneration: 8, programRunId: null, cycleClaimGeneration: null,
        causalCorrelationId: test.reset.mock.calls[0][0].correlationId });
    });
    apiMock.cclc.targetRetirementOffer.mockImplementationOnce(async () => {
      if (change === 'workspace') test.change({ ...test.current,
        authoringWorkspace: { ...test.workspace, workspaceRevision: 2, deploymentDraftRevision: 5 } });
      else test.cycle({ cycleGeneration: 9, causalCorrelationId: 'another.reset' });
      return { scope, targets: [] };
    });
    await expect(closeScenario(test.request)).rejects.toThrow('Reset done; Close not completed');
    expect(test.reset).toHaveBeenCalledTimes(1);
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
    expect(apiMock.scene.commit).not.toHaveBeenCalled();
  },
);


it('empty-start Close without a captured replacement preparation skips Reset and completes the ordinary empty handoff', async () => {
  const predecessor: ManagerActiveProjection = { ...empty,
    cellGraph: { controllable_components: [] }, publication: { ...empty.publication,
      publication_generation: 2, durable_public_pointer_content_digest: scope.active_public_pointer_digest } };
  const request = { ...inputs(), expected: predecessor, preparation: undefined };
  apiMock.manager.status.mockReset().mockResolvedValueOnce(projection(predecessor))
    .mockResolvedValue(projection(empty));
  expect(await closeScenario(request)).toEqual(empty);
  expect(request.progress).not.toHaveBeenCalledWith('Resetting the Program before Close…');
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(apiMock.cclc.component).not.toHaveBeenCalled();
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
  expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
});

function heldCloseFixture() {
  const test = standaloneFixture('running');
  const runner = 'program-runner-9f8ad954ed9a49c1918d451f3c602f66';
  const run: NonNullable<ProgramCycleStatus['run']> = {
    schemaVersion: 'manyforge.program_run_status.v3', ownerId: 'program.runner', ownerEpoch: runner,
    runRevision: 41, state: 'intervention_required', treeStatus: 'running', nodeStatusRevision: 13,
    nodeStatuses: { left: 'held', right: 'held', control_flow: 'running' }, activeOperationCount: null, operationFailures: [],
    reason: null, message: 'Motion held after ordinary Stop', capture: {
      programRunId: 'run.1', appliedRecordDigest: 'applied.digest', scenarioId: 'scenario.current',
      sourceDocumentDigest: 'source.digest', programId: 'program.current', programRevision: 'program.revision',
      programBindingId: 'binding.current', retainedBundleId: active.retainedBundleId,
      graphRevision: active.graphRevision, nodeEnvironmentDigest: 'environment.digest', nodePolicies: [],
      nodePolicyDigest: 'policy.digest', ownerIdentities: [],
    },
  };
  const runtimeControls: NonNullable<ProgramCycleStatus['runtimeControls']> = {
    schemaVersion: 'manyforge.program_runtime_control_projection.v9', ownerId: run.ownerId, ownerEpoch: runner,
    programRunId: 'run.1', runRevision: run.runRevision, recoveryOffer: null, recoveryHolds: [],
    runtimeControls: { schemaVersion: 'manyforge.program_runtime_controls.v3', programRunId: 'run.1',
      runtimeControlRevision: 5, recoveryFences: { programGeneration: 1, deploymentGeneration: 1,
        sceneAdmissionEpoch: 1, poseSourceApplyGeneration: 1 }, audit: [] },
    executionSelections: { selectionRevision: 7, motionHeld: true, groups: [{ groupId: 'all',
      nodeCount: 3, heldNodeCount: 2, state: 'mixed', stopAllowed: true, startAllowed: true,
      startUnavailableReason: null, resumeAllowed: true, resumeUnavailableReason: null,
      runtimeScaleSelector: null, runtimeScaleUnavailableReason: 'No scale selector', resourceIds: [] }] },
  };
  test.change({ ...test.current, programCycle: { ...test.current.programCycle, programRunnerOwnerEpoch: runner,
    runRevision: run.runRevision, nodeStatusRevision: run.nodeStatusRevision, runtimeControlRevision: 5,
    run, runtimeControls, allowedCommands: [{ commandKind: 'program_reset', allowed: true, disabledReason: null,
      semanticTarget: { program_runner_owner_epoch: runner, program_run_id: 'run.1', cycle_generation: 7 } }] } });
  test.request.preparation.confirmed.closeCycle = test.current.programCycle;
  return test;
}

it('shared Close Resets one identity-matched held run, using the owner fact despite non-effectful mixed counts', async () => {
  const test = heldCloseFixture();
  test.request.waitForObservation.mockImplementationOnce(async () => {
    expect(test.reset).toHaveBeenCalledTimes(1);
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    test.cycle({ state: 'resetting' });
  }).mockImplementationOnce(async () => {
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    test.cycle({ state: 'idle_after_reset', programRunId: null, cycleGeneration: 8, cycleClaimGeneration: null,
      causalCorrelationId: test.reset.mock.calls[0][0].correlationId });
  });
  expect(await closeScenario(test.request)).toEqual(empty);
  expect(test.reset).toHaveBeenCalledExactlyOnceWith({ correlationId: expect.any(String), expected: {
    programRunId: 'run.1', programRunnerOwnerEpoch: 'program-runner-9f8ad954ed9a49c1918d451f3c602f66', cycleGeneration: 7 } });
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
});

it.each(['moving', 'unknown_held', 'missing_run', 'missing_controls', 'run_owner', 'run_id', 'run_revision',
  'control_owner', 'control_run_id', 'control_revision', 'runtime_revision', 'reset_refused'])(
  'running Close refuses %s before Reset or any lifecycle effect', async (change) => {
    const test = heldCloseFixture();
    const cycle = test.current.programCycle;
    if (change === 'moving') cycle.runtimeControls!.executionSelections.motionHeld = false;
    if (change === 'unknown_held') delete (cycle.runtimeControls!.executionSelections as unknown as Record<string, unknown>).motionHeld;
    if (change === 'missing_run') cycle.run = null;
    if (change === 'missing_controls') cycle.runtimeControls = null;
    if (change === 'run_owner') cycle.run!.ownerEpoch = 'program-runner.other';
    if (change === 'run_id') cycle.run!.capture.programRunId = 'run.other';
    if (change === 'run_revision') cycle.runRevision = 42;
    if (change === 'control_owner') cycle.runtimeControls!.ownerEpoch = 'program-runner.other';
    if (change === 'control_run_id') cycle.runtimeControls!.programRunId = 'run.other';
    if (change === 'control_revision') cycle.runtimeControls!.runRevision = 40;
    if (change === 'runtime_revision') cycle.runtimeControls!.runtimeControls.runtimeControlRevision = 4;
    if (change === 'reset_refused') cycle.allowedCommands[0].allowed = false;
    await expect(closeScenario(test.request)).rejects.toThrow('Stop motion before closing the Scenario');
    expect(test.reset).not.toHaveBeenCalled();
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
    expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
    expect(apiMock.scene.commit).not.toHaveBeenCalled();
  },
);

it.each(['refused', 'unconfirmed'] as const)('held-run Reset %s stops without Close, detach or resend', async (status) => {
  const test = heldCloseFixture();
  test.reset.mockImplementationOnce(async (options) => status === 'refused'
    ? { status, message: 'Held Reset refused' }
    : { status, message: 'Held Reset unconfirmed', reset: { requestId: 'reset.1', correlationId: options.correlationId, ...options.expected } });
  await expect(closeScenario(test.request)).rejects.toThrow(status === 'refused' ? 'Held Reset refused' : 'Held Reset unconfirmed');
  expect(test.reset).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
});

function pendingResetObservationFixture() {
  const test = standaloneFixture('terminal_awaiting_reset');
  let shell: Parameters<typeof resetCloseSnapshot>[0] = {
    snapshot: test.current, transportStatus: 'connected', semanticStatus: 'current',
  };
  let strictReadsDuringSynchronization = 0;
  const strictRead = vi.fn(() => {
    if (shell.semanticStatus === 'synchronizing') strictReadsDuringSynchronization += 1;
    if (shell.snapshot === null || shell.transportStatus !== 'connected' || shell.semanticStatus !== 'current')
      throw new Error('Control source is unavailable; Close is unresolved.');
    return shell.snapshot;
  });
  const pendingRead = vi.fn(() => resetCloseSnapshot(shell));
  test.request.preparation.readSnapshot = strictRead;
  test.request.preparation.readResetSnapshot = pendingRead;
  test.reset.mockImplementation(async (options) => {
    shell = { snapshot: test.current, transportStatus: 'connected', semanticStatus: 'synchronizing' };
    return { status: 'sent', reset: { requestId: 'reset.1', correlationId: options.correlationId, ...options.expected } };
  });
  const idle = () => test.cycle({ state: 'idle_after_reset', programRunId: null, cycleGeneration: 8,
    cycleClaimGeneration: null, causalCorrelationId: test.reset.mock.calls[0][0].correlationId });
  return { ...test, idle, pendingRead, get current() { return test.current; },
    observe: (patch: Partial<Parameters<typeof resetCloseSnapshot>[0]> = {}) => {
      shell = { snapshot: test.current, transportStatus: 'connected', semanticStatus: 'current', ...patch };
    },
    get strictReadsDuringSynchronization() { return strictReadsDuringSynchronization; },
  };
}

it('Reset snapshot classification withholds synchronization and keeps unavailable or disconnected control fatal', () => {
  const snapshot = standaloneFixture().current;
  expect(resetCloseSnapshot({ snapshot, transportStatus: 'connected', semanticStatus: 'synchronizing' })).toBeNull();
  expect(resetCloseSnapshot({ snapshot, transportStatus: 'connected', semanticStatus: 'current' })).toBe(snapshot);
  for (const source of [
    { snapshot: null, transportStatus: 'connected', semanticStatus: 'synchronizing' },
    { snapshot, transportStatus: 'connected', semanticStatus: 'unavailable' },
    { snapshot, transportStatus: 'disconnected', semanticStatus: 'synchronizing' },
    { snapshot, transportStatus: 'stale', semanticStatus: 'synchronizing' },
  ] as const) expect(() => resetCloseSnapshot(source)).toThrow('Control source is unavailable');
});

it('accepted Reset waits through two synchronization gaps before exact correlated idle and one replacement Close', async () => {
    const test = pendingResetObservationFixture();
    test.request.waitForObservation.mockImplementationOnce(async () => {
      expect(test.reset).toHaveBeenCalledTimes(1);
      expect(test.pendingRead).toHaveBeenCalledTimes(1);
      expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
      test.idle();
      // Even the correct idle body cannot grant freshness during synchronization.
      test.observe({ semanticStatus: 'synchronizing' });
    }).mockImplementationOnce(async () => {
      expect(test.pendingRead).toHaveBeenCalledTimes(2);
      expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
      expect(apiMock.scene.commit).not.toHaveBeenCalled();
      expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
      test.observe();
    });
    expect(await closeScenario(test.request)).toEqual(empty);
    expect(test.reset).toHaveBeenCalledTimes(1);
    expect(test.pendingRead).toHaveBeenCalledTimes(3);
    expect(test.strictReadsDuringSynchronization).toBe(0);
    expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
    expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
    expect(apiMock.scene.commit).not.toHaveBeenCalled();
});

it('workspace change after accepted Reset synchronization refuses before retirement without repeating Reset', async () => {
  const test = pendingResetObservationFixture();
  test.request.waitForObservation.mockImplementationOnce(async () => {
    test.idle();
    test.change({ ...test.current, authoringWorkspace: { ...test.workspace,
      workspaceRevision: 2, deploymentDraftRevision: test.workspace.deploymentDraftRevision + 1 } });
    test.observe();
  });
  await expect(closeScenario(test.request)).rejects.toThrow('Reset done; Close not completed');
  expect(test.reset).toHaveBeenCalledTimes(1);
  expect(test.pendingRead).toHaveBeenCalledTimes(2);
  expect(test.strictReadsDuringSynchronization).toBe(0);
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
});

it.each(['unavailable', 'disconnected'] as const)(
  'accepted Reset synchronization followed by %s control is fatal without a retry or Close', async (condition) => {
    const test = pendingResetObservationFixture();
    test.request.waitForObservation.mockImplementationOnce(async () => {
      test.observe(condition === 'unavailable' ? { semanticStatus: 'unavailable' } : { transportStatus: 'disconnected' });
    });
    await expect(closeScenario(test.request)).rejects.toThrow('Control source is unavailable');
    expect(test.reset).toHaveBeenCalledTimes(1);
    expect(test.pendingRead).toHaveBeenCalledTimes(2);
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    expect(apiMock.scene.commit).not.toHaveBeenCalled();
    expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
  },
);

it('caller abort during an accepted-Reset synchronization gap prevents every next read and Close effect', async () => {
  const test = pendingResetObservationFixture();
  test.request.waitForObservation.mockImplementationOnce(async () => { test.abort.abort(new Error('Close caller unmounted')); });
  await expect(closeScenario(test.request)).rejects.toThrow('Close caller unmounted');
  expect(test.reset).toHaveBeenCalledTimes(1);
  expect(test.pendingRead).toHaveBeenCalledTimes(1);
  expect(test.strictReadsDuringSynchronization).toBe(0);
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
});

it.each(['refused', 'unconfirmed'] as const)(
  'a %s Reset never enters the pending observation reader', async (status) => {
    const test = pendingResetObservationFixture();
    test.reset.mockImplementationOnce(async (options) => status === 'refused'
      ? { status, message: 'Reset refused' }
      : { status, message: 'Reset delivery unknown', reset: { requestId: 'reset.1', correlationId: options.correlationId, ...options.expected } });
    await expect(closeScenario(test.request)).rejects.toThrow(status === 'refused' ? 'Reset refused' : 'Reset delivery unknown');
    expect(test.reset).toHaveBeenCalledTimes(1);
    expect(test.pendingRead).not.toHaveBeenCalled();
    expect(test.request.waitForObservation).not.toHaveBeenCalled();
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    expect(apiMock.scene.commit).not.toHaveBeenCalled();
  },
);

it('a refused lifecycle step keeps what the refusal waits for in the Close message', async () => {
  // The API client's refusal carries the owner's structured details.
  const refusal = Object.assign(new Error('lifecycle mutation requires settled physical and Program work'), {
    structuredError: {
      code: 'execution_rejected',
      message: 'lifecycle mutation requires settled physical and Program work',
      details: { blocking_conditions: 'resources_not_free', busy_resource_ids: 'resource.arm' },
      origin: null,
    },
  });
  apiMock.cclc.submitLifecycle.mockRejectedValue(refusal);
  const caught = await closeScenario(inputs()).then(() => null, (error: unknown) => error);
  // The Close surface renders every refusal through errorMessage().
  expect(errorMessage(caught, 'Scenario replacement is unresolved.')).toBe(
    'lifecycle mutation requires settled physical and Program work '
    + '(waiting for: resources_not_free; resources: resource.arm)',
  );
  expect(apiMock.manager.applyCandidate).not.toHaveBeenCalled();
});
