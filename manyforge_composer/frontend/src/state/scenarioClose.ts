import { parseLogicalSceneProjection } from '../api/parsers';
import { sameAuthoringWorkspaceIdentity, type HeadlessStateResult } from '../hooks/useHeadlessState';
import type { ScenarioReplacementIdentity } from '../hooks/useProgramWorkspace';
import type { ProgramResetExpectation, ProgramResetOutcome } from '../hooks/useProgramRuntime';
import { scenarioCloseUncertainOutputRefusal, uncertainOutputs } from '../utils/uncertainOutputs';
import type { ComposerHeadlessState } from '../types/headless';
import { api } from '../api/client';
import { awaitActivationCompletion, browserRequestIdentity, cclcRequestOwnerEpoch, lifecycleRequestFromSnapshot, requestContext } from '../hooks/useCCLC';
import type { JsonValue, ManagerActiveProjection, ProjectionRevision } from '../types/api';
import type { ComponentProcessStopResult } from '../types/cclc';
import type { ManagerApplyIdentity } from './managerApply';

interface CloseInputs {
  expected: ManagerActiveProjection;
  signal: AbortSignal;
  admissionGeneration: () => number;
  waitForObservation: () => Promise<void>;
  trackApply: (identity: ManagerApplyIdentity) => void;
  progress: (message: string) => void;
  /** Every confirmed Close prepares the live runtime before unloading. */
  preparation?: ClosePreparationInputs;
}
export interface ClosePreparationInputs {
  confirmed: ScenarioReplacementIdentity;
  readSnapshot: () => ComposerHeadlessState;
  /** Pending accepted Reset only: null means connected observation synchronization. */
  readResetSnapshot?: () => ComposerHeadlessState | null;
  reset: (options: { correlationId: string; expected: ProgramResetExpectation }) => Promise<ProgramResetOutcome>;
}
const identity = () => browserRequestIdentity('composer-close');
/** After the backend's 750 ms response wait elapsed, only these unknowns mean the exact
 * reply of the SAME Stop may still arrive; every other unknown is terminal. */
const awaitingExactReply = (result: ComponentProcessStopResult) => result.state === 'unknown'
  && (result.error?.code === 'component_process_stop_response_unavailable'
    || result.error?.code === 'component_process_stop_pending');

/** Synchronization after accepted Reset is a wait; it grants no command freshness. */
export function resetCloseSnapshot(current: Pick<HeadlessStateResult, 'snapshot' | 'transportStatus' | 'semanticStatus'>)
  : ComposerHeadlessState | null {
  if (current.snapshot !== null && current.transportStatus === 'connected' && current.semanticStatus === 'synchronizing')
    return null;
  if (current.snapshot === null || current.transportStatus !== 'connected' || current.semanticStatus !== 'current')
    throw new Error('Control source is unavailable; Close is unresolved.');
  return current.snapshot;
}

const object = (value: JsonValue | undefined): value is Record<string, JsonValue> =>
  value !== null && typeof value === 'object' && !Array.isArray(value);
const positiveGeneration = (value: JsonValue | undefined): boolean =>
  typeof value === 'number' && Number.isSafeInteger(value) && value > 0;
const nonemptyString = (value: JsonValue | undefined): boolean =>
  typeof value === 'string' && value.length > 0;

function validPublication(active: ManagerActiveProjection): boolean {
  const publication = active.publication;
  const revision = publication.projection_revision;
  const transaction = publication.transaction;
  if (!positiveGeneration(publication.publication_generation)
    || !nonemptyString(publication.durable_public_pointer_content_digest)
    || !object(revision) || revision.retained_bundle_id !== active.retainedBundleId
    || revision.graph_revision !== active.graphRevision || !object(revision.scene_revision)
    || !nonemptyString(revision.scene_revision.scene_id)
    || !positiveGeneration(revision.scene_revision.revision_id)
    || !nonemptyString(revision.scene_revision.content_hash) || !object(transaction)) return false;
  return transaction.kind === 'named_apply'
    ? nonemptyString(transaction.apply_id) && positiveGeneration(transaction.apply_generation)
    : transaction.kind === 'bootstrap'
      && nonemptyString(transaction.bootstrap_id) && positiveGeneration(transaction.bootstrap_generation);
}

function sameJson(left: JsonValue, right: JsonValue): boolean {
  if (left === right) return true;
  if (Array.isArray(left) && Array.isArray(right))
    return left.length === right.length && left.every((value, index) => sameJson(value, right[index]));
  if (!object(left) || !object(right)) return false;
  const keys = Object.keys(left);
  return keys.length === Object.keys(right).length
    && keys.every((key) => Object.hasOwn(right, key) && sameJson(left[key], right[key]));
}

/** Compare accepted control facts with the exact HTTP Close publication. */
export function managerPublicationMatches(value: unknown, expected: ManagerActiveProjection): boolean {
  if (!object(value as JsonValue) || !validPublication(expected)) return false;
  const active = value as Record<string, JsonValue>;
  return active.canonical_empty === expected.canonicalEmpty
    && active.retained_bundle_id === expected.retainedBundleId
    && active.graph_revision === expected.graphRevision
    && object(active.publication) && sameJson(active.publication, expected.publication);
}

/** Fresh-start policy content, with its owner-issued revision left intact. */
export function freshClosePolicy(policy: Record<string, JsonValue>): boolean {
  return policy.policy_id === 'free_space' && policy.require_full_diagnostics === true
    && ['allowed_static_static_pairs', 'allowed_touch_link_pairs', 'support_contacts', 'contact_grasps']
      .every((field) => Array.isArray(policy[field]) && policy[field].length === 0);
}
const sameProjection = (left: ProjectionRevision, right: ProjectionRevision) =>
  sameJson(left as unknown as JsonValue, right as unknown as JsonValue);

function currentCloseSnapshot(expected: ManagerActiveProjection, preparation: ClosePreparationInputs, signal: AbortSignal): ComposerHeadlessState {
  signal.throwIfAborted();
  const current = preparation.readSnapshot();
  const manager = current.runtime.sections.manager;
  if (current.receiverEpoch !== preparation.confirmed.receiverEpoch
    || !sameAuthoringWorkspaceIdentity(current.authoringWorkspace, preparation.confirmed.authoring)
    || current.authoringWorkspaceContainment !== null
    || (preparation.confirmed.closeScene !== undefined
    && (current.runtime.sections.logicalScene?.sourceOwnerId !== preparation.confirmed.closeScene.sourceOwnerId
      || current.runtime.sections.logicalScene?.sourceOwnerEpoch !== preparation.confirmed.closeScene.sourceOwnerEpoch))
    || manager.sourceOwnerId !== preparation.confirmed.managerOwnerId
    || manager.sourceOwnerEpoch !== preparation.confirmed.managerOwnerEpoch
    || !manager.available || manager.freshness !== 'fresh' || manager.knownGaps.length > 0
    || !managerPublicationMatches(manager.payload?.active, expected))
    throw new Error('The confirmed workspace, owner or Applied publication changed.');
  return current;
}

/** Close always uses ordinary Reset with no optional restoration selected. */
async function resetProgramForClose(input: ClosePreparationInputs, expected: ProgramResetExpectation,
  correlationId: string): Promise<ProgramResetOutcome> {
  return input.reset({ expected, correlationId });
}

/** The same ordinary Reset-before-Close phase for every confirmed replacement. */
export async function prepareProgramCycleForClose(
  { expected, signal, waitForObservation, progress }: CloseInputs, preparation: ClosePreparationInputs,
): Promise<{ resetDone: boolean; idleCycle: ProgramResetExpectation }> {
  let resetDone = false;
  let resetSubmitted = false;
  let idleCycle: ProgramResetExpectation | null = null;
  const read = () => currentCloseSnapshot(expected, preparation, signal);
  try {
    const initial = read();
    // Close never acknowledges outputs with unknown acceptance (contract 66).
    const uncertain = scenarioCloseUncertainOutputRefusal(
      uncertainOutputs(initial.executionControlStatus?.manualOperations),
    );
    if (uncertain !== null) throw new Error(uncertain);
    const cycle = initial.programCycle;
    const confirmedCycle = preparation.confirmed.closeCycle;
    const confirmedScene = preparation.confirmed.closeScene;
    if (confirmedCycle === undefined || confirmedScene === undefined
      || cycle.state !== confirmedCycle.state || cycle.programRunId !== confirmedCycle.programRunId
      || cycle.programRunnerOwnerEpoch !== confirmedCycle.programRunnerOwnerEpoch
      || cycle.cycleGeneration !== confirmedCycle.cycleGeneration
      || cycle.cycleClaimGeneration !== confirmedCycle.cycleClaimGeneration
      || cycle.terminalOrSettlementReference !== confirmedCycle.terminalOrSettlementReference
      || initial.runtime.sections.logicalScene?.sourceOwnerId !== confirmedScene.sourceOwnerId
      || initial.runtime.sections.logicalScene?.sourceOwnerEpoch !== confirmedScene.sourceOwnerEpoch
      || !sameJson(initial.runtime.sections.logicalScene?.payload as JsonValue, confirmedScene.payload as JsonValue))
      throw new Error('The confirmed Program cycle or logical Scene changed.');
    const run = cycle.run;
    const controls = cycle.runtimeControls;
    const heldResetAllowed = cycle.state === 'running' && run !== null && controls !== null
      && run.ownerEpoch === cycle.programRunnerOwnerEpoch && run.capture.programRunId === cycle.programRunId
      && run.runRevision === cycle.runRevision
      && controls.ownerEpoch === run.ownerEpoch && controls.programRunId === run.capture.programRunId
      && controls.runRevision === run.runRevision
      && controls.runtimeControls.runtimeControlRevision === cycle.runtimeControlRevision
      && controls.executionSelections.motionHeld === true
      && cycle.allowedCommands.some((command) => command.commandKind === 'program_reset' && command.allowed);
    if ((cycle.state === 'terminal_awaiting_reset' || heldResetAllowed) && cycle.programRunId !== null) {
      progress('Resetting the Program before Close…');
      resetSubmitted = true;
      const result = await resetProgramForClose(preparation, {
        programRunId: cycle.programRunId, programRunnerOwnerEpoch: cycle.programRunnerOwnerEpoch,
        cycleGeneration: cycle.cycleGeneration,
      }, identity());
      if (result.status !== 'sent') {
        resetSubmitted = result.status === 'unconfirmed';
        throw new Error(result.message);
      }
      for (;;) {
        signal.throwIfAborted();
        const observation = preparation.readResetSnapshot === undefined
          ? preparation.readSnapshot() : preparation.readResetSnapshot();
        if (observation === null) {
          await waitForObservation();
          continue;
        }
        const current = observation.programCycle;
        if (current.programRunnerOwnerEpoch !== result.reset.programRunnerOwnerEpoch)
          throw new Error('The Program owner changed before Reset settled.');
        if (current.causalCorrelationId === result.reset.correlationId
          && current.state === 'idle_after_reset' && current.programRunId === null
          && current.cycleGeneration > result.reset.cycleGeneration) {
          resetDone = true;
        }
        read();
        if (resetDone) {
          idleCycle = { programRunId: null, programRunnerOwnerEpoch: current.programRunnerOwnerEpoch, cycleGeneration: current.cycleGeneration };
          break;
        }
        if (current.state === 'recovery_required' || (current.cycleGeneration > result.reset.cycleGeneration
          && current.causalCorrelationId !== result.reset.correlationId))
          throw new Error('Reset did not settle the confirmed Program cycle.');
        await waitForObservation();
      }
    } else if (cycle.state !== 'idle_after_reset' || cycle.programRunId !== null) {
      throw new Error('Stop motion before closing the Scenario; Reset must be available for the stopped or held Program.');
    }
    idleCycle ??= { programRunId: null, programRunnerOwnerEpoch: cycle.programRunnerOwnerEpoch, cycleGeneration: cycle.cycleGeneration };
    return { resetDone, idleCycle };
  } catch (error) {
    throw new Error(`${error instanceof Error ? error.message : 'Close preparation is unresolved.'} ${resetDone
      ? 'Reset done; Close not completed.' : resetSubmitted ? 'Close not completed; Reset was submitted; its outcome is not confirmed.' : 'Close not completed.'} Detached objects confirmed: none. Editors were kept. Nothing was retried.`);
  }
}

interface PreparedClose {
  resetDone: boolean;
  detached: string[];
  idleCycle?: ProgramResetExpectation;
  sceneProjection?: ProjectionRevision;
}

/** Existing Reset and runtime detach only; no retries or invented poses. */
export async function prepareSceneForClose(input: CloseInputs): Promise<PreparedClose> {
  const { expected, signal, waitForObservation, progress, preparation } = input;
  const detached: string[] = [];
  if (preparation === undefined) return { resetDone: false, detached };
  const { resetDone, idleCycle } = await prepareProgramCycleForClose(input, preparation);
  let attempted: string | null = null;
  const read = () => currentCloseSnapshot(expected, preparation, signal);
  const scene = () => {
    const current = read();
    if (current.programCycle.state !== 'idle_after_reset' || current.programCycle.programRunId !== null
      || current.programCycle.programRunnerOwnerEpoch !== idleCycle.programRunnerOwnerEpoch
      || current.programCycle.cycleGeneration !== idleCycle.cycleGeneration)
      throw new Error('The confirmed idle Program cycle is unavailable.');
    const section = current.runtime.sections.logicalScene;
    if (!section?.available || section.freshness !== 'fresh' || section.knownGaps.length > 0
      || section.payload === null || !section.sourceOwnerId || !section.sourceOwnerEpoch)
      throw new Error('The current logical Scene owner is unavailable.');
    const parsed = parseLogicalSceneProjection(section.payload);
    if (parsed.snapshot === null || parsed.access.active_projection === null
      || parsed.access.active_projection.retained_bundle_id !== expected.retainedBundleId
      || parsed.access.active_projection.graph_revision !== expected.graphRevision)
      throw new Error('The current logical Scene is unavailable or fenced.');
    return { current, section, access: parsed.access, projection: parsed.access.active_projection, content: parsed.snapshot };
  };
  try {
    let view = scene();
    const owner = { id: view.section.sourceOwnerId, epoch: view.section.sourceOwnerEpoch };
    const attachments = [...view.content.carried_object_attachments];
    let expectedScene = view.projection;
    for (const attachment of attachments) {
      view = scene();
      if (view.section.sourceOwnerId !== owner.id || view.section.sourceOwnerEpoch !== owner.epoch
        || !sameProjection(view.projection, expectedScene))
        throw new Error('The logical Scene changed outside this Close.');
      if (!view.access.scene_writes_open || !view.access.projection_leases_open)
        throw new Error('The logical Scene owner currently fences detachment.');
      attempted = attachment.scene_resource_id;
      progress(`Detaching ${attempted} at its current pose…`);
      const requestId = identity();
      const response = await api.scene.commit({
        request_context: { request_id: requestId, request_owner_epoch: owner.epoch!,
          source_label: 'composer_ui', correlation_id: requestId },
        expected_projection_revision: expectedScene,
        expected_authoring_workspace: preparation.confirmed.authoring,
        operation: { kind: 'detach_carried_object_at_current_pose',
          carried_object_attachment_id: attachment.carried_object_attachment_id,
          expected_carried_object_attachment_revision: attachment.carried_object_attachment_revision,
          expected_scene_resource_id: attachment.scene_resource_id,
          expected_controllable_component_id: attachment.controllable_component_id,
          expected_command_target_id: attachment.command_target_id },
      }, signal);
      const committed = response.committed_projection_revision;
      if (response.request_id !== requestId || response.request_owner_epoch !== owner.epoch
        || committed.retained_bundle_id !== expectedScene.retained_bundle_id
        || committed.graph_revision !== expectedScene.graph_revision
        || committed.scene_revision.scene_id !== expectedScene.scene_revision.scene_id
        || committed.scene_revision.revision_id !== expectedScene.scene_revision.revision_id + 1)
        throw new Error('The detach receipt is not the exact requested Scene successor.');
      detached.push(attempted); attempted = null;
      for (;;) {
        view = scene();
        if (view.section.sourceOwnerId !== owner.id || view.section.sourceOwnerEpoch !== owner.epoch)
          throw new Error('The logical Scene owner changed after detach.');
        if (sameProjection(view.projection, committed)) break;
        if (!sameProjection(view.projection, expectedScene)) throw new Error('The logical Scene changed outside this Close.');
        await waitForObservation();
      }
      expectedScene = committed;
    }
    view = scene();
    if (view.section.sourceOwnerId !== owner.id || view.section.sourceOwnerEpoch !== owner.epoch
      || !sameProjection(view.projection, expectedScene) || view.content.carried_object_attachments.length > 0
      || view.content.objects.some((object) => object.placement.kind !== 'cell_fixed'))
      throw new Error('The logical Scene still has carried objects or changed outside this Close.');
    return { resetDone, detached, idleCycle, sceneProjection: expectedScene };
  } catch (error) {
    throw new Error(`${error instanceof Error ? error.message : 'Close preparation is unresolved.'} ${resetDone
      ? 'Reset done; Close not completed.' : 'Close not completed.'} Detached objects confirmed: ${detached.join(', ') || 'none'}.${attempted
      ? ` Detach of ${attempted} was refused or is unconfirmed.` : ''} Editors were kept. Nothing was retried.`);
  }
}

/** GUI composition only. Every effect uses the existing owner command and refusal. */
export async function closeScenario({ expected, signal, admissionGeneration,
  waitForObservation, trackApply, progress, preparation }: CloseInputs): Promise<ManagerActiveProjection> {
  const confirmed = preparation;
  const current = await api.manager.status(signal);
  if (!current.source.available || current.source.freshness !== 'fresh'
    || !current.status.managerReady || current.status.active.retainedBundleId !== expected.retainedBundleId
    || current.status.active.graphRevision !== expected.graphRevision
    || (confirmed !== undefined && (!sameJson(current.status.active.publication, expected.publication)
      || current.source.sourceOwnerId !== confirmed.confirmed.managerOwnerId
      || current.source.sourceOwnerEpoch !== confirmed.confirmed.managerOwnerEpoch))) {
    throw new Error('Current Scenario changed; Close was not started.');
  }
  const input = { expected, signal, admissionGeneration, waitForObservation, trackApply, progress, preparation };
  const preparedClose = await prepareSceneForClose(input);
  const check = () => {
    if (confirmed === undefined) return;
    const latest = currentCloseSnapshot(expected, confirmed, signal);
    if (latest.programCycle.state !== 'idle_after_reset' || latest.programCycle.programRunId !== null
      || latest.programCycle.programRunnerOwnerEpoch !== preparedClose.idleCycle?.programRunnerOwnerEpoch
      || latest.programCycle.cycleGeneration !== preparedClose.idleCycle?.cycleGeneration)
      throw new Error('The Program cycle changed before Close completed.');
    const section = latest.runtime.sections.logicalScene;
    if (!section?.available || section.freshness !== 'fresh' || section.knownGaps.length > 0 || section.payload === null)
      throw new Error('The current logical Scene projection is unavailable before Close.');
    const projection = parseLogicalSceneProjection(section.payload).access.active_projection;
    if (projection === null || preparedClose.sceneProjection === undefined || !sameProjection(projection, preparedClose.sceneProjection))
      throw new Error('The logical Scene changed before Close completed. No further lifecycle effect was sent.');
  };
  try {
    progress('Retiring current lifecycle targets…');
    const retirementOwner = confirmed === undefined ? undefined : await api.cclc.owner(signal);
    const offer = await api.cclc.targetRetirementOffer(signal);
    if (retirementOwner !== undefined && (offer.scope.request_owner_epoch !== retirementOwner.requestOwnerEpoch
      || offer.scope.publication_generation !== expected.publication.publication_generation
      || offer.scope.active_public_pointer_digest !== expected.publication.durable_public_pointer_content_digest))
      throw new Error('The lifecycle retirement owner or publication changed.');
    const context = { request_id: identity(), request_owner_epoch: offer.scope.request_owner_epoch,
      source_label: 'composer_ui', correlation_id: identity() };
    check();
    await api.cclc.retireTargets({ request_context: context, scope: offer.scope, expected_targets: offer.targets }, signal);
    check();
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
      if (retirementOwner !== undefined && ownerEpoch !== retirementOwner.requestOwnerEpoch)
        throw new Error('The lifecycle owner changed during Close.');
      const incident = snapshot.lifecycleProgress?.incident;
      if (incident != null) {
        const request = lifecycleRequestFromSnapshot(snapshot, 'recover', requestContext(ownerEpoch, `close:${id}:recover`), admissionGeneration());
        request.reason = 'cell closed by operator';
        request.close_incident_recovery = { expected_incident_id: incident.incidentId };
        check();
        const result = await api.cclc.submitLifecycle(request, signal);
        if (result.incidentRecovery?.closureReason !== 'cell_closed_by_operator') throw new Error('Component incident Close is unresolved.');
      }
      const latest = await api.cclc.component(id, signal);
      if (latest.lifecycleState === 'active' || latest.lifecycleState === 'prepared') {
        check();
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
          if (occurrence.process_owner_kind === 'pneumatic_none_local') {
            const predecessor = current.status.active.publication;
            const revision = inventory.request.predecessor_projection_revision;
            if (!sameJson({ ...revision, scene_revision: { ...revision.scene_revision } }, predecessor.projection_revision)
              || inventory.request.expected_predecessor_publication_generation !== predecessor.publication_generation
              || inventory.request.expected_predecessor_public_pointer_digest !== predecessor.durable_public_pointer_content_digest) {
              throw new Error('Applied configuration changed during Close.');
            }
            // The parsed Manager proof names an installed logical owner, with no
            // child launch to Stop or drain. Lifecycle retirement still ran above.
            continue;
          }
          if (occurrence.cleanup_state === 'drained') continue;
          const request = { ...inventory.request, request_context: {
            request_id: identity(), request_owner_epoch: offer.scope.request_owner_epoch, source_label: 'composer_ui',
          }, controllable_component_id: id, launch_identity: occurrence.launch_identity, leader_pid: occurrence.leader_pid };
          check();
          let result = await api.cclc.stopComponentProcess(request);
          while (result.state === 'pending' || awaitingExactReply(result)) {
            if (result.state !== 'pending') progress(`Waiting for the exact Stop result of ${id}…`);
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
    check();
    if (confirmed !== undefined && selection.request_owner_epoch !== confirmed.confirmed.managerOwnerEpoch)
      throw new Error('The Manager owner changed before empty configuration preparation.');
    const prepared = await api.manager.prepareCanonicalEmpty({ request_owner_epoch: selection.request_owner_epoch,
      expected_admission_generation: admissionGeneration(), origin: {
        kind: 'sealed_canonical_empty', cell_id: selection.cell_id,
        deployment_artifact_id: selection.deployment_artifact_id, deployment_revision: selection.deployment_revision,
        retained_bundle_id: selection.candidate_retained_bundle_id, graph_revision: selection.candidate_graph_revision,
        admission_digest: selection.admission_digest,
      } });
    const expectedCurrent = { retained_bundle_id: expected.retainedBundleId, graph_revision: expected.graphRevision };
    check();
    const preview = await api.manager.previewCandidate({ deployment_artifact_id: prepared.deployment_artifact_id,
      deployment_revision: prepared.deployment_revision, prepared_activation_id: prepared.prepared_activation_id,
      candidate_retained_bundle_id: prepared.retained_bundle_id, candidate_graph_revision: prepared.graph_revision,
      expected_current: expectedCurrent, expected_admission_generation: admissionGeneration(), request_owner_epoch: selection.request_owner_epoch });
    signal.throwIfAborted();
    progress('Applying the empty configuration…');
    check();
    const accepted = await api.manager.applyCandidate({ request_id: identity(), request_owner_epoch: selection.request_owner_epoch,
      source_label: 'composer_ui', correlation_id: context.correlation_id,
      deployment_artifact_id: prepared.deployment_artifact_id, deployment_revision: prepared.deployment_revision,
      expected_current: expectedCurrent, expected_admission_generation: admissionGeneration(), reject_if_affected_busy: true, scene_initialization: 'preserve',
      prepared_activation: prepared, candidate_receipt: preview.candidateReceipt, accepted_impact_digest: preview.candidateReceipt.impact_digest });
    const apply = { applyId: accepted.applyId, applyGeneration: accepted.applyGeneration };
    trackApply(apply);
    let status = await api.manager.applyStatus(apply.applyId, apply.applyGeneration, signal);
    while (status.state === 'pending') {
      await waitForObservation();
      status = await api.manager.applyStatus(apply.applyId, apply.applyGeneration, signal);
    }
    if (status.state !== 'succeeded') throw new Error(status.failure?.message ?? 'Empty Apply failed or is unknown; the Scenario was not replaced.');
    for (;;) {
      signal.throwIfAborted();
      const final = await api.manager.status(signal);
      const active = final.status.active;
      if (!final.source.available || final.source.freshness !== 'fresh'
        || current.source.sourceOwnerEpoch !== selection.request_owner_epoch
        || final.source.sourceOwnerEpoch !== current.source.sourceOwnerEpoch
        || !current.source.sourceOwnerId
        || final.source.sourceOwnerId !== current.source.sourceOwnerId
        || !validPublication(active)) {
        throw new Error('Empty Apply completed, but its current publication cannot be verified.');
      }
      const transaction = active.publication.transaction;
      if (active.canonicalEmpty && active.retainedBundleId === prepared.retained_bundle_id
        && active.graphRevision === prepared.graph_revision && object(transaction)
        && transaction.kind === 'named_apply' && transaction.apply_id === apply.applyId
        && transaction.apply_generation === apply.applyGeneration) {
        progress('Scenario closed.');
        return active;
      }
      const predecessor = current.status.active;
      if (!validPublication(predecessor) || active.canonicalEmpty !== predecessor.canonicalEmpty
        || active.retainedBundleId !== predecessor.retainedBundleId
        || active.graphRevision !== predecessor.graphRevision
        || !sameJson(active.publication, predecessor.publication)) {
        throw new Error('Empty Apply completed, but its current publication cannot be verified.');
      }
      await waitForObservation();
    }
  } catch (error) {
    if (preparedClose.resetDone || preparedClose.detached.length > 0)
      throw new Error(`${error instanceof Error ? error.message : 'Close is unresolved.'} ${preparedClose.resetDone
        ? 'Reset done; Close not completed.' : 'Close not completed.'} Detached objects confirmed: ${preparedClose.detached.join(', ') || 'none'}. Editors were kept. Nothing was retried.`);
    throw error;
  }
}
