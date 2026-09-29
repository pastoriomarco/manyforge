import type { DeploymentWorkspaceResponse, ProgramWorkspaceResponse } from '../types/api';

/**
 * Draft / Saved / Applied state of the Program and of the Deployment, shown
 * as the D / S / A letters beside the workspace switcher. The Scene is part
 * of the Program document, so Scene edits are Program changes.
 */
export type DocumentStateName =
  | 'draft' | 'modified' | 'incomplete'
  | 'saved' | 'different' | 'unsaved'
  | 'applied' | 'unavailable';

export interface DocumentStateLetter {
  letter: 'D' | 'S' | 'A';
  state: DocumentStateName;
  title: string;
}

export interface DocumentStates {
  letters: [DocumentStateLetter, DocumentStateLetter, DocumentStateLetter];
  /** At least one letter differs (amber): the letters are shown. */
  pending: boolean;
}

const MATCHING: ReadonlySet<DocumentStateName> = new Set(['draft', 'saved', 'applied']);

function states(letters: DocumentStates['letters']): DocumentStates {
  return { letters, pending: letters.some((letter) => !MATCHING.has(letter.state)) };
}

export function programDocumentStates(
  workspace: ProgramWorkspaceResponse | null,
): DocumentStates {
  const authoring = workspace?.draft.authoring ?? null;
  const incomplete = authoring !== null && workspace?.draft.program === null;
  const draft: DocumentStateName = incomplete
    ? 'incomplete'
    : workspace?.hasUnsavedChanges ? 'modified' : 'draft';
  const saved: DocumentStateName = workspace?.saved.available
    ? workspace.saved.matchesDraft ? 'saved' : 'different'
    : 'unsaved';
  const applied: DocumentStateName = workspace?.applied.available
    ? workspace.applied.matchesDraft ? 'applied' : 'different'
    : 'unavailable';
  const path = workspace?.saved.path ?? null;
  return states([
    {
      letter: 'D',
      state: draft,
      title: draft === 'incomplete'
        ? `Program Draft: incomplete. ${workspace?.draft.diagnostics[0]?.message ?? 'Complete it before Save or Apply.'}`
        : draft === 'modified'
          ? 'Program Draft: modified (Scene edits included). Your edits are not saved to the file; Save writes them to the Scenario.'
          : 'Program Draft: no unsaved edits.',
    },
    {
      letter: 'S',
      state: saved,
      title: saved === 'saved'
        ? `Program Saved: the file matches the Draft.${path ? ` File: ${path}` : ''}`
        : saved === 'different'
          ? `Program Saved: the file differs from the Draft. Save to update it.${path ? ` File: ${path}` : ''}`
          : 'Program Saved: not saved to a file yet.',
    },
    {
      letter: 'A',
      state: applied,
      title: applied === 'applied'
        ? 'Program Applied: the running Program is your Draft.'
        : applied === 'different'
          ? 'Program Applied: the running Program is not your Draft (Scene edits included). Apply to run your changes, or Discard to return to the applied Program. Execute is blocked until then.'
          : `Program Applied: unavailable. ${workspace?.applied.unavailableReason?.message ?? 'No Program is applied.'}`,
    },
  ]);
}

/**
 * The Deployment Draft is the one the Manager applied. The publication
 * identity (revision) decides when both sides carry one, so the answer does
 * not flicker while an accepted Apply settles. Scenario Deployments need not
 * carry a revision field; then the backend's document comparison decides.
 */
export function deploymentMatchesApplied(workspace: DeploymentWorkspaceResponse | null): boolean {
  if (workspace === null || workspace.draft.document === null) return false;
  const draft = workspace.draft.deploymentRevision;
  const applied = workspace.managerApplied.deploymentRevision;
  if (draft !== null && applied !== null) return draft === applied;
  return workspace.runAdmission.ready;
}

export function deploymentActionUnavailableReason(
  workspace: DeploymentWorkspaceResponse | null,
  action: 'apply' | 'revert' | 'discard',
  commonReason: string | null,
): string | null {
  if (commonReason !== null) return commonReason;
  if (workspace === null) return 'Deployment workspace is not loaded.';
  if (action === 'discard') return deploymentUnapplied(workspace)
    ? null : 'Draft already matches Manager Applied.';
  if (action === 'revert') return workspace.savedScenarioCandidates.length === 0
    ? 'No saved Deployment is available.'
    : !workspace.draft.differsFromSaved
      && workspace.saved.deploymentRevision === workspace.managerApplied.deploymentRevision
      ? 'Saved already matches Draft and Applied.' : null;
  return !workspace.draft.document ? 'No Deployment Draft is loaded.'
    : deploymentMatchesApplied(workspace) ? 'Draft already matches Manager Applied.' : null;
}

/** Something is applied and the Deployment Draft is not it. */
export function deploymentUnapplied(workspace: DeploymentWorkspaceResponse | null): boolean {
  return workspace !== null
    && workspace.draft.document !== null
    && workspace.managerApplied.deploymentRevision !== null
    && !deploymentMatchesApplied(workspace);
}

export function deploymentDocumentStates(
  workspace: DeploymentWorkspaceResponse | null,
): DocumentStates {
  const differsFromSaved = workspace?.draft.differsFromSaved === true;
  const matchesApplied = deploymentMatchesApplied(workspace);
  const path = workspace?.saved.path ?? null;
  return states([
    {
      letter: 'D',
      state: differsFromSaved ? 'modified' : 'draft',
      title: differsFromSaved
        ? 'Deployment Draft: modified. Your edits are not saved to the file; Save writes them to the Scenario.'
        : 'Deployment Draft: no unsaved edits.',
    },
    {
      letter: 'S',
      state: differsFromSaved ? 'different' : 'saved',
      title: differsFromSaved
        ? `Deployment Saved: the file differs from the Draft. Save to update it.${path ? ` File: ${path}` : ''}`
        : `Deployment Saved: the file matches the Draft.${path ? ` File: ${path}` : ''}`,
    },
    {
      letter: 'A',
      state: matchesApplied ? 'applied' : 'different',
      title: matchesApplied
        ? 'Deployment Applied: the running Deployment is your Draft.'
        : 'Deployment Applied: the running Deployment is not your Draft. Apply it in the Deployment workspace, or Discard. Execute is blocked until then.',
    },
  ]);
}
