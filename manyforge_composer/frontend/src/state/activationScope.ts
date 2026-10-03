/**
 * The complete input a Deployment activation reads: the Deployment Draft and
 * also the current Program Draft, the Scenario's Program role assignment and
 * its source document. A confirmation captures all of it; the Apply is sent
 * only while it is unchanged, and the Composer backend rechecks it
 * (`expectedActivationScope`) before any effect.
 */
import type { JsonValue } from '../types/api';
import { deploymentRevisionOf, type PublishedAuthoringWorkspace } from './authoringWorkspace';

export interface DeploymentActivationScope {
  deploymentDraftId: string;
  deploymentDraftRevision: number;
  deploymentRevision: string | null;
  programDraftId: string;
  programDraftRevision: number;
  programRevision: string | null;
  sourceDocumentDigest: string | null;
  programRoleAssignment: Record<string, JsonValue> | null;
}

/** The scope of one installed publication (both Drafts and project facts). */
export function deploymentActivationScope(
  published: PublishedAuthoringWorkspace | null,
): DeploymentActivationScope | null {
  if (published === null) return null;
  const { identity, snapshot } = published;
  return {
    deploymentDraftId: identity.deploymentDraftId,
    deploymentDraftRevision: identity.deploymentDraftRevision,
    deploymentRevision: deploymentRevisionOf(snapshot.deployment.document),
    programDraftId: identity.programDraftId,
    programDraftRevision: identity.programDraftRevision,
    programRevision: snapshot.program.draft.programRevision ?? null,
    sourceDocumentDigest: snapshot.project.sourceDocumentDigest,
    programRoleAssignment: snapshot.project.programRoleAssignment,
  };
}

function canonical(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(canonical).join(',')}]`;
  if (value !== null && typeof value === 'object') {
    return `{${Object.keys(value).sort().map((key) => (
      `${JSON.stringify(key)}:${canonical((value as Record<string, unknown>)[key])}`
    )).join(',')}}`;
  }
  return JSON.stringify(value);
}

export function sameActivationScope(
  left: DeploymentActivationScope | null,
  right: DeploymentActivationScope | null,
): boolean {
  if (left === null || right === null) return false;
  return left.deploymentDraftId === right.deploymentDraftId
    && left.deploymentDraftRevision === right.deploymentDraftRevision
    && left.deploymentRevision === right.deploymentRevision
    && left.programDraftId === right.programDraftId
    && left.programDraftRevision === right.programDraftRevision
    && left.programRevision === right.programRevision
    && left.sourceDocumentDigest === right.sourceDocumentDigest
    && canonical(left.programRoleAssignment) === canonical(right.programRoleAssignment);
}

export const ACTIVATION_SCOPE_CHANGED = 'Draft changed after confirmation; review again.';
