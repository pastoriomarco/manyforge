import { parseLogicalSceneProjection } from '../api/parsers';
import { sameAuthoringWorkspaceIdentity, type HeadlessStateResult } from '../hooks/useHeadlessState';
import type { ScenarioReplacementIdentity } from '../hooks/useProgramWorkspace';
import type { ProgramResetExpectation, ProgramResetOutcome } from '../hooks/useProgramRuntime';
import { scenarioCloseUncertainOutputRefusal, uncertainOutputs } from '../utils/uncertainOutputs';
import type { ComposerHeadlessState } from '../types/headless';
import { api } from '../api/client';
import { browserRequestIdentity, lifecycleRequestFromSnapshot, requestContext } from '../hooks/useCCLC';
import type { CarriedObjectAttachment, JsonValue, LogicalSceneProjection, ManagerActiveProjection, ManagerStatusProjection,
  ProjectionRevision, SceneTransactionResponse } from '../types/api';
import type { ComponentProcessStopResult } from '../types/cclc';
import type { SceneClearStatus } from '../types/scenarioClose';
import type { ManagerApplyIdentity } from './managerApply';
import { errorMessage } from './structuralResource';

function disableFailedText(terminalResult: string | null): string {
  return terminalResult === 'connection_lost'
    ? 'Disable failed: the connection to the component was lost.'
    : 'Disable failed.';
}

interface CloseInputs {
  expected: ManagerActiveProjection;
  signal: AbortSignal;
  admissionGeneration: () => number;
  waitForObservation: (signal: AbortSignal) => Promise<void>;
  trackApply: (identity: ManagerApplyIdentity) => void;
  progress: (message: string) => void;
  sceneClearNotice?: (source: ManagerApplyIdentity | { requestId: string }, error: Error) => void;
  /** Every confirmed Close prepares the live runtime before unloading. */
  preparation?: ClosePreparationInputs;
  /** SX-679: whether a Recover cell press runs now; Close then starts nothing. */
  cellRecoverRunning?: () => boolean;
}
export interface ClosePreparationInputs {
  confirmed: ScenarioReplacementIdentity;
  readSnapshot: () => ComposerHeadlessState;
  /** Pending accepted Reset only: retains source state so its unavailable interval can be bounded. */
  readResetSnapshot?: () => Pick<HeadlessStateResult, 'snapshot' | 'transportStatus' | 'semanticStatus'>;
  reset: (options: { correlationId: string; expected: ProgramResetExpectation; keepStopFault: boolean }) => Promise<ProgramResetOutcome>;
}
const identity = () => browserRequestIdentity('composer-close');

const CLOSE_RESET_UNAVAILABLE_WAIT_MS = 2_000;
const resetSourceUnavailable = () => new Error('Control source is unavailable; Close is unresolved.');

function waitForResetObservation(waitForObservation: CloseInputs['waitForObservation'], signal: AbortSignal,
  deadlineAt: number): Promise<void> {
  signal.throwIfAborted();
  const remaining = deadlineAt - performance.now();
  if (remaining <= 0) return Promise.reject(resetSourceUnavailable());
  const waitController = new AbortController();
  const timeoutError = resetSourceUnavailable();
  let timer: ReturnType<typeof setTimeout> | null = null;
  let abortListener: (() => void) | null = null;
  const deadline = new Promise<never>((_, reject) => {
    const checkDeadline = () => {
      const left = deadlineAt - performance.now();
      if (left > 0) {
        timer = setTimeout(checkDeadline, Math.ceil(left));
        return;
      }
      waitController.abort(timeoutError);
      reject(timeoutError);
    };
    timer = setTimeout(checkDeadline, Math.ceil(remaining));
  });
  const aborted = new Promise<never>((_, reject) => {
    const onAbort = () => {
      waitController.abort(signal.reason);
      reject(signal.reason);
    };
    abortListener = onAbort;
    signal.addEventListener('abort', onAbort, { once: true });
    if (signal.aborted) onAbort();
  });
  const observation = Promise.resolve().then(() => waitForObservation(waitController.signal));
  return Promise.race([observation, deadline, aborted]).finally(() => {
    if (timer !== null) clearTimeout(timer);
    if (abortListener !== null) signal.removeEventListener('abort', abortListener);
    if (!waitController.signal.aborted) waitController.abort();
  });
}
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

const sameProjection = (left: ProjectionRevision, right: ProjectionRevision) =>
  sameJson(left as unknown as JsonValue, right as unknown as JsonValue);

/** The same Scene (bundle, graph, scene id) at the same or a later revision.
 * Runtime pose updates advance the revision at any time during Close. */
export function sceneAtOrAfter(current: ProjectionRevision, base: ProjectionRevision): boolean {
  return current.retained_bundle_id === base.retained_bundle_id && current.graph_revision === base.graph_revision
    && current.scene_revision.scene_id === base.scene_revision.scene_id
    && (current.scene_revision.revision_id > base.scene_revision.revision_id || sameProjection(current, base));
}

const attachmentIdentity = (row: CarriedObjectAttachment) => JSON.stringify([row.carried_object_attachment_id,
  row.scene_resource_id, row.controllable_component_id, row.command_target_id]);
function sameMembers(left: string[], right: string[]): boolean {
  const [a, b] = [[...left].sort(), [...right].sort()];
  return a.length === b.length && a.every((value, index) => value === b[index]);
}

/** Which Scene, objects, attachments and policy: not their poses or revisions. */
function sameSceneStructure(left: JsonValue | undefined, right: JsonValue | undefined): boolean {
  if (left === undefined || right === undefined || left === null || right === null) return left === right;
  try {
    const [a, b] = [parseLogicalSceneProjection(left), parseLogicalSceneProjection(right)];
    const [pa, pb] = [a.access.active_projection, b.access.active_projection];
    if (pa === null || pb === null || a.snapshot === null || b.snapshot === null)
      return pa === pb && a.snapshot === b.snapshot;
    return pa.retained_bundle_id === pb.retained_bundle_id && pa.graph_revision === pb.graph_revision
      && pa.scene_revision.scene_id === pb.scene_revision.scene_id
      && sameMembers(a.snapshot.objects.map((row) => row.scene_resource_id), b.snapshot.objects.map((row) => row.scene_resource_id))
      && sameMembers(a.snapshot.carried_object_attachments.map(attachmentIdentity),
        b.snapshot.carried_object_attachments.map(attachmentIdentity))
      && sameJson(a.snapshot.configured_collision_policy as JsonValue, b.snapshot.configured_collision_policy as JsonValue);
  } catch {
    return false;
  }
}

/** The owner refused before any effect only because a newer Scene revision is active. */
function staleSceneRevision(error: unknown): boolean {
  const payload = (error as { payload?: JsonValue } | null)?.payload;
  return object(payload) && payload.ok === false && object(payload.error) && payload.error.code === 'stale_revision';
}
/** The Scene owner's typed refusal (not a transport failure or an abort). */
function sceneRefusal(error: unknown): boolean {
  const { payload, structuredError } = (error ?? {}) as { payload?: JsonValue; structuredError?: { code?: unknown } | null };
  return typeof structuredError?.code === 'string'
    || (object(payload) && payload.ok === false && object(payload.error) && typeof payload.error.code === 'string');
}
/** Bounded read-latest-and-retry of a detach refused only for a newer revision. */
const DETACH_STALE_ATTEMPTS = 8;

function currentCloseSnapshot(expected: ManagerActiveProjection, preparation: ClosePreparationInputs, signal: AbortSignal): ComposerHeadlessState {
  signal.throwIfAborted();
  const current = preparation.readSnapshot();
  const manager = current.runtime.sections.manager;
  if (!sameAuthoringWorkspaceIdentity(current.authoringWorkspace, preparation.confirmed.authoring)
    || current.authoringWorkspaceContainment !== null
    || !managerPublicationMatches(manager.payload?.active, expected))
    throw new Error('The confirmed workspace or Applied publication changed.');
  return current;
}

/** Typed codes of Close's own refusals (item 37); the message stays operator text. */
export const SCENARIO_CLOSE_OUTPUT_UNCERTAIN = 'scenario_close_output_uncertain';
export const SCENARIO_CLOSE_PROGRAM_NOT_RESET = 'scenario_close_program_not_reset';
/** SX-679: Close waits for a running Recover cell (the backend's retirement refusal too). */
export const RECOVER_CELL_IN_PROGRESS = 'recover_cell_in_progress';
const typedError = (code: string, message: string): Error => Object.assign(new Error(message), { code });
function refuseDuringCellRecover(input: CloseInputs, action: string): void {
  if (input.cellRecoverRunning?.() === true)
    throw typedError(RECOVER_CELL_IN_PROGRESS,
      `Recover cell is in progress; wait for it to finish or abort it. ${action} was not started.`);
}
/** The typed code of a refusal: a GUI refusal's own, or the owner's ErrorDTO code. */
export function closeErrorCode(error: unknown): string | null {
  const own = (error as { code?: unknown } | null)?.code;
  if (typeof own === 'string' && own !== '') return own;
  const owner = (error as { structuredError?: { code?: unknown } | null } | null)?.structuredError?.code;
  return typeof owner === 'string' && owner !== '' ? owner : null;
}
/** Close's wrapped message keeps the typed code of its cause. */
function closeError(message: string, cause: unknown): Error {
  const code = closeErrorCode(cause);
  const error = code === null ? new Error(message) : typedError(code, message);
  const structuredError = (cause as { structuredError?: unknown } | null)?.structuredError;
  return structuredError === undefined ? error : Object.assign(error, { structuredError });
}

/** Close always uses ordinary Reset with no optional restoration selected. Its
 * Reset never clears the cell Stop fault, whatever the ordering (D6). */
async function resetProgramForClose(input: ClosePreparationInputs, expected: ProgramResetExpectation,
  correlationId: string): Promise<ProgramResetOutcome> {
  return input.reset({ expected, correlationId, keepStopFault: true });
}

/** The same ordinary Reset-before-Close phase for every confirmed replacement.
 * Force close (item 37) runs it after its own Full Stop and Kernel step: it then
 * skips the output pre-check (those operations are history) and accepts the
 * Program cycle its Full Stop changed; the Scene must still be the confirmed one. */
export async function prepareProgramCycleForClose(
  { expected, signal, waitForObservation, progress }: CloseInputs, preparation: ClosePreparationInputs,
  force = false,
): Promise<{ resetDone: boolean; idleCycle: ProgramResetExpectation }> {
  let resetDone = false;
  let resetSubmitted = false;
  let idleCycle: ProgramResetExpectation | null = null;
  const read = () => currentCloseSnapshot(expected, preparation, signal);
  try {
    const initial = read();
    // Close never acknowledges outputs with unknown acceptance (contract 66).
    const uncertain = force ? null : scenarioCloseUncertainOutputRefusal(
      uncertainOutputs(initial.executionControlStatus?.manualOperations),
    );
    if (uncertain !== null) throw typedError(SCENARIO_CLOSE_OUTPUT_UNCERTAIN, uncertain);
    const cycle = initial.programCycle;
    const confirmedCycle = preparation.confirmed.closeCycle;
    const confirmedScene = preparation.confirmed.closeScene;
    if (confirmedCycle === undefined || confirmedScene === undefined
      || (!force && (cycle.state !== confirmedCycle.state || cycle.programRunId !== confirmedCycle.programRunId
      || cycle.cycleGeneration !== confirmedCycle.cycleGeneration
      || cycle.terminalOrSettlementReference !== confirmedCycle.terminalOrSettlementReference))
      || !sameSceneStructure(initial.runtime.sections.logicalScene?.payload as JsonValue | undefined,
        confirmedScene.payload as JsonValue | undefined))
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
        programRunId: cycle.programRunId, cycleGeneration: cycle.cycleGeneration,
      }, identity());
      if (result.status !== 'sent') {
        resetSubmitted = result.status === 'unconfirmed';
        throw new Error(result.message);
      }
      let unavailableMs = 0;
      let unavailableStartedAt: number | null = null;
      for (;;) {
        signal.throwIfAborted();
        const resetObservation = preparation.readResetSnapshot?.();
        const now = performance.now();
        const unavailable = resetObservation !== undefined && resetObservation.snapshot !== null
          && resetObservation.transportStatus === 'connected' && resetObservation.semanticStatus === 'unavailable';
        if (unavailableStartedAt !== null) {
          const elapsed = unavailableMs + now - unavailableStartedAt;
          if (elapsed >= CLOSE_RESET_UNAVAILABLE_WAIT_MS) throw resetSourceUnavailable();
          if (!unavailable) {
            unavailableMs = elapsed;
            unavailableStartedAt = null;
          }
        }
        let observation: ComposerHeadlessState | null;
        if (unavailable) {
          unavailableStartedAt ??= now;
          const remaining = CLOSE_RESET_UNAVAILABLE_WAIT_MS - unavailableMs - (now - unavailableStartedAt);
          if (remaining <= 0) throw resetSourceUnavailable();
          await waitForResetObservation(waitForObservation, signal, now + remaining);
          continue;
        }
        observation = resetObservation === undefined ? preparation.readSnapshot() : resetCloseSnapshot(resetObservation);
        if (observation === null) {
          await waitForObservation(signal);
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
          idleCycle = { programRunId: null, cycleGeneration: current.cycleGeneration };
          break;
        }
        if (current.state === 'recovery_required' || (current.cycleGeneration > result.reset.cycleGeneration
          && current.causalCorrelationId !== result.reset.correlationId))
          throw new Error('Reset did not settle the confirmed Program cycle.');
        await waitForObservation(signal);
      }
    } else if (cycle.state !== 'idle_after_reset' || cycle.programRunId !== null) {
      throw typedError(SCENARIO_CLOSE_PROGRAM_NOT_RESET,
        'Stop motion before closing the Scenario; Reset must be available for the stopped or held Program.');
    }
    idleCycle ??= { programRunId: null, cycleGeneration: cycle.cycleGeneration };
    return { resetDone, idleCycle };
  } catch (error) {
    throw closeError(`${errorMessage(error, 'Close preparation is unresolved.')} ${resetDone
      ? 'Reset done; Close not completed.' : resetSubmitted ? 'Close not completed; Reset was submitted; its outcome is not confirmed.' : 'Close not completed.'} Detached objects confirmed: none. Editors were kept. Nothing was retried.`, error);
  }
}

interface PreparedClose {
  resetDone: boolean;
  detached: string[];
  idleCycle?: ProgramResetExpectation;
  sceneProjection?: ProjectionRevision;
  /** The cell-fixed objects Close leaves in place; their poses may still update. */
  sceneObjects?: string[];
  /** Item 38: parts Force close left attached because their detach was refused. */
  attached?: string[];
}

/** Existing Reset and runtime detach only; no invented poses. Only a detach refused
 * for a newer Scene revision is read again and resent, bounded. With `force`
 * (item 38) a part whose detach the Scene owner refuses, as when the robot's
 * process is already dead, stays attached. */
export async function prepareSceneForClose(input: CloseInputs, force = false): Promise<PreparedClose> {
  const { expected, signal, waitForObservation, progress, preparation } = input;
  const detached: string[] = [];
  const attached: string[] = [];
  if (preparation === undefined) return { resetDone: false, detached };
  const { resetDone, idleCycle } = await prepareProgramCycleForClose(input, preparation, force);
  let attempted: string | null = null;
  const read = () => currentCloseSnapshot(expected, preparation, signal);
  const scene = () => {
    const current = read();
    if (current.programCycle.state !== 'idle_after_reset' || current.programCycle.programRunId !== null
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
    const attachments = [...view.content.carried_object_attachments];
    let expectedScene = view.projection;
    for (const confirmed of attachments) {
      let committed: ProjectionRevision | null = null;
      let kept = false;
      for (let attempt = 1; committed === null && !kept; attempt += 1) {
        view = scene();
        const requestOwnerEpoch = view.section.sourceOwnerEpoch!;
        if (!sceneAtOrAfter(view.projection, expectedScene))
          throw new Error('The logical Scene changed outside this Close.');
        if (!view.access.scene_writes_open || !view.access.projection_leases_open)
          throw new Error('The logical Scene owner currently fences detachment.');
        // Pose updates advance the attachment revision; the ids are its identity.
        const attachment = view.content.carried_object_attachments.find((row) =>
          attachmentIdentity(row) === attachmentIdentity(confirmed));
        if (attachment === undefined) throw new Error('The logical Scene changed outside this Close.');
        expectedScene = view.projection;
        attempted = confirmed.scene_resource_id;
        progress(`Detaching ${attempted} at its current pose…`);
        const requestId = identity();
        let response: SceneTransactionResponse;
        try {
          response = await api.scene.commit({
            request_context: { request_id: requestId, request_owner_epoch: requestOwnerEpoch,
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
        } catch (error) {
          // A pose update landed between the read and the detach: read the latest Scene again.
          if (attempt < DETACH_STALE_ATTEMPTS && staleSceneRevision(error)) continue;
          if (!force || staleSceneRevision(error) || !sceneRefusal(error)) throw error;
          progress(`${attempted} stays attached: its detach was refused (${errorMessage(error, 'refused')}).`);
          attached.push(confirmed.scene_resource_id); attempted = null; kept = true;
          continue;
        }
        const receipt = response.committed_projection_revision;
        if (response.request_id !== requestId || response.request_owner_epoch !== requestOwnerEpoch
          || !sceneAtOrAfter(receipt, expectedScene) || sameProjection(receipt, expectedScene))
          throw new Error('The detach receipt is not a successor of the requested Scene.');
        committed = receipt;
      }
      if (committed === null) continue;
      detached.push(confirmed.scene_resource_id); attempted = null;
      for (;;) {
        view = scene();
        if (sceneAtOrAfter(view.projection, committed)) break;
        if (!sceneAtOrAfter(view.projection, expectedScene)) throw new Error('The logical Scene changed outside this Close.');
        await waitForObservation(signal);
      }
      expectedScene = committed;
    }
    view = scene();
    if (!sceneAtOrAfter(view.projection, expectedScene) || !onlyKept(view.content, attached))
      throw new Error('The logical Scene still has carried objects or changed outside this Close.');
    return { resetDone, detached, idleCycle, sceneProjection: expectedScene,
      sceneObjects: view.content.objects.map((object) => object.scene_resource_id), attached };
  } catch (error) {
    throw closeError(`${errorMessage(error, 'Close preparation is unresolved.')} ${resetDone
      ? 'Reset done; Close not completed.' : 'Close not completed.'} Detached objects confirmed: ${detached.join(', ') || 'none'}.${attempted
      ? ` Detach of ${attempted} was refused or is unconfirmed.` : ''} Editors were kept. Nothing was retried.`, error);
  }
}

/** Every carried part is one Force close kept attached (none for Close). */
function onlyKept(content: Pick<NonNullable<LogicalSceneProjection['snapshot']>, 'carried_object_attachments' | 'objects'>,
  attached: string[]): boolean {
  return content.carried_object_attachments.every((row) => attached.includes(row.scene_resource_id))
    && content.objects.every((object) => object.placement.kind === 'cell_fixed' || attached.includes(object.scene_resource_id));
}

/** D6: appended when a later step fails after Close's early retirement. */
const RETIRED_NOTE = ' Lifecycle targets retired; finish with New or Load.';

/** D6: an open cell Stop fault (latch). Close keeps it: it never recovers it. */
export function cellStopFaultOpen(snapshot: ComposerHeadlessState): boolean {
  const fault = snapshot.executionControlStatus?.cellStopAction.fault;
  return fault != null && fault.state !== 'cleared';
}

/** The confirmed Scenario is still the Applied one; otherwise nothing starts. */
async function confirmedManagerStatus({ expected, signal, preparation: confirmed }: CloseInputs)
  : Promise<ManagerStatusProjection> {
  const current = await api.manager.status(signal);
  if (!current.source.available || current.source.freshness !== 'fresh'
    || !current.status.managerReady || current.status.active.retainedBundleId !== expected.retainedBundleId
    || current.status.active.graphRevision !== expected.graphRevision
    || (confirmed !== undefined && !sameJson(current.status.active.publication, expected.publication))) {
    throw new Error('Current Scenario changed; Close was not started.');
  }
  return current;
}

/** Retire the current lifecycle targets (the Close lease). */
async function retireLifecycleTargets({ expected, signal, progress, preparation: confirmed }: CloseInputs,
  check: () => void, phase: (next: 'sent' | 'retired') => void = () => {}) {
  progress('Retiring current lifecycle targets…');
  const offer = await api.cclc.targetRetirementOffer(signal);
  if (confirmed !== undefined && (offer.scope.publication_generation !== expected.publication.publication_generation
    || offer.scope.active_public_pointer_digest !== expected.publication.durable_public_pointer_content_digest))
    throw new Error('The lifecycle retirement publication changed.');
  const context = { request_id: identity(), request_owner_epoch: offer.scope.request_owner_epoch,
    source_label: 'composer_ui', correlation_id: identity() };
  check();
  phase('sent');
  await api.cclc.retireTargets({ request_context: context, scope: offer.scope, expected_targets: offer.targets }, signal);
  phase('retired');
  check();
  await api.cclc.ownerSettlement(offer.scope, signal);
  signal.throwIfAborted();
  return { offer, context };
}
type Retirement = Awaited<ReturnType<typeof retireLifecycleTargets>>;

/** D6: the retirement before the Program Reset on a latched cell. Its failure
 * reports how far it got; nothing is retried. */
async function retireEarly(input: CloseInputs, confirmed: ClosePreparationInputs): Promise<Retirement> {
  const retirement: { phase: 'none' | 'sent' | 'retired' } = { phase: 'none' };
  try {
    return await retireLifecycleTargets(input, () => { currentCloseSnapshot(input.expected, confirmed, input.signal); },
      (next) => { retirement.phase = next; });
  } catch (error) {
    const effect = retirement.phase === 'retired' ? RETIRED_NOTE
      : retirement.phase === 'sent' ? ' Lifecycle target retirement was sent; its outcome is not confirmed. Finish with New or Load.'
        : ' Close not completed.';
    throw closeError(`${errorMessage(error, 'Close is unresolved.')}${effect} The cell Stop fault is kept. Editors were kept. Nothing was retried.`, error);
  }
}

/** Each component: open incident closed, Disable, process Stop. Returns whether
 * an inactive post-failure state left a waiting target to retire again. With `force`
 * (item 37) an incident close or a Disable that is refused or fails only
 * leaves the component to its process Stop, which must still drain. */
async function retireComponents(input: CloseInputs, current: ManagerStatusProjection,
  { offer }: Retirement, check: () => void, force = false): Promise<boolean> {
  const { expected, signal, admissionGeneration, waitForObservation, progress } = input;
  let retireAgain = false;
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
    const [owner, snapshot] = await Promise.all([
      api.cclc.owner(signal), api.cclc.component(id, signal),
    ]);
    const ownerEpoch = owner.requestOwnerEpoch;
    if (snapshot.retainedBundleId !== expected.retainedBundleId || snapshot.graphRevision !== expected.graphRevision)
      throw new Error('Applied configuration changed during Close.');
    const incident = snapshot.lifecycleProgress?.incident;
    if (incident != null) {
      const request = lifecycleRequestFromSnapshot(snapshot, 'recover', requestContext(ownerEpoch, `close:${id}:recover`), admissionGeneration());
      request.reason = 'cell closed by operator';
      request.close_incident_recovery = { expected_incident_id: incident.incidentId };
      check();
      try {
        const result = await api.cclc.submitLifecycle(request, signal);
        if (result.incidentRecovery?.closureReason !== 'cell_closed_by_operator') throw new Error('Component incident Close is unresolved.');
      } catch (error) {
        if (!force) throw error;
        progress(`${id}: its incident was not closed (${errorMessage(error, 'unresolved')}); stopping its process…`);
      }
    }
    const latest = await api.cclc.component(id, signal);
    // D6: Close disables without a recovery. A faulted component has no
    // Disable (its recovery clears vendor faults); stopping its process
    // below shuts it down, and its fault stays with the robot.
    if (latest.lifecycleState === 'faulted') {
      progress(`${id} is faulted; stopping its process without a recovery…`);
    } else if (latest.lifecycleState === 'active' || latest.lifecycleState === 'prepared'
      || latest.lifecycleState === 'degraded') {
      check();
      try {
        const result = await api.cclc.submitLifecycle(lifecycleRequestFromSnapshot(latest, 'disable',
          requestContext(ownerEpoch, `close:${id}:disable`), admissionGeneration()), signal);
        if (result.transactionId === null) throw new Error(`Disable of ${id} was not accepted.`);
        let status = result.status;
        while (status === null || status.terminalResult === null) {
          if (status?.reconciliationRequired) throw new Error('Disable outcome requires reconciliation.');
          await waitForObservation(signal);
          status = await api.cclc.lifecycleStatus(result.transactionId, signal);
          if (status === null) throw new Error('Accepted disable transaction has no retained status.');
        }
        if ((status.terminalResult === 'failed' || status.terminalResult === 'connection_lost')
          && !status.reconciliationRequired) {
          const current = await api.cclc.component(id, signal);
          if (current.lifecycleState === 'declared' || current.lifecycleState === 'disabled'
            || current.lifecycleState === 'faulted') {
            // O3/D6: nothing remains engaged (or the provider is gone), so
            // process Stop below shuts it down and its target is retired again.
            progress(`${id} cannot be disabled (${status.error?.code ?? status.terminalResult}); stopping its process…`);
            retireAgain = true;
          } else if (current.lifecycleState === 'prepared' || current.lifecycleState === 'active'
            || current.lifecycleState === 'degraded') {
            if (status.error === null) throw new Error(disableFailedText(status.terminalResult));
            throw typedError(status.error.code, status.error.message);
          } else {
            throw new Error(status.error?.message ?? disableFailedText(status.terminalResult));
          }
        } else if (status.terminalResult !== 'succeeded' || status.reconciliationRequired) {
          throw new Error(status.error?.message ?? disableFailedText(status.terminalResult));
        }
      } catch (error) {
        if (!force) throw error;
        // Item 37: Disable where possible; resident operations refuse it.
        progress(`${id} was not disabled (${errorMessage(error, 'refused')}); stopping its process…`);
        retireAgain = true;
      }
    } else if (latest.lifecycleState !== 'disabled' && latest.lifecycleState !== 'declared') {
      if (!force) throw new Error(`Component ${id} has an unresolved lifecycle state.`);
      progress(`${id} has an unresolved lifecycle state; stopping its process…`);
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
          await waitForObservation(signal);
          signal.throwIfAborted();
          result = await api.cclc.componentProcessStopResult(request);
        }
        if (result.state !== 'drained') throw new Error(result.error?.message ?? `Process Stop of ${id} is unknown. Restart the workbench.`);
      }
    }
  }
  return retireAgain;
}

/** Before each later effect: the idle cycle and the detached Scene Close prepared. */
function closeCheck({ expected, signal, preparation: confirmed }: CloseInputs, preparedClose: PreparedClose) {
  return () => {
    if (confirmed === undefined) return;
    const latest = currentCloseSnapshot(expected, confirmed, signal);
    if (latest.programCycle.state !== 'idle_after_reset' || latest.programCycle.programRunId !== null
      || latest.programCycle.cycleGeneration !== preparedClose.idleCycle?.cycleGeneration)
      throw new Error('The Program cycle changed before Close completed.');
    const section = latest.runtime.sections.logicalScene;
    if (section == null || section.payload === null)
      throw new Error('The current logical Scene projection is unavailable before Close.');
    const parsed = parseLogicalSceneProjection(section.payload);
    const projection = parsed.access.active_projection;
    if (projection === null || parsed.snapshot === null || preparedClose.sceneProjection === undefined
      || !sceneAtOrAfter(projection, preparedClose.sceneProjection)
      || !onlyKept(parsed.snapshot, preparedClose.attached ?? [])
      || !sameMembers(parsed.snapshot.objects.map((object) => object.scene_resource_id), preparedClose.sceneObjects ?? []))
      throw new Error('The logical Scene changed before Close completed. No further lifecycle effect was sent.');
  };
}

export function sceneClearDescription(status: SceneClearStatus): string {
  const objects = status.remaining === null ? 'Remaining objects are unknown.'
    : status.state === 'unknown' ? `Last observed objects: ${status.remaining.join(', ') || 'none'}; the outcome is unknown.`
      : `Remaining objects: ${status.remaining.join(', ') || 'none'}.`;
  const refused = status.reason?.details.refused;
  const reasons = Array.isArray(refused) ? refused.flatMap((item: unknown) => {
    if (item === null || typeof item !== 'object' || Array.isArray(item)) return [];
    const row = item as Record<string, unknown>;
    return typeof row.resource_id === 'string' && typeof row.message === 'string'
      ? [`${row.resource_id}: ${row.message}${typeof row.code === 'string' ? ` (${row.code})` : ''}`] : [];
  }) : [];
  return `${status.reason?.message ?? 'No further detail is available.'} ${objects}${reasons.length > 0 ? ` ${reasons.join('; ')}.` : ''}`;
}

const SCENE_CLEAR_POLL_MS = 250;
function waitForSceneClearPoll(signal: AbortSignal): Promise<void> {
  signal.throwIfAborted();
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { signal.removeEventListener('abort', abort); resolve(); }, SCENE_CLEAR_POLL_MS);
    const abort = () => { clearTimeout(timer); reject(signal.reason); };
    signal.addEventListener('abort', abort, { once: true });
    if (signal.aborted) abort();
  });
}

/** The canonical-empty Apply that ends every Close, verified on its publication. */
async function applyCanonicalEmpty(input: CloseInputs, current: ManagerStatusProjection, correlationId: string,
  check: () => void): Promise<ManagerActiveProjection> {
  const { expected, signal, admissionGeneration, waitForObservation, trackApply, progress } = input;
  signal.throwIfAborted();
  progress('Preparing the empty configuration…');
  const selection = await api.manager.canonicalEmptySelection(signal);
  check();
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
    source_label: 'composer_ui', correlation_id: correlationId,
    deployment_artifact_id: prepared.deployment_artifact_id, deployment_revision: prepared.deployment_revision,
    expected_current: expectedCurrent, expected_admission_generation: admissionGeneration(), reject_if_affected_busy: true, scene_initialization: 'initialize',
    prepared_activation: prepared, candidate_receipt: preview.candidateReceipt, accepted_impact_digest: preview.candidateReceipt.impact_digest });
  const apply = { applyId: accepted.applyId, applyGeneration: accepted.applyGeneration };
  trackApply(apply);
  let status = await api.manager.applyStatus(apply.applyId, apply.applyGeneration, signal);
  while (status.state === 'pending') {
    await waitForObservation(signal);
    status = await api.manager.applyStatus(apply.applyId, apply.applyGeneration, signal);
  }
  if (status.state !== 'succeeded') throw new Error(status.failure?.message ?? 'Empty Apply failed or is unknown; the Scenario was not replaced.');
  for (;;) {
    signal.throwIfAborted();
    const final = await api.manager.status(signal);
    const active = final.status.active;
    if (!final.source.available || final.source.freshness !== 'fresh' || !validPublication(active)) {
      throw new Error('Empty Apply completed, but its current publication cannot be verified.');
    }
    const transaction = active.publication.transaction;
    if (active.canonicalEmpty && active.retainedBundleId === prepared.retained_bundle_id
      && active.graphRevision === prepared.graph_revision && object(transaction)
      && transaction.kind === 'named_apply' && transaction.apply_id === apply.applyId
      && transaction.apply_generation === apply.applyGeneration) {
      progress('Clearing the Scene…');
      const pollAbort = new AbortController();
      const abortPoll = () => pollAbort.abort(signal.reason);
      signal.addEventListener('abort', abortPoll, { once: true });
      let timer: ReturnType<typeof setTimeout> | undefined;
      const deadline = new Promise<null>((resolve) => { timer = setTimeout(() => { resolve(null); pollAbort.abort(); }, 10_000); });
      const poll = async (): Promise<ManagerActiveProjection> => {
      for (;;) {
        pollAbort.signal.throwIfAborted();
        let clearing: Awaited<ReturnType<typeof api.programs.sceneClearStatus>>;
        try { clearing = await api.programs.sceneClearStatus(pollAbort.signal); }
        catch {
          pollAbort.signal.throwIfAborted();
          input.sceneClearNotice?.(apply, typedError('scene_clear_unknown', 'Scene clear outcome unknown; check the Scene.'));
          return active;
        }
        let latest: ManagerStatusProjection;
        try { latest = await api.manager.status(pollAbort.signal); }
        catch {
          pollAbort.signal.throwIfAborted();
          input.sceneClearNotice?.(apply, typedError('scene_clear_unknown', 'Scene clear outcome unknown; check the Scene.'));
          return active;
        }
        pollAbort.signal.throwIfAborted();
        const transaction = latest.status.active.publication.transaction;
        if (!latest.source.available || latest.source.freshness !== 'fresh') {
          input.sceneClearNotice?.(apply, typedError('scene_clear_unknown', 'Scene clear outcome unknown; check the Scene.'));
          return active;
        }
        if (!object(transaction) || transaction.kind !== 'named_apply' || transaction.apply_id !== apply.applyId
          || transaction.apply_generation !== apply.applyGeneration)
          throw typedError('scene_clear_superseded', 'A newer Apply superseded Scene clearing.');
        if (clearing !== null && clearing.applyId === apply.applyId && clearing.applyGeneration === apply.applyGeneration) {
          if (clearing.state === 'succeeded') { progress('Scenario closed.'); return active; }
          if (clearing.state !== 'pending') {
            const reason = clearing.reason;
            const error = Object.assign(typedError(`scene_clear_${clearing.state}`,
              `Scene clearing ${clearing.state}: ${sceneClearDescription(clearing)}`),
            { structuredError: { code: `scene_clear_${clearing.state}`,
              message: reason?.message ?? '', details: { ...reason?.details, reason_code: reason?.code ?? null, remaining: clearing.remaining } } });
            if (clearing.state === 'superseded') throw error;
            input.sceneClearNotice?.(apply, error);
            return active;
          }
        }
        await waitForSceneClearPoll(pollAbort.signal);
      }
      };
      try {
        const result = await Promise.race([poll(), deadline]);
        if (result !== null) return result;
        signal.throwIfAborted();
        input.sceneClearNotice?.(apply, typedError('scene_clear_unknown', 'Scene clear outcome unknown; check the Scene.'));
        return active;
      } finally {
        if (timer !== undefined) clearTimeout(timer);
        signal.removeEventListener('abort', abortPoll);
        pollAbort.abort();
      }
    }
    const predecessor = current.status.active;
    if (!validPublication(predecessor) || active.canonicalEmpty !== predecessor.canonicalEmpty
      || active.retainedBundleId !== predecessor.retainedBundleId
      || active.graphRevision !== predecessor.graphRevision
      || !sameJson(active.publication, predecessor.publication)) {
      throw new Error('Empty Apply completed, but its current publication cannot be verified.');
    }
    await waitForObservation(signal);
  }
}

/** One direct Scene-owner clear when canonical empty is already Applied. */
async function clearAlreadyEmpty(input: CloseInputs, current: ManagerStatusProjection): Promise<ManagerActiveProjection> {
  const { expected, preparation, signal, progress } = input;
  if (preparation === undefined) throw new Error('A confirmed Scenario is required to clear the Scene.');
  const snapshot = currentCloseSnapshot(expected, preparation, signal);
  const section = snapshot.runtime.sections.logicalScene;
  if (!section?.available || section.freshness !== 'fresh' || section.knownGaps.length > 0 || section.payload === null
    || !sameSceneStructure(section.payload as JsonValue, preparation.confirmed.closeScene?.payload as JsonValue | undefined))
    throw new Error('The confirmed logical Scene is unavailable or changed.');
  const projection = parseLogicalSceneProjection(section.payload).access.active_projection;
  if (projection === null || projection.retained_bundle_id !== expected.retainedBundleId
    || projection.graph_revision !== expected.graphRevision)
    throw new Error('The current logical Scene is unavailable or fenced.');
  const publicationGeneration = expected.publication.publication_generation;
  if (typeof publicationGeneration !== 'number' || !Number.isSafeInteger(publicationGeneration) || publicationGeneration < 1)
    throw new Error('The current Applied publication is unavailable.');
  const requestId = identity();
  const source = { requestId };
  const controller = new AbortController();
  const abort = () => controller.abort(signal.reason);
  signal.addEventListener('abort', abort, { once: true });
  if (signal.aborted) abort();
  let timer: ReturnType<typeof setTimeout> | undefined;
  const deadline = new Promise<null>((resolve) => {
    timer = setTimeout(() => { resolve(null); controller.abort(); }, 10_000);
  });
  progress('Clearing the Scene…');
  const request = api.programs.clearEmptyScene({ request_id: requestId,
    request_owner_epoch: snapshot.programCycle.programRunnerOwnerEpoch, source_label: 'composer_ui', correlation_id: null,
    expected_admission_generation: input.admissionGeneration(), clear_empty_scene: true,
    expected_applied_record_digest: null, expected_publication_generation: publicationGeneration }, controller.signal)
    .then((status) => {
      if (status === null || status.requestId !== requestId || status.state === 'pending')
        throw new Error('Direct Scene clear has no terminal receipt.');
      return status;
    });
  const poll = async (): Promise<SceneClearStatus> => {
    for (;;) {
      controller.signal.throwIfAborted();
      const status = await api.programs.sceneClearStatus(controller.signal);
      controller.signal.throwIfAborted();
      if (status !== null && status.requestId === requestId && status.state !== 'pending') return status;
      await waitForSceneClearPoll(controller.signal);
    }
  };
  try {
    const status = await Promise.race([request, poll(), deadline]);
    signal.throwIfAborted();
    if (status === null) {
      input.sceneClearNotice?.(source, typedError('scene_clear_unknown', 'Scene clear outcome unknown; check the Scene.'));
    } else if (status.state === 'superseded') {
      throw typedError('scene_clear_superseded', 'A newer request superseded Scene clearing.');
    } else if (status.state !== 'succeeded') {
      const error = Object.assign(typedError(`scene_clear_${status.state}`, sceneClearDescription(status)),
        { structuredError: { code: `scene_clear_${status.state}`, message: status.reason?.message ?? '',
          details: { ...status.reason?.details, remaining: status.remaining } } });
      input.sceneClearNotice?.(source, error);
    } else progress('Scenario closed.');
    return current.status.active;
  } catch (error) {
    if (signal.aborted) throw typedError('scene_clear_unknown', 'Scene clear was requested; its outcome is unknown. Check the Scene.');
    if (closeErrorCode(error) === 'scene_clear_superseded') throw error;
    input.sceneClearNotice?.(source, typedError('scene_clear_unknown', 'Scene clear outcome unknown; check the Scene.'));
    return current.status.active;
  } finally {
    if (timer !== undefined) clearTimeout(timer);
    signal.removeEventListener('abort', abort);
    controller.abort();
  }
}

/** GUI composition only. Every effect uses the existing owner command and refusal. */
export async function closeScenario(input: CloseInputs): Promise<ManagerActiveProjection> {
  const { expected, signal, preparation } = input;
  const confirmed = preparation;
  const current = await confirmedManagerStatus(input);
  if (expected.canonicalEmpty) return clearAlreadyEmpty(input, current);
  refuseDuringCellRecover(input, 'Close');
  // D6: with an open cell Stop fault, Close retires the lifecycle targets
  // first. The Kernel then admits only Close's own claims (the verified
  // Disable, the canonical-empty Apply) and keeps the fault for Recover cell.
  let early: Retirement | null = null;
  const initial = confirmed === undefined ? null : currentCloseSnapshot(expected, confirmed, signal);
  // D6: which order Close takes depends on the cell Stop fault, so it needs the
  // current control projection; without it Close does nothing.
  if (initial !== null && initial.executionControlStatus == null)
    throw typedError('cell_state_unavailable', 'Cell control state is unavailable (cell_state_unavailable); Close was not started. Retry.');
  if (confirmed !== undefined && initial !== null && cellStopFaultOpen(initial)) {
    // Close never acknowledges outputs with unknown acceptance (contract 66).
    const uncertain = scenarioCloseUncertainOutputRefusal(
      uncertainOutputs(initial.executionControlStatus?.manualOperations),
    );
    if (uncertain !== null) throw typedError(SCENARIO_CLOSE_OUTPUT_UNCERTAIN, `${uncertain} Close not completed. Nothing was retried.`);
    early = await retireEarly(input, confirmed);
  }
  let preparedClose: PreparedClose;
  try {
    preparedClose = await prepareSceneForClose(input);
  } catch (error) {
    throw early === null ? error : closeError(`${errorMessage(error, 'Close is unresolved.')}${RETIRED_NOTE}`, error);
  }
  const check = closeCheck(input, preparedClose);
  try {
    const retirement = early ?? await retireLifecycleTargets(input, check);
    // A failed Disable with a declared, disabled or faulted current state left
    // a waiting `disable` target: retire it before the empty Apply.
    if (await retireComponents(input, current, retirement, check)) await retireLifecycleTargets(input, check);
    return await applyCanonicalEmpty(input, current, retirement.context.correlation_id, check);
  } catch (error) {
    const retired = early === null ? '' : RETIRED_NOTE;
    if (preparedClose.resetDone || preparedClose.detached.length > 0)
      throw closeError(`${errorMessage(error, 'Close is unresolved.')} ${preparedClose.resetDone
        ? 'Reset done; Close not completed.' : 'Close not completed.'} Detached objects confirmed: ${preparedClose.detached.join(', ') || 'none'}. Editors were kept. Nothing was retried.${retired}`, error);
    if (early !== null) throw closeError(`${errorMessage(error, 'Close is unresolved.')}${retired}`, error);
    throw error;
  }
}

/**
 * Item 37: Force close, the operator's explicit action after a refused Close.
 * 1. A cell Full Stop that the Kernel must confirm; otherwise it refuses
 *    (`scenario_force_close_stop_unconfirmed`) and nothing else is done.
 * 2. Forced retirement of the targets (the Kernel now admits resident
 *    operations).
 * 3. The Kernel step that moves stuck or uncertain operations into history,
 *    marked force-closed, with one History row of what stays unknown.
 * 4. The ordinary Reset and detach, while the robots' processes still run:
 *    the detach reads the carrier frame from the robot's live state. A part
 *    whose detach is refused (the robot's process already dead) stays
 *    attached; the server's empty preparation, which knows Force close ran its
 *    Kernel step, skips the attachment check (item 38).
 * 5. Each component disabled where possible and its processes stopped (a
 *    process Stop must still drain), then the canonical-empty Apply.
 * It never commands an output, and it keeps the cell Stop fault.
 */
export async function forceCloseScenario(input: CloseInputs): Promise<ManagerActiveProjection> {
  const { expected, signal, progress, preparation } = input;
  refuseDuringCellRecover(input, 'Force close');
  if (preparation === undefined) throw new Error('Force close needs the confirmed Scenario; nothing was done.');
  const current = await confirmedManagerStatus(input);
  const control = currentCloseSnapshot(expected, preparation, signal).executionControlStatus;
  if (control == null)
    throw typedError('cell_state_unavailable', 'Cell control state is unavailable (cell_state_unavailable); Force close was not started. Retry.');
  const execution = await api.execution.identity(signal);
  const context = () => ({ request_id: identity(), request_owner_epoch: execution.ownerEpoch,
    source_label: 'composer_ui', correlation_id: null });
  progress('Full Stop of the cell, waiting for its confirmation…');
  try {
    await api.execution.forceClose({ request_context: context(), step: 'stop',
      expected_stop_generation: control.cellStopAction.expectedStopGeneration }, signal);
  } catch (error) {
    throw closeError(`${errorMessage(error, 'The Full Stop is not confirmed.')} Force close stopped before any retirement. The cell Stop fault is kept. Editors were kept. Nothing was retried.`, error);
  }
  const retirement = await retireEarly(input, preparation);
  const workspace = () => { currentCloseSnapshot(expected, preparation, signal); };
  try {
    progress('Moving stuck operations into history…');
    const closed = await api.execution.forceClose({ request_context: context(), step: 'close_operations' }, signal);
    if (closed.operations.length > 0)
      progress(`Moved into history, effect unknown: ${closed.operations.map((row) => row.operationId).join(', ')}.${
        closed.motionBlockCleared ? ' Motion block cleared; robots still need Prepare and Enable.' : ''}`);
    // B1: detach before any process Stop, while the robot's state source runs.
    const preparedClose = await prepareSceneForClose(input, true);
    const check = closeCheck(input, preparedClose);
    if (await retireComponents(input, current, retirement, workspace, true)) await retireLifecycleTargets(input, check);
    return await applyCanonicalEmpty(input, current, retirement.context.correlation_id, check);
  } catch (error) {
    throw closeError(`${errorMessage(error, 'Force close is unresolved.')}${RETIRED_NOTE} The cell Stop fault is kept.`, error);
  } finally {
    // SX-616: the server's attachment exception lives for this attempt only.
    await api.execution.forceClose({ request_context: context(), step: 'end' }).catch(() => undefined);
  }
}
