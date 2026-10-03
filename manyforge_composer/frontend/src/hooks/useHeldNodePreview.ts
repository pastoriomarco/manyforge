import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { api } from '../api/client';
import { ROBOT_CONTROL_AUTHOR_DEBOUNCE_MS } from '../state/robotControl';
import {
  findProgramNode,
  heldNodeGhostSource,
  heldNodePreviewInputs,
  type HeldNodeGhostSource,
} from '../state/heldNodePreview';
import { humanizeReasonCode } from '../state/structuredError';
import { errorMessage } from '../state/structuralResource';
import type {
  CatalogEntry,
  ProgramFormulaReferences,
  ProgramNodeCorrectionPreview,
  ProgramNodeCorrectionPreviewRequest,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { DirectControlMotionStatus } from '../types/robotControl';
import type { ProgramRuntimeController } from './useProgramRuntime';
import type { ProgramWorkspaceController } from './useProgramWorkspace';
import { motionPreviewRejectionMessage } from './useRobotControl';

const NO_FORMULAS: ProgramFormulaReferences = { computedKeys: {}, nodes: {} };

function identity(prefix: string): string {
  const random = globalThis.crypto?.randomUUID?.()
    ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  return `${prefix}.${random}`;
}

/** The latest advisory preview of the held node's Draft edits (D4). */
export type HeldNodePreviewState =
  | { status: 'unavailable'; reason: string }
  | { status: 'pending'; previous: ProgramNodeCorrectionPreview | null }
  | { status: 'ready'; result: ProgramNodeCorrectionPreview }
  | { status: 'error'; message: string };

interface PreviewReply {
  key: string;
  result: ProgramNodeCorrectionPreview | null;
  error: string | null;
}

/** One short status line for the node's Properties editor. */
export function heldNodePreviewText(preview: HeldNodePreviewState): string {
  if (preview.status === 'unavailable') return `Preview unavailable: ${preview.reason}`;
  if (preview.status === 'pending') return 'Previewing the edited motion…';
  if (preview.status === 'error') return `Preview unavailable: ${preview.message}`;
  const { result } = preview;
  if (result.unavailableReason !== null) {
    return `Preview unavailable: ${result.unavailableReason.message || humanizeReasonCode(result.unavailableReason.code)}`;
  }
  if (result.preview?.ok === true) {
    return 'Reachable: trajectory planned and collision-checked from the current robot state (advisory; execution plans again).';
  }
  const error = result.preview?.error ?? null;
  return `Not reachable: ${motionPreviewRejectionMessage(error)
    ?? (error?.message || humanizeReasonCode(error?.code ?? 'preview_failed'))}`;
}

export interface HeldNodePreviewInputs {
  runtime: ProgramRuntimeController;
  workspace: ProgramWorkspaceController;
  snapshot: ComposerHeadlessState | null;
  catalogEntries: readonly CatalogEntry[];
  /** Every addressed motion target with its robot model descriptor. */
  motionTargets: readonly DirectControlMotionStatus[];
}

/**
 * While a run is held on a failed node and the Draft gives that node other
 * inputs than the run's captured Program, preview them with the node's own
 * motion builder and the manual-move native preview. One request in flight,
 * only the newest edit waits, late replies are ignored, and unrelated frames
 * or joint samples never restart planning. Advisory only: nothing reads it
 * to decide whether Apply, Reset or Retry may be sent.
 */
export function useHeldNodePreview({
  runtime,
  workspace,
  snapshot,
  catalogEntries,
  motionTargets,
}: HeldNodePreviewInputs) {
  const run = runtime.state.run;
  const offer = runtime.state.controls?.recoveryOffer ?? null;
  const applied = runtime.state.applied;
  const draft = workspace.state.workspace?.draft ?? null;
  const authoring = draft?.authoring ?? null;
  const formulas = draft?.formulaReferences ?? NO_FORMULAS;
  // A failed node held for intervention, captured by this exact run.
  const held = useMemo(() => (
    offer !== null && run !== null && applied !== null
      && offer.recoveryClass !== 'ordinary_operator_stop'
      && offer.programRunId === run.capture.programRunId
      && applied.program.program_revision === run.capture.programRevision
      ? { offer, run, program: applied.program }
      : null
  ), [applied, offer, run]);
  const nodeInstanceId = held?.offer.nodeInstanceId ?? null;
  const changes = useMemo(() => {
    if (held === null || authoring === null) return null;
    const node = findProgramNode([held.program.tree], held.offer.nodeInstanceId);
    const entry = node === null ? null
      : catalogEntries.find((candidate) => candidate.id === node.node_id) ?? null;
    return heldNodePreviewInputs(held.program, authoring, held.offer.nodeInstanceId, entry, formulas);
  }, [authoring, catalogEntries, formulas, held]);

  const [reply, setReply] = useState<PreviewReply | null>(null);
  const mounted = useRef(true);
  useEffect(() => {
    mounted.current = true;
    return () => { mounted.current = false; };
  }, []);

  const sceneRevision = snapshot?.executionControlStatus?.sceneRevision ?? null;
  // The native execution owner's epoch: Robot Control discards preview
  // evidence when that owner is replaced, so a reply from a previous owner is
  // never shown as ready for the current one.
  const executionOwnerEpoch = snapshot?.executionControlStatus?.ownerEpoch ?? null;
  const inputs = changes?.kind === 'inputs' ? changes.inputs : null;
  const requestKey = held === null || inputs === null
    ? null
    : JSON.stringify([
      held.run.ownerEpoch, executionOwnerEpoch, held.offer.programRunId, held.offer.nodeInstanceId,
      held.offer.nodeAttemptId, held.run.capture.programBindingId,
      held.run.capture.programRevision, sceneRevision, inputs,
    ]);
  const basis = useRef({ held, inputs });
  useEffect(() => {
    basis.current = { held, inputs };
  });
  // A browser abort does not cancel the owner's synchronous planning, so a
  // superseded request is never aborted and re-sent: at most one preview is in
  // flight, only the newest unsent edit waits, and it is sent once when the
  // in-flight one settles. A reply is kept only for the current edit.
  const wanted = useRef<string | null>(null);
  const inFlight = useRef<string | null>(null);
  const waiting = useRef<{ key: string; request: ProgramNodeCorrectionPreviewRequest } | null>(null);
  const request = useCallback((key: string, body: ProgramNodeCorrectionPreviewRequest) => {
    const send = (sentKey: string, sent: ProgramNodeCorrectionPreviewRequest) => {
      inFlight.current = sentKey;
      const settle = (next: PreviewReply) => {
        inFlight.current = null;
        if (!mounted.current) return;
        if (next.key === wanted.current) setReply(next);
        const queued = waiting.current;
        waiting.current = null;
        if (queued !== null && queued.key !== sentKey && queued.key === wanted.current) {
          send(queued.key, queued.request);
        }
      };
      api.programs.nodeCorrectionPreview(sent).then(
        (result) => settle({ key: sentKey, result, error: null }),
        (error: unknown) => settle({
          key: sentKey, result: null, error: errorMessage(error, 'Preview failed.'),
        }),
      );
    };
    if (inFlight.current !== null) {
      waiting.current = { key, request: body };
      return;
    }
    send(key, body);
  }, []);
  useEffect(() => {
    wanted.current = requestKey;
    const { held: current, inputs: proposed } = basis.current;
    if (requestKey === null || current === null || proposed === null) {
      waiting.current = null;
      return undefined;
    }
    const key = requestKey;
    const body: ProgramNodeCorrectionPreviewRequest = {
      schema_version: 'manyforge.program_node_correction_preview_request.v1',
      request_id: identity('node-correction-preview'),
      program_run_id: current.offer.programRunId,
      expected_owner_epoch: current.run.ownerEpoch,
      program_id: current.run.capture.programId,
      program_revision: current.run.capture.programRevision,
      program_binding_id: current.run.capture.programBindingId,
      node_instance_id: current.offer.nodeInstanceId,
      node_attempt_id: current.offer.nodeAttemptId,
      proposed_inputs: proposed,
    };
    // Coalesce edits before anything reaches the single in-flight slot.
    const timer = window.setTimeout(() => request(key, body), ROBOT_CONTROL_AUTHOR_DEBOUNCE_MS);
    return () => window.clearTimeout(timer);
  }, [request, requestKey]);

  // Null when there is nothing to preview: no held failed node, or the Draft
  // gives it the captured inputs.
  const preview: HeldNodePreviewState | null = changes === null || changes.kind === 'unchanged'
    ? null
    : changes.kind === 'unavailable'
      ? { status: 'unavailable', reason: changes.reason }
      : reply?.key !== requestKey
        ? { status: 'pending', previous: reply?.result ?? null }
        : reply.result !== null
          ? { status: 'ready', result: reply.result }
          : { status: 'error', message: reply.error ?? 'Preview failed.' };

  // The last reply stays visible, marked stale, until the current one arrives.
  const result = preview?.status === 'ready' ? preview.result
    : preview?.status === 'pending' ? preview.previous : null;
  const ghostSource = heldNodeGhostSource(result, applied?.programBinding ?? null, motionTargets);
  const ghost: (HeldNodeGhostSource & { stale: boolean }) | null = ghostSource !== null
    && 'ghost' in ghostSource
    ? { ...ghostSource.ghost, stale: preview?.status !== 'ready' }
    : null;
  const ghostUnavailableReason = ghostSource !== null && 'reason' in ghostSource
    ? ghostSource.reason
    : null;
  const statusText = preview === null
    ? null
    : `${heldNodePreviewText(preview)}${ghostUnavailableReason === null ? '' : ` No ghost is shown: ${ghostUnavailableReason}`}`;

  return { nodeInstanceId, preview, ghost, statusText };
}

export type HeldNodePreviewController = ReturnType<typeof useHeldNodePreview>;
