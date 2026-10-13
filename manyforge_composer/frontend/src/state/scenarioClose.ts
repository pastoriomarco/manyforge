import { api } from '../api/client';
import { awaitActivationCompletion, browserRequestIdentity, cclcRequestOwnerEpoch, lifecycleRequestFromSnapshot, requestContext } from '../hooks/useCCLC';
import type { ManagerActiveProjection } from '../types/api';
import type { ManagerApplyIdentity } from './managerApply';

interface CloseInputs {
  expected: ManagerActiveProjection;
  signal: AbortSignal;
  admissionGeneration: () => number;
  waitForObservation: () => Promise<void>;
  trackApply: (identity: ManagerApplyIdentity) => void;
  progress: (message: string) => void;
}
const identity = () => browserRequestIdentity('composer-close');

/** GUI composition only. Every effect uses the existing owner command and refusal. */
export async function closeScenario({ expected, signal, admissionGeneration,
  waitForObservation, trackApply, progress }: CloseInputs): Promise<ManagerActiveProjection> {
  const current = await api.manager.status(signal);
  if (!current.source.available || current.source.freshness !== 'fresh'
    || !current.status.managerReady || current.status.active.retainedBundleId !== expected.retainedBundleId
    || current.status.active.graphRevision !== expected.graphRevision) {
    throw new Error('Current Scenario changed; Close was not started.');
  }
  progress('Retiring current lifecycle targets…');
  const offer = await api.cclc.targetRetirementOffer(signal);
  const context = { request_id: identity(), request_owner_epoch: offer.scope.request_owner_epoch,
    source_label: 'composer_ui', correlation_id: identity() };
  await api.cclc.retireTargets({ request_context: context, scope: offer.scope, expected_targets: offer.targets }, signal);
  await api.cclc.ownerSettlement(offer.scope, signal);
  signal.throwIfAborted();
  const graphRows = expected.cellGraph.controllable_components;
  if (!Array.isArray(graphRows)) throw new Error('Current component inventory is unavailable.');
  const ids = [...new Set([...offer.targets.map((row) => row.controllable_component_id),
    ...graphRows.map((row) => {
      if (row === null || typeof row !== 'object' || Array.isArray(row)
        || typeof row.controllable_component_id !== 'string') throw new Error('Current component inventory is invalid.');
      return row.controllable_component_id;
    })])];
  for (const id of ids) {
    signal.throwIfAborted();
    progress(`Disabling and stopping ${id}…`);
    const [owner, execution, snapshot] = await Promise.all([
      api.cclc.owner(signal), api.execution.identity(signal), api.cclc.component(id, signal),
    ]);
    const ownerEpoch = cclcRequestOwnerEpoch(owner, snapshot, execution);
    if (snapshot.retainedBundleId !== expected.retainedBundleId || snapshot.graphRevision !== expected.graphRevision)
      throw new Error('Applied configuration changed during Close.');
    const incident = snapshot.lifecycleProgress?.incident;
    if (incident != null) {
      const request = lifecycleRequestFromSnapshot(snapshot, 'recover', requestContext(ownerEpoch, `close:${id}:recover`), admissionGeneration());
      request.reason = 'cell closed by operator';
      request.close_incident_recovery = { expected_incident_id: incident.incidentId };
      const result = await api.cclc.submitLifecycle(request, signal);
      if (result.incidentRecovery?.closureReason !== 'cell_closed_by_operator') throw new Error('Component incident Close is unresolved.');
    }
    const latest = await api.cclc.component(id, signal);
    if (latest.lifecycleState === 'active' || latest.lifecycleState === 'prepared') {
      const result = await api.cclc.submitLifecycle(lifecycleRequestFromSnapshot(latest, 'disable',
        requestContext(ownerEpoch, `close:${id}:disable`), admissionGeneration()), signal);
      if (result.transactionId === null) throw new Error(`Disable of ${id} was not accepted.`);
      await awaitActivationCompletion(result.status,
        () => api.cclc.lifecycleStatus(result.transactionId!, signal), waitForObservation);
    } else if (latest.lifecycleState !== 'disabled' && latest.lifecycleState !== 'declared') {
      throw new Error(`Component ${id} has an unresolved lifecycle state.`);
    }
    const processes = await api.cclc.componentProcessStopOffer(id);
    for (const inventory of processes.process_inventories) {
      for (const occurrence of inventory.process_occurrences) {
        if (occurrence.cleanup_state === 'drained') continue;
        const request = { ...inventory.request, request_context: {
          request_id: identity(), request_owner_epoch: offer.scope.request_owner_epoch, source_label: 'composer_ui',
        }, controllable_component_id: id, launch_identity: occurrence.launch_identity, leader_pid: occurrence.leader_pid };
        let result = await api.cclc.stopComponentProcess(request);
        while (result.state === 'pending') {
          await waitForObservation();
          signal.throwIfAborted();
          result = await api.cclc.componentProcessStopResult(request);
        }
        if (result.state !== 'drained') throw new Error(result.error?.message ?? `Process Stop of ${id} is unknown. Restart the workbench.`);
      }
    }
  }
  signal.throwIfAborted();
  progress('Preparing the empty configuration…');
  const selection = await api.manager.canonicalEmptySelection(signal);
  const prepared = await api.manager.prepareCanonicalEmpty({ request_owner_epoch: selection.request_owner_epoch,
    expected_admission_generation: admissionGeneration(), origin: {
      kind: 'sealed_canonical_empty', cell_id: selection.cell_id,
      deployment_artifact_id: selection.deployment_artifact_id, deployment_revision: selection.deployment_revision,
      retained_bundle_id: selection.candidate_retained_bundle_id, graph_revision: selection.candidate_graph_revision,
      admission_digest: selection.admission_digest,
    } });
  const expectedCurrent = { retained_bundle_id: expected.retainedBundleId, graph_revision: expected.graphRevision };
  const preview = await api.manager.previewCandidate({ deployment_artifact_id: prepared.deployment_artifact_id,
    deployment_revision: prepared.deployment_revision, prepared_activation_id: prepared.prepared_activation_id,
    candidate_retained_bundle_id: prepared.retained_bundle_id, candidate_graph_revision: prepared.graph_revision,
    expected_current: expectedCurrent, expected_admission_generation: admissionGeneration(), request_owner_epoch: selection.request_owner_epoch });
  signal.throwIfAborted();
  progress('Applying the empty configuration…');
  const accepted = await api.manager.applyCandidate({ request_id: identity(), request_owner_epoch: selection.request_owner_epoch,
    source_label: 'composer_ui', correlation_id: context.correlation_id,
    deployment_artifact_id: prepared.deployment_artifact_id, deployment_revision: prepared.deployment_revision,
    expected_current: expectedCurrent, expected_admission_generation: admissionGeneration(), reject_if_affected_busy: true,
    prepared_activation: prepared, candidate_receipt: preview.candidateReceipt, accepted_impact_digest: preview.candidateReceipt.impact_digest });
  const apply = { applyId: accepted.applyId, applyGeneration: accepted.applyGeneration };
  trackApply(apply);
  let status = await api.manager.applyStatus(apply.applyId, apply.applyGeneration, signal);
  while (status.state === 'pending') {
    await waitForObservation();
    status = await api.manager.applyStatus(apply.applyId, apply.applyGeneration, signal);
  }
  if (status.state !== 'succeeded') throw new Error(status.failure?.message ?? 'Empty Apply failed or is unknown; the Scenario was not replaced.');
  const final = await api.manager.status(signal);
  if (!final.source.available || final.source.freshness !== 'fresh' || !final.status.active.canonicalEmpty
    || final.status.active.retainedBundleId !== prepared.retained_bundle_id
    || final.status.active.graphRevision !== prepared.graph_revision) {
    throw new Error('Empty Apply completed, but its current publication cannot be verified.');
  }
  progress('Scenario closed.');
  return final.status.active;
}
