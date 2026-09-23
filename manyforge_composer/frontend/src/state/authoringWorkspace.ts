import type {
  AuthoringWorkspaceProjectFacts,
  AuthoringWorkspaceSnapshot,
  DeploymentWorkspaceResponse,
  JsonValue,
  LocalWorkspaceFenceRequest,
  ProgramWorkspaceResponse,
  ScenarioDocumentFenceRequest,
} from '../types/api';
import type {
  AuthoringWorkspaceIdentity,
  ComposerHeadlessState,
} from '../types/headless';

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
  'The backend has not admitted authoring; execution must be Idle and required components healthy.';
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
  if (snapshot === null) return ADMISSION_REASON_DISCONNECTED;
  if (published === null) {
    return 'The shared authoring workspace has not been read yet.';
  }
  if (!publicationIsCurrent(published, snapshot)) {
    return 'The displayed workspace is still synchronizing with the backend.';
  }
  if (snapshot.programCycle.state !== 'idle_after_reset') {
    return `Program execution is ${snapshot.programCycle.state.replaceAll('_', ' ')}; authoring requires Idle.`;
  }
  if (!snapshot.authoringAdmission.authoringAllowed) return ADMISSION_REASON_ALLOWED;
  return null;
}

export function programLocalFence(
  published: PublishedAuthoringWorkspace,
  snapshot: ComposerHeadlessState,
  requestOwnerEpoch: string,
  requestId: string,
): LocalWorkspaceFenceRequest {
  return {
    ...localDraftFence(published),
    expectedAdmissionGeneration: snapshot.authoringAdmission.admissionGeneration,
    requestOwnerEpoch,
    requestId,
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
export function withSharedDeploymentAuthoring(
  projection: DeploymentWorkspaceResponse,
  snapshot: AuthoringWorkspaceSnapshot,
): DeploymentWorkspaceResponse {
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
    },
  };
}

function deploymentRevisionOf(
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
