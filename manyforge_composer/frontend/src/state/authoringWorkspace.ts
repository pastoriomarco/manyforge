import type {
  AuthoringWorkspaceProjectFacts,
  AuthoringWorkspaceSnapshot,
  CurrentScenarioResponse,
  DeploymentWorkspaceResponse,
  JsonValue,
  LocalWorkspaceFenceRequest,
  ProgramWorkspaceResponse,
  ScenarioDocumentFenceRequest,
} from '../types/api';
import type {
  AuthoringWorkspaceContainment,
  AuthoringWorkspaceIdentity,
  ComposerHeadlessState,
} from '../types/headless';
import { manualOperationSettled } from './heldRunAuthoring';
import type { WorkbenchNotice } from './workbenchNotices';

/** Component reason the alarm owner issues for active alarms it can report. */
const ACTIVE_FUNCTIONAL_ALARM_REASON = 'active_functional_alarm';

/**
 * One installed composite authoring publication.
 *
 * `identity` is the observed identity the snapshot was read at, so a consumer can
 * prove that what it displays belongs to the identity it fences. A publication
 * whose identity no longer matches the control stream is superseded, not merged.
 */
export interface PublishedAuthoringWorkspace {
  identity: AuthoringWorkspaceIdentity;
  snapshot: AuthoringWorkspaceSnapshot;
}

/**
 * Why Program Apply has no current Scenario to apply to, or ``null``.
 *
 * A Scenario applied without a Program (Scenario New) reports exactly
 * `program_not_selected`: it is not execution-ready, but it is a valid base
 * for Applying a first Program.
 */
export function programApplyScenarioReason(
  projection: CurrentScenarioResponse | null,
): string | null {
  if (projection === null) return 'Current Scenario is still being read from the backend.';
  if (projection.currentScenario !== null) return null;
  if (projection.unavailableReasons.length === 1
    && projection.unavailableReasons[0] === 'program_not_selected') return null;
  return projection.unavailableReasons.length > 0
    ? projection.unavailableReasons.join(', ').replaceAll('_', ' ')
    : 'No exact current Scenario exists.';
}

/** Document-specific prerequisites; admission remains the common owner gate. */
export function programActionUnavailableReason(
  workspace: ProgramWorkspaceResponse | null,
  action: 'apply' | 'revert' | 'discard',
  commonReason: string | null,
): string | null {
  if (commonReason !== null) return commonReason;
  if (workspace === null) return 'Program workspace is not loaded.';
  if (action === 'discard') {
    return !workspace.applied.available ? 'No Applied Program is available.'
      : workspace.hasUnappliedChanges !== true ? 'Nothing to apply: the Program Draft matches Applied.' : null;
  }
  if (action === 'revert') {
    // Revert reloads Saved and applies it, so it exists only while Saved
    // differs from Applied; Draft-only edits over that baseline are Discard.
    return !workspace.saved.available ? 'No Saved Program is available.'
      : workspace.applied.available && workspace.saved.programRevision !== null
        && workspace.saved.programRevision === workspace.applied.programRevision
        ? 'Nothing to revert: the Saved Program matches Applied.' : null;
  }
  return !workspace.draft.program || !workspace.applicability.applyReady
    ? 'Complete the Draft before Apply.'
    : workspace.applied.matchesDraft === true ? 'Nothing to apply: the Program Draft matches Applied.' : null;
}

export function authoringWorkspaceIdentity(
  snapshot: AuthoringWorkspaceSnapshot,
): AuthoringWorkspaceIdentity {
  return {
    workspaceEpoch: snapshot.workspaceEpoch,
    workspaceRevision: snapshot.workspaceRevision,
    programDraftId: snapshot.program.draft.draftId,
    programDraftRevision: snapshot.program.draft.draftRevision,
    deploymentDraftId: snapshot.deployment.draftId,
    deploymentDraftRevision: snapshot.deployment.draftRevision,
  };
}

/**
 * Whether one publication is the exact identity the control stream advertises.
 *
 * Authoring stays disabled with a stable synchronizing reason while this is
 * false, so a stale identity can never be fenced and a local edit can never be
 * sent against content the backend has already replaced.
 */
export function publicationIsCurrent(
  published: PublishedAuthoringWorkspace | null,
  snapshot: ComposerHeadlessState | null,
): boolean {
  if (published === null || snapshot === null) return false;
  const observed = snapshot.authoringWorkspace;
  const installed = published.identity;
  return installed.workspaceEpoch === observed.workspaceEpoch
    && installed.workspaceRevision === observed.workspaceRevision
    && installed.programDraftId === observed.programDraftId
    && installed.programDraftRevision === observed.programDraftRevision
    && installed.deploymentDraftId === observed.deploymentDraftId
    && installed.deploymentDraftRevision === observed.deploymentDraftRevision;
}

const ADMISSION_REASON_ALLOWED =
  'Authoring is currently locked by the backend.';
const ADMISSION_REASON_DISCONNECTED =
  'Backend status is disconnected or has not arrived.';

/**
 * The single authoring-admission gate every migrated request shares.
 *
 * Returns the reason authoring is unavailable, or ``null`` when it is available.
 * The synchronizing reason is deliberately the same text everywhere so the UI
 * cannot claim a different cause than the request path enforces.
 */
export function authoringUnavailableReason(
  published: PublishedAuthoringWorkspace | null,
  snapshot: ComposerHeadlessState | null,
): string | null {
  switch (authoringUnavailableCause(published, snapshot)) {
    case 'disconnected': return ADMISSION_REASON_DISCONNECTED;
    case 'contained':
      return authoringContainmentMessage(snapshot!.authoringWorkspaceContainment!);
    case 'not_read': return 'The shared authoring workspace has not been read yet.';
    case 'synchronizing': return 'The displayed workspace is still synchronizing with the backend.';
    case 'execution_active':
      return `Program execution is ${snapshot!.programCycle.state.replaceAll('_', ' ')}; authoring requires Idle.`;
    case 'not_admitted':
      return snapshot?.executionControlStatus?.manualOperations.some((operation) => (
        operation.resident && !manualOperationSettled(operation)
      ))
        ? 'Authoring is locked while a manual operation is active.'
        : ADMISSION_REASON_ALLOWED;
    case null: return null;
  }
}

/**
 * Why editing the Draft (Program, Scene or Deployment, including Discard) is
 * unavailable, or ``null``. Unlike the other authoring actions, an edit only
 * changes the Draft and takes no Kernel claim, so it never waits for Kernel
 * authoring admission (another client's Apply, a manual operation), and it is
 * also available while a Program run exists with all its motion confirmed
 * stopped (the backend's published `heldRunDraftEditAllowed`); the edits then
 * reach the robot only through Apply, which Resets the run first.
 */
export function draftEditUnavailableReason(
  published: PublishedAuthoringWorkspace | null,
  snapshot: ComposerHeadlessState | null,
): string | null {
  const cause = authoringUnavailableCause(published, snapshot);
  if (cause === 'not_admitted') return null;
  if (cause !== 'execution_active') {
    return authoringUnavailableReason(published, snapshot);
  }
  return snapshot!.heldRunDraftEditAllowed
    ? null
    : `Program execution is ${snapshot!.programCycle.state.replaceAll('_', ' ')}; editing requires Idle or all Program motion stopped (Stop, Halt, a failure pause, or Executed but not yet Run).`;
}

/**
 * Why New, Load or Close is unavailable, or ``null``. A
 * finished or stopped run doesn't block them: the server Resets a run awaiting
 * Reset first and refuses a run in progress with its typed reason.
 * A closed Kernel admission (a held claim) doesn't block them either: New
 * always works, so the server decides and names any refusal. Only an
 * Apply in flight disables them (the caller's Manager gate).
 */
export function scenarioReplacementUnavailableReason(
  published: PublishedAuthoringWorkspace | null,
  snapshot: ComposerHeadlessState | null,
): string | null {
  const cause = authoringUnavailableCause(published, snapshot);
  return cause === 'execution_active' || cause === 'not_admitted'
    ? null : authoringUnavailableReason(published, snapshot);
}

/**
 * The typed cause behind :func:`authoringUnavailableReason`, so presentation
 * can distinguish normal execution/synchronization (information) from lost
 * authoritative state without treating a closed permission gate as a fault.
 */
export type AuthoringUnavailableCause =
  | 'disconnected'
  | 'contained'
  | 'not_read'
  | 'synchronizing'
  | 'execution_active'
  | 'not_admitted';

export function authoringUnavailableCause(
  published: PublishedAuthoringWorkspace | null,
  snapshot: ComposerHeadlessState | null,
): AuthoringUnavailableCause | null {
  if (snapshot === null) return 'disconnected';
  // A contained workspace refuses every edit until Composer restarts; that
  // outranks synchronization, which can no longer converge.
  if (snapshot.authoringWorkspaceContainment !== null) return 'contained';
  if (published === null) return 'not_read';
  if (!publicationIsCurrent(published, snapshot)) return 'synchronizing';
  if (snapshot.programCycle.state !== 'idle_after_reset') return 'execution_active';
  if (!snapshot.authoringAdmission.authoringAllowed) return 'not_admitted';
  return null;
}

/**
 * Owner-reported faults remain errors independently of the authoring gate.
 * A fault that first appears during a known controlled transition is its
 * expected, related gap; one present before it began, or one
 * that outlasts it, stays an error.
 *
 * The functional-alarm component also reports `fault` when the reconciled
 * alarm owner is available and merely carries active alarms
 * (`active_functional_alarm`). Those alarms are presented by the alarm surface
 * itself; they are not an unavailable component, so only an unavailable alarm
 * owner becomes a notice here.
 */
export function requiredComponentFaultNotices(snapshot: ComposerHeadlessState | null): WorkbenchNotice[] {
  return (snapshot?.componentOverview.components ?? [])
    .filter((component) => component.required && ['fault', 'offline'].includes(component.status))
    .filter((component) => component.reasonCode !== ACTIVE_FUNCTIONAL_ALARM_REASON)
    .map((component) => ({
      id: `component.health.${component.id}`,
      severity: 'error',
      domain: 'runtime',
      title: `${component.label} unavailable`,
      message: component.detail,
      expectedDuringTransition: true,
    }));
}

/** The operator-facing text of one published workspace containment. */
export function authoringContainmentMessage(
  containment: AuthoringWorkspaceContainment,
): string {
  const saved = containment.savedPath === null
    ? ''
    : ` Saved file: ${containment.savedPath}.`;
  const cause = containment.cause.replace(/\.+$/, '');
  return `Authoring contained: ${cause}.${saved} Restart Composer to recover.`;
}

/**
 * The published workspace containment as one persistent error. It is shown
 * without an attempted edit and stays until a Composer restart (a normal cold
 * start) publishes a new, uncontained workspace.
 */
export function authoringContainmentNotices(
  snapshot: ComposerHeadlessState | null,
): WorkbenchNotice[] {
  const containment = snapshot?.authoringWorkspaceContainment ?? null;
  if (containment === null) return [];
  return [{
    id: 'authoring.contained',
    severity: 'error',
    domain: 'program_authoring',
    title: 'Authoring contained',
    message: authoringContainmentMessage(containment),
  }];
}

export function programLocalFence(
  published: PublishedAuthoringWorkspace,
  snapshot: ComposerHeadlessState,
  requestOwnerEpoch: string,
  requestId: string,
): LocalWorkspaceFenceRequest {
  return programLocalFenceFromAdmission(
    published, snapshot.authoringAdmission.admissionGeneration, requestOwnerEpoch, requestId,
  );
}

export function programLocalFenceFromAdmission(
  published: PublishedAuthoringWorkspace,
  admissionGeneration: number,
  requestOwnerEpoch: string,
  requestId: string,
): LocalWorkspaceFenceRequest {
  return {
    ...localDraftFence(published),
    expectedAdmissionGeneration: admissionGeneration,
    requestOwnerEpoch,
    requestId,
  };
}

/** The same local fence for Deployment routes, which check the Deployment Draft. */
export function deploymentLocalFence(
  published: PublishedAuthoringWorkspace,
  snapshot: ComposerHeadlessState,
  requestOwnerEpoch: string,
  requestId: string,
): LocalWorkspaceFenceRequest {
  return {
    ...programLocalFence(published, snapshot, requestOwnerEpoch, requestId),
    draftId: published.identity.deploymentDraftId,
    expectedDraftRevision: published.identity.deploymentDraftRevision,
  };
}

export function scenarioLocalFence(
  published: PublishedAuthoringWorkspace,
  snapshot: ComposerHeadlessState,
  requestOwnerEpoch: string,
  requestId: string,
): ScenarioDocumentFenceRequest {
  return {
    ...programLocalFence(published, snapshot, requestOwnerEpoch, requestId),
    deploymentDraftId: published.identity.deploymentDraftId,
    expectedDeploymentDraftRevision: published.identity.deploymentDraftRevision,
  };
}

/**
 * Whether the shared publication is authoritative over one Program projection.
 *
 * A mutation response can arrive before the control delta that advertises it, so
 * a publication that is older than the projection is skipped instead of
 * reverting the just-committed content. A different Draft identity always means
 * the whole workspace was replaced, and then the publication is authoritative.
 */
export function publicationSupersedesProgram(
  published: PublishedAuthoringWorkspace,
  projection: ProgramWorkspaceResponse,
): boolean {
  const draft = published.snapshot.program.draft;
  if (draft.draftId !== projection.draft.draftId) return true;
  return draft.draftRevision >= projection.draft.draftRevision;
}

/** The Deployment counterpart of :func:`publicationSupersedesProgram`. */
export function publicationSupersedesDeployment(
  published: PublishedAuthoringWorkspace,
  projection: DeploymentWorkspaceResponse,
): boolean {
  const draft = published.snapshot.deployment;
  if (draft.draftId !== projection.draft.id) return true;
  return draft.draftRevision >= projection.draft.revision;
}

function localDraftFence(
  published: PublishedAuthoringWorkspace,
): Pick<
  LocalWorkspaceFenceRequest,
  | 'draftId'
  | 'expectedDraftRevision'
  | 'expectedWorkspaceEpoch'
  | 'expectedWorkspaceRevision'
> {
  return {
    draftId: published.identity.programDraftId,
    expectedDraftRevision: published.identity.programDraftRevision,
    expectedWorkspaceEpoch: published.identity.workspaceEpoch,
    expectedWorkspaceRevision: published.identity.workspaceRevision,
  };
}

/**
 * Replace the authoring half of one Program workspace projection.
 *
 * The Program Draft and its Saved baseline are authoring truth and now come from
 * the shared composite publication, while applicability, Applied identity and
 * the two derived dirty flags stay with the existing owner projection: they
 * describe runtime truth this owner does not own.
 */
export function withSharedProgramAuthoring(
  projection: ProgramWorkspaceResponse,
  snapshot: AuthoringWorkspaceSnapshot,
): ProgramWorkspaceResponse {
  return {
    ...projection,
    authoringWorkspace: authoringWorkspaceIdentity(snapshot),
    draft: snapshot.program.draft,
    saved: snapshot.program.saved,
    hasUnsavedChanges: snapshot.program.draft.programLoaded
      && !snapshot.program.saved.matchesDraft,
  };
}

/**
 * Replace the authoring half of one Deployment workspace projection.
 *
 * The Deployment Draft and Saved baseline come from the shared composite
 * publication; the compiled artifact identity and the Manager/Applied
 * projections stay with their existing owner read.
 */
/**
 * The owner read predates a publication that changed the Deployment Draft or
 * its Saved baseline, so its Applied comparisons (Run admission, Saved vs
 * Applied) describe other documents until the owner is read again.
 */
/** The owner comparisons of a projection the publication outdated, until read again. */
export const DEPLOYMENT_RECHECKING_REASON = 'Rechecking the Deployment Draft against Manager Applied';

export function deploymentComparisonsStale(
  projection: DeploymentWorkspaceResponse,
  snapshot: AuthoringWorkspaceSnapshot,
): boolean {
  const deployment = snapshot.deployment;
  return (projection.authoringWorkspace.workspaceEpoch !== snapshot.workspaceEpoch
    || projection.authoringWorkspace.workspaceRevision !== snapshot.workspaceRevision)
    && (deployment.draftId !== projection.draft.id
      || deployment.draftRevision !== projection.draft.revision
      || deployment.savedPath !== projection.saved.path
      || deployment.differsFromSaved !== projection.draft.differsFromSaved);
}

export function withSharedDeploymentAuthoring(
  projection: DeploymentWorkspaceResponse,
  snapshot: AuthoringWorkspaceSnapshot,
): DeploymentWorkspaceResponse {
  // Stale comparisons are unknown, never a stale certainty.
  const stale = deploymentComparisonsStale(projection, snapshot);
  return {
    ...projection,
    authoringWorkspace: authoringWorkspaceIdentity(snapshot),
    draft: {
      ...projection.draft,
      id: snapshot.deployment.draftId,
      revision: snapshot.deployment.draftRevision,
      document: snapshot.deployment.document,
      differsFromSaved: snapshot.deployment.differsFromSaved,
    },
    saved: {
      path: snapshot.deployment.savedPath,
      deploymentRevision: deploymentRevisionOf(snapshot.deployment.savedDocument),
      differsFromApplied: stale ? null : projection.saved.differsFromApplied,
    },
    runAdmission: stale
      ? { ready: false, reason: DEPLOYMENT_RECHECKING_REASON }
      : projection.runAdmission,
  };
}

export function deploymentRevisionOf(
  document: Record<string, JsonValue> | null,
): string | null {
  const revision = document?.deployment_revision;
  return typeof revision === 'string' ? revision : null;
}

export function projectFacts(
  published: PublishedAuthoringWorkspace | null,
): AuthoringWorkspaceProjectFacts | null {
  return published === null ? null : published.snapshot.project;
}
