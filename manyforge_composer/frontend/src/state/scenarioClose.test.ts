import { beforeEach, expect, it, vi } from 'vitest';
import type { ManagerActiveProjection } from '../types/api';

const apiMock = vi.hoisted(() => ({ manager: { status: vi.fn(), canonicalEmptySelection: vi.fn(),
  prepareCanonicalEmpty: vi.fn(), previewCandidate: vi.fn(), applyCandidate: vi.fn(), applyStatus: vi.fn() },
  execution: { identity: vi.fn() }, cclc: { targetRetirementOffer: vi.fn(), retireTargets: vi.fn(),
    ownerSettlement: vi.fn(), owner: vi.fn(), component: vi.fn(), submitLifecycle: vi.fn(), lifecycleStatus: vi.fn(),
    componentProcessStopOffer: vi.fn(), stopComponentProcess: vi.fn(), componentProcessStopResult: vi.fn() } }));
vi.mock('../api/client', () => ({ api: apiMock }));
import { closeScenario } from './scenarioClose';

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
const source = { available: true, freshness: 'fresh', sourceOwnerEpoch: 'owner', sourceOwnerId: 'manager.cell' };
const projection = (value: ManagerActiveProjection, metadata = source) => ({ source: metadata, status: { managerReady: true, active: value } });
const scope = { request_owner_epoch: 'owner', publication_generation: 2, active_public_pointer_digest: 'pointer' };
const component = { componentId: 'arm', retainedBundleId: active.retainedBundleId, graphRevision: active.graphRevision,
  publicationGeneration: 2, lifecycleGeneration: 3, lifecycleState: 'active', readiness: { requestOwnerEpoch: 'owner' },
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
  apiMock.cclc.owner.mockResolvedValue({ requestOwnerEpoch: 'owner' });
  apiMock.execution.identity.mockResolvedValue({ retainedBundleId: active.retainedBundleId, graphRevision: active.graphRevision, activePublicationGeneration: 2 });
  apiMock.cclc.component.mockResolvedValue(component);
  apiMock.cclc.submitLifecycle.mockResolvedValue({ transactionId: 'disable.1', status: { terminalResult: 'succeeded', reconciliationRequired: false } });
  apiMock.cclc.componentProcessStopOffer.mockResolvedValue({ controllable_component_id: 'arm', process_inventories: [{ request: processScope,
    process_occurrences: [{ controllable_component_id: 'arm', launch_identity: 'actual-launch', leader_pid: 42,
      process_presence: 'present', cleanup_state: 'not_requested', receipt: null, installed_runtime_identity: null }], error: null }] });
  apiMock.cclc.stopComponentProcess.mockResolvedValue({ state: 'pending', receipt: null, error: null });
  apiMock.cclc.componentProcessStopResult.mockResolvedValue({ state: 'drained', receipt: { leader_pid: 42 }, error: null });
  apiMock.manager.canonicalEmptySelection.mockResolvedValue({ request_owner_epoch: 'owner', cell_id: 'actual-cell',
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
  expect(apiMock.manager.applyCandidate.mock.calls[0][0]).toMatchObject({ prepared_activation: prepared,
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
