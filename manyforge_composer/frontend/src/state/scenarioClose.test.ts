import { beforeEach, expect, it, vi } from 'vitest';
import type { CarriedObjectAttachment, LogicalSceneObject, LogicalSceneProjection, ManagerActiveProjection,
  ProgramCycleStatus, SceneTransactionRequest } from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';

const apiMock = vi.hoisted(() => ({ manager: { status: vi.fn(), canonicalEmptySelection: vi.fn(),
  prepareCanonicalEmpty: vi.fn(), previewCandidate: vi.fn(), applyCandidate: vi.fn(), applyStatus: vi.fn() },
  execution: { identity: vi.fn(), forceClose: vi.fn() }, cclc: { targetRetirementOffer: vi.fn(), retireTargets: vi.fn(),
    ownerSettlement: vi.fn(), owner: vi.fn(), component: vi.fn(), submitLifecycle: vi.fn(), lifecycleStatus: vi.fn(),
    componentProcessStopOffer: vi.fn(), stopComponentProcess: vi.fn(), componentProcessStopResult: vi.fn() },
  scene: { commit: vi.fn() }, programs: { sceneClearStatus: vi.fn(), clearEmptyScene: vi.fn() } }));
vi.mock('../api/client', () => ({ api: apiMock }));
import { closeErrorCode, closeScenario, forceCloseScenario, prepareSceneForClose, resetCloseSnapshot, sceneAtOrAfter,
  type ClosePreparationInputs } from './scenarioClose';
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
  waitForObservation: vi.fn(async (_signal?: AbortSignal) => {}), trackApply: vi.fn(), progress: vi.fn() });
beforeEach(() => {
  vi.resetAllMocks();
  apiMock.manager.status.mockResolvedValueOnce({ source, status: { managerReady: true, active } })
    .mockResolvedValue({ source, status: { managerReady: true, active: empty } });
  apiMock.cclc.targetRetirementOffer.mockResolvedValue({ scope, targets: [] });
  apiMock.cclc.retireTargets.mockResolvedValue({ replay_status: 'accepted_new', scope, retired_targets: [] });
  apiMock.cclc.ownerSettlement.mockResolvedValue({ scope, settled: true });
  apiMock.cclc.owner.mockResolvedValue({ requestOwnerEpoch: cclcOwnerEpoch });
  apiMock.execution.identity.mockResolvedValue({ retainedBundleId: active.retainedBundleId, graphRevision: active.graphRevision,
    activePublicationGeneration: 2, ownerEpoch: 'kernel.epoch' });
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
  apiMock.programs.sceneClearStatus.mockResolvedValue({ applyId: 'apply.empty', applyGeneration: 4, state: 'succeeded', remaining: [], reason: null });
});
it('times out an unavailable clear as unknown and permits the replacement to continue', async () => {
  vi.useFakeTimers();
  try {
    const request = { ...inputs(), sceneClearNotice: vi.fn() };
    apiMock.programs.sceneClearStatus.mockReturnValue(new Promise(() => {}));
    const result = closeScenario(request);
    await vi.advanceTimersByTimeAsync(10_000);
    expect(await result).toEqual(empty);
    expect(request.sceneClearNotice).toHaveBeenCalledWith({ applyId: 'apply.empty', applyGeneration: 4 }, expect.objectContaining({ code: 'scene_clear_unknown' }));
    expect(request.progress).not.toHaveBeenCalledWith('Scenario closed.');
  } finally { vi.useRealTimers(); }
});

it.each(['partial', 'failed', 'unknown'])('reports %s and permits replacement without claiming clearing succeeded', async (state) => {
  const request = { ...inputs(), sceneClearNotice: vi.fn() };
  apiMock.programs.sceneClearStatus.mockResolvedValue({ applyId: 'apply.empty', applyGeneration: 4, state,
    remaining: ['held.1'], reason: { code: 'detach_refused', message: 'Detach refused', details: {} } });
  expect(await closeScenario(request)).toEqual(empty);
  const error = (request.sceneClearNotice.mock.calls as unknown as [unknown, Error][])[0][1];
  expect(closeErrorCode(error)).toBe(`scene_clear_${state}`);
  expect(error.message).toContain('held.1');
  expect(error.message).toContain(state === 'unknown' ? 'Last observed objects' : 'Remaining objects');
  expect(request.progress).not.toHaveBeenCalledWith('Scenario closed.');
});

it('reports a status read error as unknown and permits replacement', async () => {
  const request = { ...inputs(), sceneClearNotice: vi.fn() };
  apiMock.programs.sceneClearStatus.mockRejectedValue(new Error('Unavailable'));
  expect(await closeScenario(request)).toEqual(empty);
  expect(closeErrorCode((request.sceneClearNotice.mock.calls as unknown as [unknown, Error][])[0][1])).toBe('scene_clear_unknown');
  expect(request.progress).not.toHaveBeenCalledWith('Scenario closed.');
});

it('does not accept a different Apply as the completed clear', async () => {
  const request = inputs();
  apiMock.programs.sceneClearStatus.mockResolvedValue({ applyId: 'apply.other', applyGeneration: 5,
    state: 'succeeded', remaining: [], reason: null });
  apiMock.manager.status.mockResolvedValueOnce(projection(active)).mockResolvedValueOnce(projection(empty))
    .mockResolvedValue(projection(active));
  await expect(closeScenario(request)).rejects.toMatchObject({ code: 'scene_clear_superseded' });
  expect(request.progress).not.toHaveBeenCalledWith('Scenario closed.');
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
  expect(apiMock.manager.applyCandidate.mock.calls[0][0]).toMatchObject({ scene_initialization: 'initialize', prepared_activation: prepared,
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
    .mockResolvedValueOnce(projection(active)).mockResolvedValueOnce(projection(empty))
    .mockResolvedValue(projection(empty));
  expect(await closeScenario(request)).toEqual(empty);
  expect(apiMock.manager.status).toHaveBeenCalledTimes(6);
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
    .mockResolvedValueOnce(projection(reordered)).mockResolvedValueOnce(projection(empty))
    .mockResolvedValue(projection(empty));
  expect(await closeScenario(request)).toEqual(empty);
  expect(apiMock.manager.status).toHaveBeenCalledTimes(4);
  expect(request.waitForObservation).toHaveBeenCalledTimes(3);
});

it.each([
  ['stale predecessor', projection(active, { ...source, freshness: 'stale' })],
  ['unavailable predecessor', projection(active, { ...source, available: false, freshness: 'unavailable' })],
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

it('accepts the exact empty Apply publication after a Manager owner change', async () => {
  const changedSource = { ...source, sourceOwnerId: 'other.manager', sourceOwnerEpoch: 'other.owner' };
  apiMock.manager.status.mockReset().mockResolvedValueOnce(projection(active))
    .mockResolvedValue(projection(empty, changedSource));
  expect(await closeScenario(inputs())).toEqual(empty);
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
    .mockResolvedValueOnce(projection(candidate, candidateSource))
    .mockResolvedValue(projection(candidate, candidateSource));
  apiMock.programs.sceneClearStatus.mockResolvedValue({ ...accepted, state: 'succeeded', remaining: [], reason: null });
  apiMock.manager.canonicalEmptySelection.mockResolvedValue({ request_owner_epoch: predecessorSource.sourceOwnerEpoch,
    cell_id: 'ui6i_ur5e_lite6_wall_cell', deployment_artifact_id: actualPrepared.deployment_artifact_id,
    deployment_revision: actualPrepared.deployment_revision, candidate_retained_bundle_id: candidate.retainedBundleId,
    candidate_graph_revision: candidate.graphRevision, admission_digest: 'sha256:a21efe1461d8c6fa3ad31b0827d1796ee915e9e06b2fb68eda714349fa461204' });
  apiMock.manager.prepareCanonicalEmpty.mockResolvedValue(actualPrepared);
  apiMock.manager.applyCandidate.mockResolvedValue({ ...accepted, state: 'pending' });
  apiMock.manager.applyStatus.mockReset().mockResolvedValue({ ...accepted, state: 'succeeded' });
  expect(await closeScenario(request)).toEqual(candidate);
  expect(request.waitForObservation).toHaveBeenCalledTimes(1);
  expect(apiMock.manager.status).toHaveBeenCalledTimes(4);
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
    executionControlStatus: { manualOperations: [], cellStopAction: { fault: null } },
    componentOverview: { readiness: 'ready', userMessage: 'Current', components: [], maintenanceOperation: null, basis: {} },
    runtime: { sections: { logicalScene: section, manager: { available: true, freshness: 'fresh', knownGaps: [],
      sourceOwnerId: source.sourceOwnerId, sourceOwnerEpoch: source.sourceOwnerEpoch,
      payload: { active: { canonical_empty: active.canonicalEmpty, retained_bundle_id: active.retainedBundleId,
        graph_revision: active.graphRevision, publication: active.publication } } } } } } as unknown as ComposerHeadlessState;
  const reset = vi.fn<ClosePreparationInputs['reset']>(async (options) => ({ status: 'sent', reset: {
    requestId: 'reset.1', correlationId: options.correlationId,
    programRunnerOwnerEpoch: current.programCycle.programRunnerOwnerEpoch, ...options.expected } }));
  const preparation: ClosePreparationInputs = { confirmed: { authoring: workspace,
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
  expect(test.reset).toHaveBeenCalledExactlyOnceWith({ correlationId: expect.any(String), keepStopFault: true, expected: {
    programRunId: 'run.1', cycleGeneration: 7 } });
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

it.each(['publication', 'pointer'])(
  'standalone Close refuses a changed retirement %s before sending an effect', async (field) => {
    const test = standaloneFixture();
    apiMock.cclc.targetRetirementOffer.mockResolvedValue({ scope: { ...scope,
      ...(field === 'publication' ? { publication_generation: scope.publication_generation + 1 } : {}),
      ...(field === 'pointer' ? { active_public_pointer_digest: `sha256:${'a'.repeat(64)}` } : {}),
    }, targets: [] });
    await expect(closeScenario(test.request)).rejects.toThrow('The lifecycle retirement publication changed.');
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
    expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
  },
);

it('standalone Close uses the current CCLC owner after retirement', async () => {
  const test = standaloneFixture();
  const changed = `sha256:${'9'.repeat(64)}`;
  apiMock.cclc.owner.mockResolvedValue({ requestOwnerEpoch: changed });
  apiMock.cclc.component.mockResolvedValue({ ...component, readiness: { requestOwnerEpoch: changed } });
  expect(await closeScenario(test.request)).toEqual(empty);
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.submitLifecycle.mock.calls[0][0].request_context.request_owner_epoch).toBe(changed);
  expect(apiMock.manager.prepareCanonicalEmpty).toHaveBeenCalledTimes(1);
});

it('continues Close across owner and transport-freshness changes when the confirmed content and cycle match', async () => {
  const test = standaloneFixture();
  test.change({ ...test.current, receiverEpoch: 'receiver.2',
    programCycle: { ...test.current.programCycle, programRunnerOwnerEpoch: 'runner.2', cycleClaimGeneration: 8 },
    runtime: { ...test.current.runtime, sections: { ...test.current.runtime.sections,
      manager: { ...test.current.runtime.sections.manager, available: false, freshness: 'stale',
        knownGaps: ['reconnecting'], sourceOwnerId: 'manager.2', sourceOwnerEpoch: 'manager.2' },
      logicalScene: { ...test.current.runtime.sections.logicalScene,
        sourceOwnerId: 'scene.2', sourceOwnerEpoch: 'scene.2' },
    } } } as ComposerHeadlessState);
  expect(await closeScenario(test.request)).toEqual(empty);
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
});

it.each(['unavailable', 'stale', 'gap'] as const)(
  'refuses the detach phase when the Scene observation is %s', async (reason) => {
    const test = standaloneFixture('idle_after_reset', ['held.1']);
    const section = test.current.runtime.sections.logicalScene;
    test.change({ ...test.current, runtime: { ...test.current.runtime, sections: { ...test.current.runtime.sections,
      logicalScene: { ...section, ...(reason === 'unavailable' ? { available: false } : {}),
        ...(reason === 'stale' ? { freshness: 'stale' } : {}),
        ...(reason === 'gap' ? { knownGaps: ['reconnecting'] } : {}) },
    } } } as ComposerHeadlessState);
    await expect(closeScenario(test.request)).rejects.toThrow('The current logical Scene owner is unavailable.');
    expect(apiMock.scene.commit).not.toHaveBeenCalled();
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  },
);

it.each(['run', 'generation', 'settlement'] as const)(
  'refuses Close when the confirmed cycle %s changes', async (field) => {
    const test = standaloneFixture('terminal_awaiting_reset');
    test.cycle(field === 'run' ? { programRunId: 'run.2' }
      : field === 'generation' ? { cycleGeneration: 8 }
        : { terminalOrSettlementReference: 'terminal.2' });
    await expect(closeScenario(test.request)).rejects.toThrow('The confirmed Program cycle or logical Scene changed.');
    expect(test.reset).not.toHaveBeenCalled();
    expect(apiMock.scene.commit).not.toHaveBeenCalled();
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  },
);

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

it('uses the current Scene owner for each detach after a rejoin', async () => {
  const test = standaloneFixture('idle_after_reset', ['held.1', 'held.2']);
  test.request.waitForObservation.mockImplementationOnce(async () => {
    test.publishScene(2, ['held.2']);
    test.change({ ...test.current, runtime: { ...test.current.runtime, sections: { ...test.current.runtime.sections,
      logicalScene: { ...test.current.runtime.sections.logicalScene, sourceOwnerId: 'scene.owner.2',
        sourceOwnerEpoch: 'scene.epoch.2' },
    } } } as ComposerHeadlessState);
  }).mockImplementationOnce(async () => test.publishScene(3, []));
  expect(await closeScenario(test.request)).toEqual(empty);
  expect(apiMock.scene.commit.mock.calls.map(([transaction]) => transaction.request_context.request_owner_epoch))
    .toEqual(['scene.epoch', 'scene.epoch.2']);
});

const staleRevision = () => Object.assign(new Error('expected composite projection revision is no longer active'),
  { payload: { ok: false, error: { code: 'stale_revision' }, projection_status: 'not_attempted' } });

it('compares Scene lineage and accepts a same-or-later revision only', () => {
  const base = { retained_bundle_id: 'bundle', graph_revision: 'graph', scene_revision: { scene_id: 'scene', revision_id: 4, content_hash: 'h4' } };
  const at = (patch: Partial<typeof base.scene_revision>, other: Partial<typeof base> = {}) =>
    ({ ...base, ...other, scene_revision: { ...base.scene_revision, ...patch } });
  expect(sceneAtOrAfter(at({}), base)).toBe(true);
  expect(sceneAtOrAfter(at({ revision_id: 9, content_hash: 'h9' }), base)).toBe(true);
  expect(sceneAtOrAfter(at({ revision_id: 3, content_hash: 'h3' }), base)).toBe(false);
  expect(sceneAtOrAfter(at({ content_hash: 'forked' }), base)).toBe(false);
  expect(sceneAtOrAfter(at({ scene_id: 'other', revision_id: 9 }), base)).toBe(false);
  expect(sceneAtOrAfter(at({ revision_id: 9 }, { retained_bundle_id: 'other' }), base)).toBe(false);
  expect(sceneAtOrAfter(at({ revision_id: 9 }, { graph_revision: 'other' }), base)).toBe(false);
});

it('pose-only Scene revisions during Close detach at the latest revision and keep every later step', async () => {
  const test = standaloneFixture('idle_after_reset', ['held.1']);
  // A pose update after confirmation: new Scene and attachment revisions, same objects.
  test.publishScene(3, ['held.1']);
  const moved = test.current.runtime.sections.logicalScene!.payload as unknown as LogicalSceneProjection;
  moved.snapshot!.carried_object_attachments[0].carried_object_attachment_revision = 'sha256:' + 'b'.repeat(64);
  // Another one lands between the read and the detach: refused, read again, retried once.
  apiMock.scene.commit.mockImplementationOnce(async () => { test.publishScene(4, ['held.1']); throw staleRevision(); });
  test.request.waitForObservation.mockImplementationOnce(async () => { test.publishScene(7, []); });
  apiMock.cclc.targetRetirementOffer.mockImplementationOnce(async () => {
    test.publishScene(8, []);
    return { scope, targets: [] };
  });
  expect(await closeScenario(test.request)).toEqual(empty);
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(2);
  const [[first], [second]] = apiMock.scene.commit.mock.calls;
  expect(first.expected_projection_revision.scene_revision).toEqual({ scene_id: 'scene', revision_id: 3, content_hash: 'hash.3' });
  expect(first.operation).toMatchObject({ carried_object_attachment_id: 'attachment.held.1',
    expected_carried_object_attachment_revision: 'sha256:' + 'b'.repeat(64) });
  expect(second.expected_projection_revision.scene_revision).toEqual({ scene_id: 'scene', revision_id: 4, content_hash: 'hash.4' });
  expect(second.operation).toMatchObject({ expected_carried_object_attachment_revision: 'sha256:' + 'a'.repeat(64) });
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
  expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
});

it('a changed attachment set since confirmation still refuses Close before any effect', async () => {
  const test = standaloneFixture('idle_after_reset', ['held.1']);
  test.publishScene(2, []);
  await expect(closeScenario(test.request)).rejects.toThrow('The confirmed Program cycle or logical Scene changed.');
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
});

it('bounds the read-latest retry of a detach refused only for a newer revision', async () => {
  const test = standaloneFixture('idle_after_reset', ['held.1']);
  let revision = 1;
  apiMock.scene.commit.mockImplementation(async () => { revision += 1; test.publishScene(revision, ['held.1']); throw staleRevision(); });
  await expect(closeScenario(test.request)).rejects.toThrow('Detach of held.1 was refused or is unconfirmed');
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(8);
  expect(apiMock.scene.commit.mock.calls.map(([request]) => request.expected_projection_revision.scene_revision.revision_id))
    .toEqual([1, 2, 3, 4, 5, 6, 7, 8]);
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
});

it.each(['refused', 'unconfirmed'] as const)('Reset %s stops without any Close continuation or resend', async (status) => {
  const test = standaloneFixture('terminal_awaiting_reset');
  test.reset.mockImplementationOnce(async (options) => status === 'refused'
    ? { status, message: 'Reset refused by current owner' }
    : { status, message: 'Reset delivery unknown', reset: { requestId: 'reset.1', correlationId: options.correlationId,
      programRunnerOwnerEpoch: test.current.programCycle.programRunnerOwnerEpoch, ...options.expected } });
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

// D6 (a): Close always works on a latched cell. It retires the targets first,
// so the Kernel admits Reset, Disable and the empty Apply, and it never clears
// the cell Stop fault (no recover_stop_fault call exists on this path).
function latchedFixture(state: ProgramCycleStatus['state'] = 'terminal_awaiting_reset') {
  const test = standaloneFixture(state);
  test.change({ ...test.current, executionControlStatus: { manualOperations: [], cellStopAction: { fault: {
    latchId: 'stop-fault.1.1', latchGeneration: 1, state: 'latched', trigger: 'physical_rest_unproven',
    observedTriggers: ['physical_rest_unproven'], recoveryStatus: 'waiting_for_observation',
    reasonCode: 'component_incident', certificate: null } } } } as unknown as ComposerHeadlessState);
  test.request.waitForObservation.mockImplementationOnce(async () => {
    test.cycle({ state: 'idle_after_reset', cycleGeneration: 8, programRunId: null, cycleClaimGeneration: null,
      causalCorrelationId: test.reset.mock.calls[0][0].correlationId });
  });
  return test;
}

it('D6: Close of a latched cell with an open incident retires first, Resets, closes the incident and keeps the fault', async () => {
  const test = latchedFixture();
  apiMock.cclc.component.mockResolvedValue({ ...component,
    lifecycleProgress: { incident: { incidentId: 'incident.1' } } });
  apiMock.cclc.submitLifecycle.mockImplementation(async (request) => request.intent === 'recover'
    ? { incidentRecovery: { closureReason: 'cell_closed_by_operator' } }
    : { transactionId: 'disable.1', status: { terminalResult: 'succeeded', reconciliationRequired: false } });
  expect(await closeScenario(test.request)).toEqual(empty);
  // Retirement precedes the Program Reset: the Kernel admits the Close steps.
  expect(apiMock.cclc.retireTargets.mock.invocationCallOrder[0])
    .toBeLessThan(test.reset.mock.invocationCallOrder[0]);
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
  // The incident is closed with the History-visible Close reason, then Disable.
  expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => request.intent)).toEqual(['recover', 'disable']);
  expect(apiMock.cclc.submitLifecycle.mock.calls[0][0]).toMatchObject({
    close_incident_recovery: { expected_incident_id: 'incident.1' }, reason: 'cell closed by operator' });
  expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
});

it('D6: an unavailable control projection stops Close before any Reset or detach', async () => {
  const test = latchedFixture();
  test.change({ ...test.current, executionControlStatus: null } as unknown as ComposerHeadlessState);
  const refused = closeScenario(test.request);
  await expect(refused).rejects.toThrow('Cell control state is unavailable');
  await expect(refused.catch((error: unknown) => (error as { code?: string }).code)).resolves.toBe('cell_state_unavailable');
  expect(test.reset).not.toHaveBeenCalled();
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
});

it('clears a canonical-empty Scene through one direct owner request, without an Apply', async () => {
  const test = standaloneFixture('idle_after_reset', ['held.1']);
  const predecessor: ManagerActiveProjection = { ...empty, publication: { ...empty.publication,
    publication_generation: 2, transaction: { kind: 'named_apply', apply_id: 'apply.previous', apply_generation: 3 } } };
  const previousPayload = { canonical_empty: true, retained_bundle_id: predecessor.retainedBundleId,
    graph_revision: predecessor.graphRevision, publication: predecessor.publication };
  const manager = test.current.runtime.sections.manager;
  const section = test.current.runtime.sections.logicalScene!;
  const scene = structuredClone(section.payload) as unknown as LogicalSceneProjection;
  scene.access.active_projection!.retained_bundle_id = predecessor.retainedBundleId;
  scene.access.active_projection!.graph_revision = predecessor.graphRevision;
  const confirmedScene = { ...section, payload: scene as unknown as Record<string, unknown> };
  test.change({ ...test.current, runtime: { ...test.current.runtime, sections: { ...test.current.runtime.sections,
    logicalScene: confirmedScene, manager: { ...manager, payload: { active: previousPayload } } } } });
  test.request.preparation.confirmed.closeScene = confirmedScene;
  test.request.preparation.confirmed.managerActive = previousPayload;
  apiMock.manager.status.mockReset().mockResolvedValue(projection(predecessor));
  apiMock.programs.clearEmptyScene.mockImplementation(async (request: { request_id: string }) => ({
    requestId: request.request_id, state: 'succeeded', remaining: [], reason: null }));
  expect(await closeScenario({ ...test.request, expected: predecessor })).toEqual(predecessor);
  expect(apiMock.programs.clearEmptyScene).toHaveBeenCalledOnce();
  expect(apiMock.programs.clearEmptyScene).toHaveBeenCalledWith(expect.objectContaining({ clear_empty_scene: true,
    expected_publication_generation: 2, request_owner_epoch: 'runner.1' }), expect.any(AbortSignal));
  expect(apiMock.manager.applyCandidate).not.toHaveBeenCalled();
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
});

it('a non-empty cell without cell control state still refuses Close as cell_state_unavailable', async () => {
  const test = standaloneFixture();
  test.change({ ...test.current, executionControlStatus: null } as unknown as ComposerHeadlessState);
  const refused = closeScenario(test.request);
  await expect(refused.catch((error: unknown) => closeErrorCode(error))).resolves.toBe('cell_state_unavailable');
  expect(test.reset).not.toHaveBeenCalled();
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(apiMock.manager.applyCandidate).not.toHaveBeenCalled();
});

it('SX-679: Close and Force close start nothing while Recover cell runs', async () => {
  const test = standaloneFixture();
  for (const run of [closeScenario, forceCloseScenario]) {
    const refused = run({ ...test.request, cellRecoverRunning: () => true });
    await expect(refused.catch((error: unknown) => closeErrorCode(error)))
      .resolves.toBe('recover_cell_in_progress');
  }
  expect(test.reset).not.toHaveBeenCalled();
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(apiMock.execution.forceClose).not.toHaveBeenCalled();
  expect(apiMock.manager.applyCandidate).not.toHaveBeenCalled();
});

it('D6: a latch appearing after the read keeps it: Close sends its Reset with keepStopFault', async () => {
  const test = standaloneFixture('terminal_awaiting_reset');
  test.request.waitForObservation.mockImplementationOnce(async () => {
    test.cycle({ state: 'idle_after_reset', cycleGeneration: 8, programRunId: null, cycleClaimGeneration: null,
      causalCorrelationId: test.reset.mock.calls[0][0].correlationId });
  });
  expect(await closeScenario(test.request)).toEqual(empty);
  // No early retirement was chosen, yet the Reset never clears a Stop fault.
  expect(test.reset.mock.calls[0][0].keepStopFault).toBe(true);
});

it.each(['refused', 'scene'] as const)(
  'D6: after the early retirement a later %s failure reports the retired targets', async (failure) => {
    const test = latchedFixture();
    if (failure === 'refused') {
      test.reset.mockResolvedValueOnce({ status: 'refused', message: 'Program Reset was rejected.' });
    } else {
      test.change({ ...test.current, runtime: { ...test.current.runtime, sections: { ...test.current.runtime.sections,
        logicalScene: { ...test.current.runtime.sections.logicalScene, available: false } } } } as ComposerHeadlessState);
      test.request.preparation.confirmed.closeCycle = { ...test.current.programCycle, state: 'idle_after_reset',
        programRunId: null, cycleClaimGeneration: null, terminalOrSettlementReference: null };
      test.cycle({ state: 'idle_after_reset', programRunId: null, cycleClaimGeneration: null,
        terminalOrSettlementReference: null });
    }
    const error = await closeScenario(test.request).then(() => new Error('Close completed'), (caught: unknown) => caught as Error);
    expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
    expect(error.message).toContain('Lifecycle targets retired; finish with New or Load.');
    expect(apiMock.manager.applyCandidate).not.toHaveBeenCalled();
  },
);

it('D6: an unconfirmed early retirement is reported as unconfirmed, a refused offer as no effect', async () => {
  const test = latchedFixture();
  apiMock.cclc.retireTargets.mockRejectedValueOnce(new Error('response lost'));
  await expect(closeScenario(test.request)).rejects.toThrow('its outcome is not confirmed');
  expect(test.reset).not.toHaveBeenCalled();
  apiMock.manager.status.mockResolvedValueOnce({ source, status: { managerReady: true, active } });
  apiMock.cclc.targetRetirementOffer.mockRejectedValueOnce(new Error('offer refused'));
  await expect(closeScenario(latchedFixture().request)).rejects.toThrow('offer refused Close not completed.');
});

it.each([
  ['failed', 'faulted'],
  ['connection_lost', 'declared'],
  ['connection_lost', 'disabled'],
  ['connection_lost', 'faulted'],
] as const)(
  'O3: %s Disable with current state %s continues after process Stop', async (terminalResult, currentState) => {
    const test = latchedFixture();
    let componentReads = 0;
    apiMock.cclc.component.mockImplementation(async () => {
      componentReads += 1;
      return { ...component,
        lifecycleState: componentReads < 3 ? 'active' : currentState,
        lifecycleProgress: componentReads === 1
          ? { incident: { incidentId: 'incident.provider_lost' } } : null };
    });
    apiMock.cclc.submitLifecycle.mockImplementation(async (request) => request.intent === 'recover'
      ? { incidentRecovery: { closureReason: 'cell_closed_by_operator' } }
      : { transactionId: 'disable.outcome', status: { terminalResult,
        reconciliationRequired: false, error: { code: 'component_lifecycle_transition_missing',
          message: 'no transition satisfied' } } });
    expect(await closeScenario(test.request)).toEqual(empty);
    expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => request.intent)).toEqual(['recover', 'disable']);
    expect(apiMock.cclc.stopComponentProcess).toHaveBeenCalledTimes(1);
    // The waiting Disable target Close's own request left is retired before Apply.
    expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(2);
    expect(apiMock.cclc.retireTargets.mock.invocationCallOrder[1])
      .toBeLessThan(apiMock.manager.prepareCanonicalEmpty.mock.invocationCallOrder[0]);
    expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
  },
);

it('O3: connection_lost Disable with current Prepared state refuses normal Close with the typed reason', async () => {
  const test = latchedFixture('idle_after_reset');
  test.request.waitForObservation.mockReset().mockResolvedValue(undefined);
  let componentReads = 0;
  apiMock.cclc.component.mockImplementation(async () => {
    componentReads += 1;
    return { ...component, lifecycleState: componentReads < 3 ? 'active' : 'prepared' };
  });
  apiMock.cclc.submitLifecycle.mockResolvedValue({ transactionId: 'disable.connection_lost', status: {
    terminalResult: 'connection_lost', reconciliationRequired: false,
    error: { code: 'controller_connection_lost', message: 'controller connection lost' } } });
  const error = await closeScenario(test.request).then(() => new Error('Close completed'), (caught: unknown) => caught as Error);
  expect(closeErrorCode(error)).toBe('controller_connection_lost');
  expect(error.message).toContain('controller connection lost');
  expect(apiMock.cclc.stopComponentProcess).not.toHaveBeenCalled();
  expect(apiMock.manager.applyCandidate).not.toHaveBeenCalled();
});

it.each(['prepared', 'active', 'degraded'] as const)(
  'O3: failed Disable with current %s state refuses normal Close with the typed reason', async (currentState) => {
    const test = latchedFixture('idle_after_reset');
    test.request.waitForObservation.mockReset().mockResolvedValue(undefined);
    let componentReads = 0;
    apiMock.cclc.component.mockImplementation(async () => {
      componentReads += 1;
      return { ...component, lifecycleState: componentReads < 3 ? 'active' : currentState };
    });
    apiMock.cclc.submitLifecycle.mockResolvedValue({ transactionId: 'disable.partial', status: {
      terminalResult: 'failed', reconciliationRequired: false,
      error: { code: 'controller_deactivate_failed', message: 'deactivate failed' } } });
    const error = await closeScenario(test.request).then(() => new Error('Close completed'), (caught: unknown) => caught as Error);
    expect(closeErrorCode(error)).toBe('controller_deactivate_failed');
    expect(error.message).toContain('deactivate failed');
    expect(apiMock.cclc.component).toHaveBeenCalledTimes(3);
    expect(apiMock.manager.applyCandidate).not.toHaveBeenCalled();
  },
);

it('D6: a faulted component is shut down by its process Stop without a Disable or a recovery', async () => {
  const test = latchedFixture('idle_after_reset');
  test.request.waitForObservation.mockReset().mockResolvedValue(undefined);
  apiMock.cclc.component.mockResolvedValue({ ...component, lifecycleState: 'faulted' });
  expect(await closeScenario(test.request)).toEqual(empty);
  expect(test.reset).not.toHaveBeenCalled();
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  expect(apiMock.cclc.stopComponentProcess).toHaveBeenCalledTimes(1);
  expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
});

it('D6: a degraded component is disabled, not recovered', async () => {
  const test = latchedFixture('idle_after_reset');
  test.request.waitForObservation.mockReset().mockResolvedValue(undefined);
  apiMock.cclc.component.mockResolvedValue({ ...component, lifecycleState: 'degraded' });
  expect(await closeScenario(test.request)).toEqual(empty);
  expect(apiMock.cclc.submitLifecycle.mock.calls.map(([request]) => request.intent)).toEqual(['disable']);
});

it('D6: retained uncertain outputs still refuse Close before any effect on a latched cell', async () => {
  const test = latchedFixture();
  test.change({ ...test.current, executionControlStatus: { ...test.current.executionControlStatus!,
    manualOperations: [{ operationId: 'op.io', operationRevision: 2, resident: true,
      outputEvidence: 'acceptance_unknown', controllableComponentId: 'io', commandTargetId: 'valve' }] } } as unknown as ComposerHeadlessState);
  await expect(closeScenario(test.request)).rejects.toThrow('Output write(s) with unknown acceptance');
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(test.reset).not.toHaveBeenCalled();
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
  expect(test.reset).toHaveBeenCalledExactlyOnceWith({ correlationId: expect.any(String), keepStopFault: true, expected: {
    programRunId: 'run.1', cycleGeneration: 7 } });
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
    : { status, message: 'Reset delivery unknown', reset: { requestId: 'reset.1', correlationId: options.correlationId,
      programRunnerOwnerEpoch: test.current.programCycle.programRunnerOwnerEpoch, ...options.expected } });
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
  const predecessor: ManagerActiveProjection = { ...empty, canonicalEmpty: false,
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
  expect(test.reset).toHaveBeenCalledExactlyOnceWith({ correlationId: expect.any(String), keepStopFault: true, expected: {
    programRunId: 'run.1', cycleGeneration: 7 } });
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
    : { status, message: 'Held Reset unconfirmed', reset: { requestId: 'reset.1', correlationId: options.correlationId,
      programRunnerOwnerEpoch: test.current.programCycle.programRunnerOwnerEpoch, ...options.expected } });
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
  let strictReadsDuringNonCurrentObservation = 0;
  const strictRead = vi.fn(() => {
    if (shell.semanticStatus !== 'current') strictReadsDuringNonCurrentObservation += 1;
    if (shell.snapshot === null || shell.transportStatus !== 'connected' || shell.semanticStatus !== 'current')
      throw new Error('Control source is unavailable; Close is unresolved.');
    return shell.snapshot;
  });
  const pendingRead = vi.fn(() => shell);
  test.request.preparation.readSnapshot = strictRead;
  test.request.preparation.readResetSnapshot = pendingRead;
  test.reset.mockImplementation(async (options) => {
    shell = { snapshot: test.current, transportStatus: 'connected', semanticStatus: 'synchronizing' };
    return { status: 'sent', reset: { requestId: 'reset.1', correlationId: options.correlationId,
      programRunnerOwnerEpoch: test.current.programCycle.programRunnerOwnerEpoch, ...options.expected } };
  });
  const idle = () => test.cycle({ state: 'idle_after_reset', programRunId: null, cycleGeneration: 8,
    cycleClaimGeneration: null, causalCorrelationId: test.reset.mock.calls[0][0].correlationId });
  return { ...test, idle, pendingRead, get current() { return test.current; },
    observe: (patch: Partial<Parameters<typeof resetCloseSnapshot>[0]> = {}) => {
      shell = { snapshot: test.current, transportStatus: 'connected', semanticStatus: 'current', ...patch };
    },
    get strictReadsDuringNonCurrentObservation() { return strictReadsDuringNonCurrentObservation; },
  };
}

it('Reset snapshot classification withholds synchronization and keeps unavailable or disconnected control fatal', () => {
  const snapshot = standaloneFixture().current;
  expect(resetCloseSnapshot({ snapshot, transportStatus: 'connected', semanticStatus: 'synchronizing' })).toBeNull();
  expect(resetCloseSnapshot({ snapshot, transportStatus: 'connected', semanticStatus: 'current' })).toBe(snapshot);
  for (const source of [
    { snapshot: null, transportStatus: 'connected', semanticStatus: 'synchronizing' },
    { snapshot, transportStatus: 'connected', semanticStatus: 'unavailable' },
    { snapshot: null, transportStatus: 'connected', semanticStatus: 'unavailable' },
    { snapshot, transportStatus: 'disconnected', semanticStatus: 'synchronizing' },
    { snapshot, transportStatus: 'stale', semanticStatus: 'synchronizing' },
  ] as const) expect(() => resetCloseSnapshot(source)).toThrow('Control source is unavailable');
});

it('accepted Reset waits through synchronization and temporary unavailable gaps before one replacement Close', async () => {
    const test = pendingResetObservationFixture();
    test.request.waitForObservation.mockImplementationOnce(async () => {
      expect(test.reset).toHaveBeenCalledTimes(1);
      expect(test.pendingRead).toHaveBeenCalledTimes(1);
      expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
      test.idle();
      // Even the correct idle body cannot grant freshness while unavailable.
      test.observe({ semanticStatus: 'unavailable' });
    }).mockImplementationOnce(async () => {
      expect(test.pendingRead).toHaveBeenCalledTimes(2);
      expect(test.pendingRead.mock.results[1]?.value.semanticStatus).toBe('unavailable');
      expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
      expect(apiMock.scene.commit).not.toHaveBeenCalled();
      expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
      test.observe();
    });
    expect(await closeScenario(test.request)).toEqual(empty);
    expect(test.reset).toHaveBeenCalledTimes(1);
    expect(test.pendingRead).toHaveBeenCalledTimes(3);
    expect(test.strictReadsDuringNonCurrentObservation).toBe(0);
    expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(1);
    expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
    expect(apiMock.scene.commit).not.toHaveBeenCalled();
});

it('permanent unavailable refuses at 2 s even when the observation wait hangs', async () => {
  vi.useFakeTimers();
  try {
    const test = pendingResetObservationFixture();
    let enterUnavailableWait!: () => void;
    const unavailableWaitEntered = new Promise<void>((resolve) => { enterUnavailableWait = resolve; });
    let unavailableWaitSignal = null as AbortSignal | null;
    test.request.waitForObservation.mockImplementationOnce(async () => {
      test.observe({ semanticStatus: 'unavailable' });
    }).mockImplementationOnce((signal) => {
      unavailableWaitSignal = signal ?? null;
      enterUnavailableWait();
      return new Promise<void>(() => {});
    });
    const close = closeScenario(test.request);
    const outcome = close.then(() => null, (error: unknown) => error as Error);
    await unavailableWaitEntered;
    await vi.advanceTimersByTimeAsync(2_000);
    const failure = await outcome;
    expect(failure?.message).toContain('Control source is unavailable; Close is unresolved.');
    expect(failure?.message).toContain('Nothing was retried.');
    expect(unavailableWaitSignal?.aborted).toBe(true);
    expect(test.abort.signal.aborted).toBe(false);
    expect(test.reset).toHaveBeenCalledTimes(1);
    expect(test.pendingRead).toHaveBeenCalledTimes(2);
    expect(test.request.waitForObservation).toHaveBeenCalledTimes(2);
    expect(test.strictReadsDuringNonCurrentObservation).toBe(0);
    expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
    expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
    expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
    expect(apiMock.scene.commit).not.toHaveBeenCalled();
  } finally {
    vi.useRealTimers();
  }
});

it('caller AbortSignal ends a hanging unavailable observation wait immediately', async () => {
  const test = pendingResetObservationFixture();
  let enterUnavailableWait!: () => void;
  const unavailableWaitEntered = new Promise<void>((resolve) => { enterUnavailableWait = resolve; });
  let unavailableWaitSignal = null as AbortSignal | null;
  test.request.waitForObservation.mockImplementationOnce(async () => {
    test.observe({ semanticStatus: 'unavailable' });
  }).mockImplementationOnce((signal) => {
    unavailableWaitSignal = signal ?? null;
    enterUnavailableWait();
    return new Promise<void>(() => {});
  });
  const close = closeScenario(test.request);
  await unavailableWaitEntered;
  test.abort.abort(new Error('Close caller unmounted'));
  await expect(close).rejects.toThrow('Close caller unmounted');
  expect(unavailableWaitSignal?.aborted).toBe(true);
  expect(test.reset).toHaveBeenCalledTimes(1);
  expect(test.pendingRead).toHaveBeenCalledTimes(2);
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
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
  expect(test.strictReadsDuringNonCurrentObservation).toBe(0);
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
});

it.each(['missing_snapshot', 'disconnected'] as const)(
  'accepted Reset wait followed by %s control is fatal without a retry or Close', async (condition) => {
    const test = pendingResetObservationFixture();
    test.request.waitForObservation.mockImplementationOnce(async () => {
      test.observe(condition === 'missing_snapshot' ? { snapshot: null } : { transportStatus: 'disconnected' });
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
  expect(test.strictReadsDuringNonCurrentObservation).toBe(0);
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(apiMock.scene.commit).not.toHaveBeenCalled();
});

it.each(['refused', 'unconfirmed'] as const)(
  'a %s Reset never enters the pending observation reader', async (status) => {
    const test = pendingResetObservationFixture();
    test.reset.mockImplementationOnce(async (options) => status === 'refused'
      ? { status, message: 'Reset refused' }
      : { status, message: 'Reset delivery unknown', reset: { requestId: 'reset.1', correlationId: options.correlationId,
        programRunnerOwnerEpoch: test.current.programCycle.programRunnerOwnerEpoch, ...options.expected } });
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

// Item 37: the Close refusals the GUI raises itself carry typed codes, and
// Force close ends each stuck Close case after a confirmed Full Stop.
const uncertainOutput = [{ operationId: 'op.io', operationRevision: 2, resident: true,
  outputEvidence: 'acceptance_unknown', controllableComponentId: 'io', commandTargetId: 'valve' }];
function stuckFixture(manualOperations: unknown[] = [], carried: string[] = []) {
  const test = standaloneFixture('idle_after_reset', carried);
  test.change({ ...test.current, executionControlStatus: { manualOperations,
    cellStopAction: { fault: null, expectedStopGeneration: 4 } } } as unknown as ComposerHeadlessState);
  apiMock.execution.forceClose.mockImplementation(async (request: { step: string }) => (
    { step: request.step, stopGeneration: 5, motionBlockCleared: request.step !== 'stop', operations: request.step === 'stop' ? [] : [{ operationId: 'op.io',
      operationRevision: 2, state: 'failed', containmentUncertain: false,
      targets: [{ controllableComponentId: 'io', commandTargetId: 'valve', outputUnknown: true }] }] }));
  return test;
}
const refusal = (code: string, message: string) => Object.assign(new Error(message),
  { structuredError: { code, message, details: null, origin: null } });

it('U1: an uncertain output refuses Close with the typed code before any effect', async () => {
  const test = stuckFixture(uncertainOutput);
  const caught = await closeScenario(test.request).then(() => null, (error: unknown) => error);
  expect(closeErrorCode(caught)).toBe('scenario_close_output_uncertain');
  expect(errorMessage(caught, '')).toContain('use Force close');
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(test.reset).not.toHaveBeenCalled();
});

it('U1 on a latched cell keeps the typed code', async () => {
  const test = latchedFixture();
  test.change({ ...test.current, executionControlStatus: { ...test.current.executionControlStatus!,
    manualOperations: uncertainOutput } } as unknown as ComposerHeadlessState);
  const caught = await closeScenario(test.request).then(() => null, (error: unknown) => error);
  expect(closeErrorCode(caught)).toBe('scenario_close_output_uncertain');
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
});

it('U4: a Program that cannot be Reset refuses Close with the typed code', async () => {
  const test = standaloneFixture('recovery_required');
  const caught = await closeScenario(test.request).then(() => null, (error: unknown) => error);
  expect(closeErrorCode(caught)).toBe('scenario_close_program_not_reset');
  expect(errorMessage(caught, '')).toContain('Stop motion before closing the Scenario');
  expect(test.reset).not.toHaveBeenCalled();
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
});

it.each([
  ['U1 uncertain output', 'scenario_close_output_uncertain'],
  ['U2 uncertain containment', 'cell_close_containment_uncertain'],
  ['U5/U6 resident operation refusing Disable', 'execution_rejected'],
] as const)('%s: Close is refused with a typed code, then Force close completes', async (_case, code) => {
  const test = stuckFixture(code === 'scenario_close_output_uncertain' ? uncertainOutput : []);
  if (code === 'cell_close_containment_uncertain') {
    apiMock.cclc.retireTargets.mockRejectedValueOnce(refusal(code, 'Close cannot infer rest from an uncertain terminal'));
  }
  if (code === 'execution_rejected') {
    apiMock.cclc.submitLifecycle.mockRejectedValue(refusal(code, 'lifecycle mutation requires settled physical and Program work'));
  }
  const caught = await closeScenario(test.request).then(() => null, (error: unknown) => error);
  expect(closeErrorCode(caught)).toBe(code);
  expect(apiMock.manager.applyCandidate).not.toHaveBeenCalled();
  expect(apiMock.execution.forceClose).not.toHaveBeenCalled();

  vi.mocked(apiMock.cclc.retireTargets).mockClear();
  vi.mocked(apiMock.cclc.stopComponentProcess).mockClear();
  apiMock.manager.status.mockResolvedValueOnce({ source, status: { managerReady: true, active } });
  expect(await forceCloseScenario(test.request)).toEqual(empty);
  const steps = apiMock.execution.forceClose.mock.calls.map(([request]) => request);
  expect(steps.map((request) => request.step)).toEqual(['stop', 'close_operations', 'end']);
  expect(steps[0]).toMatchObject({ expected_stop_generation: 4,
    request_context: { request_owner_epoch: 'kernel.epoch', source_label: 'composer_ui' } });
  // Full Stop, retirement, Kernel step, process Stop, then the empty Apply.
  const order = [apiMock.execution.forceClose.mock.invocationCallOrder[0], apiMock.cclc.retireTargets.mock.invocationCallOrder[0],
    apiMock.execution.forceClose.mock.invocationCallOrder[1], apiMock.cclc.stopComponentProcess.mock.invocationCallOrder[0],
    apiMock.manager.prepareCanonicalEmpty.mock.invocationCallOrder[0]];
  expect(order).toEqual([...order].sort((a, b) => a - b));
  expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
  expect(test.request.progress).toHaveBeenCalledWith(expect.stringContaining('Motion block cleared'));
  // A refused Disable leaves its target to a second retirement before Apply.
  expect(apiMock.cclc.retireTargets).toHaveBeenCalledTimes(code === 'execution_rejected' ? 2 : 1);
});

it('Force close refuses with a typed reason when the Full Stop is not confirmed, before any retirement', async () => {
  const test = stuckFixture(uncertainOutput);
  apiMock.execution.forceClose.mockRejectedValue(Object.assign(new Error('Force close requires the operator\'s confirmed Full Stop'),
    { structuredError: { code: 'scenario_force_close_stop_unconfirmed', message: 'unconfirmed',
      details: { reason: 'stop_not_contained' }, origin: null } }));
  const caught = await forceCloseScenario(test.request).then(() => null, (error: unknown) => error);
  expect(closeErrorCode(caught)).toBe('scenario_force_close_stop_unconfirmed');
  expect(errorMessage(caught, '')).toContain('Force close stopped before any retirement');
  expect(apiMock.execution.forceClose).toHaveBeenCalledTimes(1);
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
  expect(apiMock.cclc.stopComponentProcess).not.toHaveBeenCalled();
  expect(apiMock.manager.applyCandidate).not.toHaveBeenCalled();
});

it('Force close stops at an unknown process Stop, after its Kernel step and before the empty Apply', async () => {
  const test = stuckFixture(uncertainOutput);
  apiMock.cclc.stopComponentProcess.mockResolvedValue({ state: 'unknown', error: { message: 'Old host drain unknown' }, receipt: null });
  const caught = await forceCloseScenario(test.request).then(() => null, (error: unknown) => error);
  expect(errorMessage(caught, '')).toContain('Old host drain unknown');
  expect(errorMessage(caught, '')).toContain('Lifecycle targets retired; finish with New or Load.');
  expect(apiMock.execution.forceClose.mock.calls.map(([request]) => request.step)).toEqual(['stop', 'close_operations', 'end']);
  expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
  expect(apiMock.manager.applyCandidate).not.toHaveBeenCalled();
});

it('B1: Force close detaches a carried part at its current pose before any process Stop', async () => {
  const test = stuckFixture(uncertainOutput, ['held.1']);
  test.request.waitForObservation.mockImplementationOnce(async () => {
    // The detach is confirmed while every component process still runs.
    expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
    expect(apiMock.cclc.stopComponentProcess).not.toHaveBeenCalled();
    test.publishScene(2, []);
  });
  expect(await forceCloseScenario(test.request)).toEqual(empty);
  expect(apiMock.execution.forceClose.mock.calls.map(([request]) => request.step)).toEqual(['stop', 'close_operations', 'end']);
  expect(apiMock.scene.commit).toHaveBeenCalledExactlyOnceWith(expect.objectContaining({
    expected_authoring_workspace: test.workspace,
    operation: expect.objectContaining({ kind: 'detach_carried_object_at_current_pose',
      carried_object_attachment_id: 'attachment.held.1', expected_scene_resource_id: 'held.1' }),
  }), test.request.signal);
  // Full Stop, retirement, Kernel step, detach, process Stop, then the empty Apply.
  const order = [apiMock.execution.forceClose.mock.invocationCallOrder[0], apiMock.cclc.retireTargets.mock.invocationCallOrder[0],
    apiMock.execution.forceClose.mock.invocationCallOrder[1], apiMock.scene.commit.mock.invocationCallOrder[0],
    apiMock.cclc.submitLifecycle.mock.invocationCallOrder[0], apiMock.cclc.stopComponentProcess.mock.invocationCallOrder[0],
    apiMock.manager.prepareCanonicalEmpty.mock.invocationCallOrder[0]];
  expect(order).toEqual([...order].sort((a, b) => a - b));
  expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
});

it('F1-FC: Force close uses the current Scene owner after a restart between detaches', async () => {
  const test = stuckFixture(uncertainOutput, ['held.1', 'held.2']);
  test.request.waitForObservation.mockImplementationOnce(async () => {
    expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
    test.publishScene(2, ['held.2']);
    test.change({ ...test.current, runtime: { ...test.current.runtime, sections: { ...test.current.runtime.sections,
      logicalScene: { ...test.current.runtime.sections.logicalScene, sourceOwnerId: 'scene.owner.2',
        sourceOwnerEpoch: 'scene.epoch.2' },
    } } } as ComposerHeadlessState);
  }).mockImplementationOnce(async () => test.publishScene(3, []));
  expect(await forceCloseScenario(test.request)).toEqual(empty);
  expect(apiMock.scene.commit.mock.calls.map(([transaction]) => transaction.request_context.request_owner_epoch))
    .toEqual(['scene.epoch', 'scene.epoch.2']);
  expect(apiMock.scene.commit.mock.calls.map(([transaction]) => transaction.operation))
    .toEqual([
      expect.objectContaining({ kind: 'detach_carried_object_at_current_pose', expected_scene_resource_id: 'held.1' }),
      expect.objectContaining({ kind: 'detach_carried_object_at_current_pose', expected_scene_resource_id: 'held.2' }),
    ]);
  expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
});

// Item 38 (b): the robot's process is already dead, so the Scene owner refuses
// the detach (no live carrier pose). Force close keeps the part attached and
// the server's empty preparation (not a client flag) skips the attachment
// check; ordinary Close does not.
const carrierGone = () => Object.assign(new Error('current carried-object pose has no exact carrier-frame resolution'), {
  payload: { ok: false, error: { code: 'missing_reference', details: { reason: 'kinematic_state_source_unavailable' } } },
  structuredError: { code: 'missing_reference', message: 'no carrier frame',
    details: { reason: 'kinematic_state_source_unavailable' }, origin: null } });

it('item 38: with the robot process dead and a part attached, Force close completes with the part kept', async () => {
  const test = stuckFixture(uncertainOutput, ['held.1']);
  apiMock.scene.commit.mockRejectedValue(carrierGone());
  expect(await forceCloseScenario(test.request)).toEqual(empty);
  expect(apiMock.scene.commit).toHaveBeenCalledTimes(1);
  expect(test.request.progress).toHaveBeenCalledWith(expect.stringContaining('held.1 stays attached'));
  expect(apiMock.manager.prepareCanonicalEmpty).toHaveBeenCalledTimes(1);
  expect(apiMock.manager.prepareCanonicalEmpty.mock.calls[0][0]).not.toHaveProperty('force_close');
  expect(apiMock.cclc.stopComponentProcess).toHaveBeenCalled();
  expect(apiMock.manager.applyCandidate).toHaveBeenCalledTimes(1);
});

it('item 38: ordinary Close keeps refusing when the detach is refused', async () => {
  const test = standaloneFixture('idle_after_reset', ['held.1']);
  apiMock.scene.commit.mockRejectedValue(carrierGone());
  await expect(closeScenario(test.request)).rejects.toThrow('Detach of held.1 was refused or is unconfirmed');
  expect(apiMock.cclc.retireTargets).not.toHaveBeenCalled();
  expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
});

it('item 38: a transport failure of the detach still stops Force close', async () => {
  const test = stuckFixture(uncertainOutput, ['held.1']);
  apiMock.scene.commit.mockRejectedValue(new Error('network down'));
  const caught = await forceCloseScenario(test.request).then(() => null, (error: unknown) => error);
  expect(errorMessage(caught, '')).toContain('network down');
  expect(apiMock.cclc.stopComponentProcess).not.toHaveBeenCalled();
  expect(apiMock.manager.prepareCanonicalEmpty).not.toHaveBeenCalled();
  // SX-616: the attempt ends on the server too, so no bypass outlives it.
  expect(apiMock.execution.forceClose.mock.calls.map(([request]) => request.step)).toEqual(['stop', 'close_operations', 'end']);
});

it('SX-616: a failed empty preparation still ends the Force close attempt', async () => {
  const test = stuckFixture(uncertainOutput, ['held.1']);
  apiMock.scene.commit.mockRejectedValue(carrierGone());
  apiMock.manager.prepareCanonicalEmpty.mockRejectedValue(new Error('preparation failed'));
  const caught = await forceCloseScenario(test.request).then(() => null, (error: unknown) => error);
  expect(errorMessage(caught, '')).toContain('preparation failed');
  expect(apiMock.manager.applyCandidate).not.toHaveBeenCalled();
  expect(apiMock.execution.forceClose.mock.calls.map(([request]) => request.step)).toEqual(['stop', 'close_operations', 'end']);
  const end = apiMock.execution.forceClose.mock.calls[2][0];
  expect(end).not.toHaveProperty('expected_stop_generation');
});

it('SX-616: a lost end reply does not change the Force close outcome', async () => {
  const test = stuckFixture(uncertainOutput);
  apiMock.execution.forceClose.mockImplementation(async (request: { step: string }) => {
    if (request.step === 'end') throw new Error('end lost');
    return { step: request.step, stopGeneration: 5, motionBlockCleared: false, operations: [] };
  });
  expect(await forceCloseScenario(test.request)).toEqual(empty);
});
