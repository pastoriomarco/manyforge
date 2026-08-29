import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { ApiError, api } from '../api/client';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type {
  AssistantModeIndex,
  AssistantProviderStatus,
} from '../types/assistant';
import type { JsonValue } from '../types/api';
import {
  reconcileAssistantRequest,
  shouldReconcileAssistantRequest,
} from './assistantRequestRecovery';
import { useOperationalErrorMirror } from '../state/operationalErrors';

const ORIENTATION_FIELDS = [
  'orientation_from',
  'rotate_local_rpy_deg',
  'rotate_world_rpy_deg',
  'rpy_deg',
  'rpy_delta_deg',
  'xyzw',
] as const;
const MAXIMUM_TRANSCRIPT_ITEMS = 200;
const MAXIMUM_PRESENTED_TOOL_RESULTS = 128;

interface TranscriptResponse {
  requestId: string;
  draftMutated: boolean;
  proposals: Array<{
    toolId: string;
    effect: string;
    arguments: Record<string, JsonValue>;
    decision: 'pending' | 'materializing' | 'materialized' | 'rejected' | 'failed';
  }>;
  toolCalls: Array<{
    toolId: string;
    status: string;
    draftMutated: boolean;
    resultSummary?: string;
  }>;
  warnings: string[];
}

interface TranscriptItem {
  id: string;
  role: 'user' | 'assistant';
  mode: string;
  text: string;
  response?: TranscriptResponse;
  error?: string;
}

interface RetryIntent {
  message: string;
  mode: string;
}

function identity(prefix: string): string {
  const suffix = globalThis.crypto?.randomUUID?.()
    ?? `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
  return `${prefix}.${suffix}`;
}

function terminal(status: AssistantProviderStatus): boolean {
  return status.state !== 'running';
}

export function assistantFailureText(status: AssistantProviderStatus): string {
  if (status.error) {
    const { code, message } = status.error;
    if (code.includes('timeout')) {
      return `Assistant timed out: ${message} Try a narrower request or retry when the model is ready.`;
    }
    if (code.includes('projection') || code.includes('catalog')) {
      return `Assistant configuration changed: ${message} Refresh mode discovery before retrying.`;
    }
    if (code.includes('provider') || code.includes('upstream')) {
      return `Assistant provider unavailable: ${message} Check the configured bridge/model service before retrying.`;
    }
    if (code.includes('tool') || code === 'response_rejected') {
      return `Assistant tool request rejected: ${message} Check the selected mode and its available tools.`;
    }
    return `${code}: ${message}`;
  }
  if (status.state === 'cancelled') return 'The assistant request was cancelled.';
  return 'The assistant request failed without a usable result.';
}

export function summarizeAssistantToolResult(value: unknown): string | undefined {
  if (value === undefined) return undefined;
  if (typeof value === 'string') return value.slice(0, 1_024);
  if (value && typeof value === 'object' && !Array.isArray(value)) {
    const record = value as Record<string, unknown>;
    const message = typeof record.message === 'string' ? record.message : null;
    const kind = typeof record.kind === 'string' ? record.kind : null;
    if (message) return `${kind ? `${kind}: ` : ''}${message}`.slice(0, 1_024);
  }
  try {
    return JSON.stringify(value).slice(0, 1_024);
  } catch {
    return 'Structured result could not be summarized.';
  }
}

function appendTranscript(
  items: TranscriptItem[],
  item: TranscriptItem,
): TranscriptItem[] {
  return [...items, item].slice(-MAXIMUM_TRANSCRIPT_ITEMS);
}

function presentationResponse(
  status: AssistantProviderStatus,
): TranscriptResponse | undefined {
  const response = status.response;
  if (!response) return undefined;
  return {
    requestId: status.requestId,
    draftMutated: response.draftMutated,
    proposals: response.proposals
      .slice(0, MAXIMUM_PRESENTED_TOOL_RESULTS)
      .map((proposal) => ({
        toolId: proposal.toolId,
        effect: proposal.effect,
        arguments: proposal.arguments,
        decision: 'pending',
      })),
    toolCalls: response.toolCalls
      .slice(0, MAXIMUM_PRESENTED_TOOL_RESULTS)
      .map((call) => ({
        toolId: call.toolId,
        status: call.status,
        draftMutated: call.draftMutated === true,
        ...(call.result === undefined
          ? {}
          : { resultSummary: summarizeAssistantToolResult(call.result) }),
      })),
    warnings: response.warnings.slice(0, 32).map((warning) => warning.slice(0, 1_024)),
  };
}

export function AssistantOverlay({
  enabled,
  headlessConnected,
  programController,
}: {
  enabled: boolean;
  headlessConnected: boolean;
  programController: ProgramWorkspaceController;
}) {
  const ownerEpoch = useRef(identity('composer.page'));
  const recovery = useRef<AbortController | null>(null);
  const modeDiscovery = useRef<AbortController | null>(null);
  const [modeIndex, setModeIndex] = useState<AssistantModeIndex | null>(null);
  const [modeError, setModeError] = useState<string | null>(null);
  const [selectedMode, setSelectedMode] = useState('');
  const [conversationId, setConversationId] = useState(() => identity('conversation'));
  const [input, setInput] = useState('');
  const [transcript, setTranscript] = useState<TranscriptItem[]>([]);
  const [active, setActive] = useState<AssistantProviderStatus | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [operationalError, setOperationalError] = useState<string | null>(null);
  const [retryIntent, setRetryIntent] = useState<RetryIntent | null>(null);
  const [orientationFields, setOrientationFields] = useState<string[]>([
    ...ORIENTATION_FIELDS,
  ]);
  useOperationalErrorMirror({
    source: 'composer.assistant',
    title: 'Assistant request or response failed',
    message: operationalError ?? modeError,
    onDismiss: () => {
      setOperationalError(null);
      setModeError(null);
    },
  });

  const loadModes = useCallback(() => {
    modeDiscovery.current?.abort();
    if (!enabled) {
      setModeIndex(null);
      modeDiscovery.current = null;
      return;
    }
    const controller = new AbortController();
    modeDiscovery.current = controller;
    setModeError(null);
    void api.assistant.modes(controller.signal)
      .then((value) => setModeIndex(value))
      .catch((error: unknown) => {
        if (!controller.signal.aborted) {
          setModeError(error instanceof Error ? error.message : 'Mode discovery failed.');
          setModeIndex(null);
        }
      })
      .finally(() => {
        if (modeDiscovery.current === controller) modeDiscovery.current = null;
      });
  }, [enabled]);

  useEffect(() => {
    loadModes();
    return () => modeDiscovery.current?.abort();
  }, [loadModes]);

  useEffect(() => {
    const modes = modeIndex?.modes ?? [];
    if (modes.some((mode) => mode.mode === selectedMode)) return;
    const preferred = modes.find((mode) => mode.mode === 'composer-assistant')
      ?? modes.find((mode) => mode.mode === 'query')
      ?? modes[0];
    setSelectedMode(preferred?.mode ?? '');
  }, [modeIndex, selectedMode]);

  useEffect(() => () => recovery.current?.abort(), []);

  const busy = active?.state === 'running';
  const selected = modeIndex?.modes.find((mode) => mode.mode === selectedMode);
  const canSend = enabled && !!selected && !busy && input.trim().length > 0;
  const orientationSet = useMemo(() => new Set(orientationFields), [orientationFields]);

  const finish = useCallback(async (status: AssistantProviderStatus) => {
    setActive(status);
    if (status.state === 'completed' && status.response) {
      setOperationalError(null);
      const response = status.response;
      setTranscript((items) => appendTranscript(items, {
        id: `${status.requestId}.assistant`,
        role: 'assistant',
        mode: status.assistantMode,
        text: response.message,
        response: presentationResponse(status),
      }));
      setRetryIntent(null);
      setNotice(
        response.draftMutated
          ? 'Assistant changes are visible in the in-memory Draft and remain unsaved/unapplied.'
          : null,
      );
      if (response.draftMutated) await programController.refresh();
      return;
    }
    const detail = assistantFailureText(status);
    setOperationalError(detail);
    setTranscript((items) => appendTranscript(items, {
      id: `${status.requestId}.error`,
      role: 'assistant',
      mode: status.assistantMode,
      text: '',
      error: detail,
    }));
    setNotice(detail);
  }, [programController]);

  const reconcile = useCallback(async (requestId: string) => {
    recovery.current?.abort();
    const controller = new AbortController();
    recovery.current = controller;
    const result = await reconcileAssistantRequest({
      readStatus: (signal) => api.assistant.status(requestId, signal),
      isTerminal: terminal,
      signal: controller.signal,
      retryDelayMs: 500,
      maxNotFoundAttempts: 4,
      maxReadFailures: 4,
      maxPollAttempts: 360,
    });
    if (recovery.current !== controller || result.kind === 'aborted') return;
    recovery.current = null;
    if (result.kind === 'terminal') {
      await finish(result.status);
      return;
    }
    const detail = result.kind === 'not_found'
        ? `No bridge record was found for ${requestId}; recheck before retrying.`
        : result.kind === 'unreachable'
          ? `The bridge status is unreachable: ${result.detail}`
          : 'The assistant result is still unknown; recheck before retrying.';
    setNotice(detail);
    setOperationalError(detail);
  }, [finish]);

  const send = useCallback(async (intent?: RetryIntent) => {
    const message = (intent?.message ?? input).trim();
    const mode = intent?.mode ?? selectedMode;
    if (!message || !mode || busy) return;
    const requestId = identity('assistant.request');
    const request: Parameters<typeof api.assistant.submit>[0] = {
      requestId,
      requestOwnerEpoch: ownerEpoch.current,
      sourceLabel: 'composer_ui',
      correlationId: null,
      conversationId,
      assistantMode: mode,
      message,
      requestedTools: [],
      fieldPolicy: { allowedOrientationFields: [...orientationFields].sort() },
    };
    setInput('');
    setNotice(null);
    setOperationalError(null);
    setRetryIntent({ message, mode });
    setTranscript((items) => appendTranscript(
      items,
      { id: `${requestId}.user`, role: 'user', mode, text: message },
    ));
    setActive({
      requestId,
      requestOwnerEpoch: ownerEpoch.current,
      sourceLabel: 'composer_ui',
      correlationId: null,
      conversationId,
      assistantMode: mode,
      state: 'running',
      response: null,
      error: null,
    });
    try {
      const accepted = await api.assistant.submit(request);
      setActive(accepted);
      if (terminal(accepted)) await finish(accepted);
      else await reconcile(requestId);
    } catch (error) {
      if (shouldReconcileAssistantRequest(error)) {
        setNotice(`Submission was interrupted; reconciling ${requestId} before any retry.`);
        await reconcile(requestId);
        return;
      }
      const detail = error instanceof Error ? error.message : 'Assistant request failed.';
      const failed: AssistantProviderStatus = {
        requestId: request.requestId,
        requestOwnerEpoch: request.requestOwnerEpoch,
        sourceLabel: request.sourceLabel,
        correlationId: request.correlationId,
        conversationId: request.conversationId,
        assistantMode: request.assistantMode,
        state: 'failed',
        response: null,
        error: { code: error instanceof ApiError ? `http_${error.status}` : 'request_failed', message: detail },
      };
      await finish(failed);
    }
  }, [busy, conversationId, finish, input, orientationFields, reconcile, selectedMode]);

  const cancel = async () => {
    if (!active || active.state !== 'running') return;
    try {
      const status = await api.assistant.cancel(active.requestId);
      recovery.current?.abort();
      recovery.current = null;
      await finish(status);
    } catch (error) {
      const detail = error instanceof Error ? error.message : 'Cancellation failed.';
      setNotice(detail);
      setOperationalError(detail);
    }
  };

  const newConversation = () => {
    if (busy) return;
    recovery.current?.abort();
    recovery.current = null;
    setConversationId(identity('conversation'));
    setTranscript([]);
    setActive(null);
    setNotice(null);
    setOperationalError(null);
    setRetryIntent(null);
  };

  const toggleOrientation = (field: string) => {
    if (busy) return;
    const next = new Set(orientationFields);
    if (next.has(field)) {
      if (next.size === 1) return;
      next.delete(field);
    } else {
      next.add(field);
    }
    setOrientationFields([...next].sort());
  };

  const setProposalDecision = useCallback((
    transcriptId: string,
    proposalIndex: number,
    decision: TranscriptResponse['proposals'][number]['decision'],
  ) => {
    setTranscript((items) => items.map((item) => {
      if (item.id !== transcriptId || item.response === undefined) return item;
      return {
        ...item,
        response: {
          ...item.response,
          proposals: item.response.proposals.map((proposal, index) => (
            index === proposalIndex ? { ...proposal, decision } : proposal
          )),
        },
      };
    }));
  }, []);

  const materializeProposal = useCallback(async (
    transcriptId: string,
    requestId: string,
    proposalIndex: number,
  ) => {
    if (!programController.mutationAvailable) {
      setNotice(
        programController.mutationUnavailableReason
          ?? 'Program authoring is unavailable.',
      );
      return;
    }
    setProposalDecision(transcriptId, proposalIndex, 'materializing');
    const result = await programController.mutate(
      'assistant_draft_materialize_proposal',
      { request_id: requestId, proposal_index: proposalIndex },
    );
    setProposalDecision(
      transcriptId,
      proposalIndex,
      result === null ? 'failed' : 'materialized',
    );
    setNotice(result === null
      ? 'The proposal was not materialized; review the Program authoring error.'
      : 'Proposal materialized as one atomic Draft change; Saved and Applied are unchanged.');
  }, [programController, setProposalDecision]);

  return (
    <section className="panel assistant-panel" aria-labelledby="assistant-heading">
      <header className="panel-header assistant-header">
        <div>
          <p className="eyebrow">Bounded assistant</p>
          <h2 id="assistant-heading">Conversation</h2>
        </div>
        <button type="button" className="quiet-button" disabled={busy} onClick={newConversation}>
          New conversation
        </button>
      </header>

      {!headlessConnected && (
        <p className="panel-intro">Connect the headless owner to discover enabled assistant modes.</p>
      )}
      {headlessConnected && !enabled && (
        <p className="panel-intro">Configure the assistant bridge to start bounded conversations.</p>
      )}
      {modeError && (
        <div className="assistant-error" role="alert">
          <span>{modeError}</span>
          <button type="button" onClick={() => loadModes()}>Retry discovery</button>
        </div>
      )}
      {enabled && !modeError && !modeIndex && <p className="panel-intro">Loading owner-issued modes…</p>}

      {modeIndex && (
        <>
          <label className="assistant-mode-select">
            Mode
            <select
              value={selectedMode}
              disabled={busy}
              onChange={(event) => setSelectedMode(event.target.value)}
            >
              {modeIndex.modes.map((mode) => (
                <option key={mode.mode} value={mode.mode}>{mode.mode}</option>
              ))}
            </select>
          </label>
          {selected && <p className="assistant-mode-description">{selected.description}</p>}

          <details className="assistant-field-policy">
            <summary>Pose fields visible to this request</summary>
            <div>
              {ORIENTATION_FIELDS.map((field) => (
                <label key={field}>
                  <input
                    type="checkbox"
                    checked={orientationSet.has(field)}
                    disabled={busy || (orientationSet.has(field) && orientationSet.size === 1)}
                    onChange={() => toggleOrientation(field)}
                  />
                  <code>{field}</code>
                </label>
              ))}
            </div>
          </details>

          <div className="assistant-transcript" aria-live="polite">
            {transcript.length === 0 && (
              <p className="panel-intro">Messages stay separate from Draft, Saved, and Applied history.</p>
            )}
            {transcript.map((item) => (
              <article key={item.id} className={`assistant-message ${item.role}`}>
                <header>
                  <strong>{item.role === 'user' ? 'You' : 'Assistant'}</strong>
                  <code>{item.mode}</code>
                </header>
                {item.text && <p>{item.text}</p>}
                {item.error && <p className="assistant-error" role="alert">{item.error}</p>}
                {item.response?.draftMutated && (
                  <p className="assistant-draft-provenance">Draft changed by assistant; Saved and Applied were not changed.</p>
                )}
                {item.response && item.response.toolCalls.length > 0 && (
                  <details>
                    <summary>{item.response.toolCalls.length} tool result(s)</summary>
                    <ul>
                      {item.response.toolCalls.map((call, index) => (
                        <li key={`${call.toolId}.${index}`}>
                          <code>{call.toolId}</code> — {call.status}
                          {call.draftMutated ? ' (Draft changed)' : ''}
                          {call.resultSummary ? `: ${call.resultSummary}` : ''}
                        </li>
                      ))}
                    </ul>
                  </details>
                )}
                {item.response && item.response.proposals.length > 0 && (
                  <details>
                    <summary>{item.response.proposals.length} reviewable Draft proposal(s)</summary>
                    <ul className="assistant-proposals">
                      {item.response.proposals.map((proposal, index) => (
                        <li key={`${proposal.toolId}.${index}`}>
                          <div><code>{proposal.toolId}</code> — {proposal.effect}</div>
                          <details>
                            <summary>Review proposed Draft input</summary>
                            <pre>{JSON.stringify(proposal.arguments, null, 2)}</pre>
                          </details>
                          <div className="assistant-proposal-actions">
                            <button
                              type="button"
                              disabled={
                                proposal.decision !== 'pending'
                                || !programController.mutationAvailable
                              }
                              title={programController.mutationAvailable
                                ? 'Materialize this proposal into the current Draft.'
                                : programController.mutationUnavailableReason ?? undefined}
                              onClick={() => void materializeProposal(
                                item.id,
                                item.response?.requestId ?? '',
                                index,
                              )}
                            >Materialize</button>
                            <button
                              type="button"
                              className="quiet-button"
                              disabled={proposal.decision !== 'pending'}
                              onClick={() => setProposalDecision(item.id, index, 'rejected')}
                            >Reject</button>
                            {proposal.decision !== 'pending' && (
                              <span>{proposal.decision}</span>
                            )}
                          </div>
                        </li>
                      ))}
                    </ul>
                  </details>
                )}
                {item.response && item.response.warnings.length > 0 && (
                  <ul className="assistant-warnings">
                    {item.response.warnings.map((warning, index) => (
                      <li key={`${item.id}.warning.${index}`}>{warning}</li>
                    ))}
                  </ul>
                )}
              </article>
            ))}
          </div>

          <label className="assistant-input">
            Message for <code>{selectedMode}</code>
            <textarea
              value={input}
              disabled={busy}
              maxLength={65_536}
              rows={3}
              onChange={(event) => setInput(event.target.value)}
            />
          </label>
          <div className="assistant-actions">
            <button type="button" disabled={!canSend} onClick={() => void send()}>Send</button>
            <button type="button" className="danger-button" disabled={!busy} onClick={() => void cancel()}>Cancel</button>
            <button
              type="button"
              className="quiet-button"
              disabled={busy || !retryIntent || active?.state === 'completed'}
              onClick={() => void send(retryIntent ?? undefined)}
            >
              Retry as a new request
            </button>
            {active && active.state !== 'completed' && active.state !== 'cancelled' && (
              <button type="button" className="quiet-button" onClick={() => void reconcile(active.requestId)}>
                Recheck outcome
              </button>
            )}
          </div>
          {active && <p className="assistant-status">Request <code>{active.requestId}</code>: {active.state}</p>}
          {notice && <p className="assistant-notice" role="status">{notice}</p>}
        </>
      )}
    </section>
  );
}
