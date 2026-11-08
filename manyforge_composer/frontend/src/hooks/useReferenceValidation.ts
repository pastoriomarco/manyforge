import { useEffect, useRef, useState } from 'react';
import { api } from '../api/client';
import type { ProgramWorkspaceResponse } from '../types/api';
import { authoringDigest, referenceResult, type ReferenceIssue, type ReferenceSelectorFacts } from '../utils/referenceValidation';

export function useReferenceValidation(workspace: ProgramWorkspaceResponse | null) {
  const authoring = workspace?.draft.authoring ?? null;
  const authoringBody = authoring === null ? null : JSON.stringify(authoring);
  const generation = workspace?.authoringWorkspace;
  const generationKey = generation === undefined ? null : JSON.stringify(generation);
  const referenceCapture = useRef({ authoringBody, generationKey });
  useEffect(() => { referenceCapture.current = { authoringBody, generationKey }; });
  const [referenceSnapshot, setReferenceSnapshot] = useState<{
    authoringBody: string; generationKey: string;
    result: { issues: ReferenceIssue[]; total: number; truncated: boolean; selectorFacts: ReferenceSelectorFacts };
  } | null>(null);
  const referenceAttention = referenceSnapshot?.authoringBody === authoringBody
    && referenceSnapshot.generationKey === generationKey ? referenceSnapshot.result : null;
  const [referenceUnavailable, setReferenceUnavailable] = useState(false);
  useEffect(() => {
    setReferenceSnapshot(null);
    setReferenceUnavailable(false);
    if (authoringBody === null || generationKey === null) return;
    const abort = new AbortController();
    const timer = window.setTimeout(() => {
      void (async () => {
        const body: unknown = JSON.parse(authoringBody);
        const identity = JSON.parse(generationKey) as {
          workspaceEpoch: string; workspaceRevision: number; programDraftId: string;
          programDraftRevision: number; deploymentDraftId: string; deploymentDraftRevision: number;
        };
        const digest = await authoringDigest(body);
        const response = await api.programWorkspace.validate(body, abort.signal);
        const result = referenceResult(response, digest, {
          workspace_epoch: identity.workspaceEpoch, workspace_revision: identity.workspaceRevision,
          program_draft_id: identity.programDraftId, program_draft_revision: identity.programDraftRevision,
          deployment_draft_id: identity.deploymentDraftId, deployment_draft_revision: identity.deploymentDraftRevision,
        });
        if (!abort.signal.aborted && referenceCapture.current.authoringBody === authoringBody
          && referenceCapture.current.generationKey === generationKey) setReferenceSnapshot({ authoringBody, generationKey, result });
      })().catch(() => { if (!abort.signal.aborted && referenceCapture.current.authoringBody === authoringBody
        && referenceCapture.current.generationKey === generationKey) setReferenceUnavailable(true); });
    }, 250);
    return () => { window.clearTimeout(timer); abort.abort(); };
  }, [authoringBody, generationKey]);
  const selectorFacts = referenceAttention?.selectorFacts ?? null;
  return { referenceAttention, referenceUnavailable, selectorFacts };
}

export type ReferenceValidationState = ReturnType<typeof useReferenceValidation>;
