import { beforeEach, expect, it, vi } from 'vitest';
import type { ManagerActiveProjection } from '../types/api';

const apiMock = vi.hoisted(() => ({ manager: { status: vi.fn(), canonicalEmptySelection: vi.fn(),
  prepareCanonicalEmpty: vi.fn(), previewCandidate: vi.fn(), applyCandidate: vi.fn(), applyStatus: vi.fn() },
  execution: { identity: vi.fn() }, cclc: { targetRetirementOffer: vi.fn(), retireTargets: vi.fn(),
    ownerSettlement: vi.fn(), owner: vi.fn(), component: vi.fn(), submitLifecycle: vi.fn(), lifecycleStatus: vi.fn(),
    componentProcessStopOffer: vi.fn(), stopComponentProcess: vi.fn(), componentProcessStopResult: vi.fn() } }));
vi.mock('../api/client', () => ({ api: apiMock }));
import { closeScenario } from './scenarioClose';

const active: ManagerActiveProjection = { canonicalEmpty: false, retainedBundleId: 'bundle.current',
  graphRevision: 'graph.current', publication: {}, cellGraph: { controllable_components: [{ controllable_component_id: 'arm' }] } };
const empty: ManagerActiveProjection = { ...active, canonicalEmpty: true, retainedBundleId: 'bundle.empty', graphRevision: 'graph.empty' };
const source = { available: true, freshness: 'fresh' };
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
  apiMock.manager.status.mockReset().mockResolvedValue({ source, status: { managerReady: true, active } });
  await expect(closeScenario(inputs())).rejects.toThrow('current publication cannot be verified');
});
it('Declared components do not receive an invented lifecycle transition', async () => {
  apiMock.cclc.component.mockResolvedValue({ ...component, lifecycleState: 'declared' });
  await closeScenario(inputs());
  expect(apiMock.cclc.submitLifecycle).not.toHaveBeenCalled();
});
