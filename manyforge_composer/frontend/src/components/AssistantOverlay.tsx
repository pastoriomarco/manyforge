import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { ApiError, api } from '../api/client';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type {
  AssistantModeIndex,
  AssistantProviderStatus,
} from '../types/assistant';
import type { StructuralResourceState } from '../state/structuralResource';
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
/** The only chat mode: edits land in the Program Draft via the tool callback. */
const ASSISTANT_MODE = 'composer-assistant';
export const NO_AI_MODEL_COMPONENT = 'No AI model component in this Deployment';
export const NO_ASSISTANT_CHAT_MODE = "This Deployment's assistant profile has no chat mode";

export interface AssistantAvailability {
  /** Why the AI tab is disabled, or null when it can be opened. */
  unavailableReason: string | null;
  modeIndex: AssistantModeIndex | null;
  modeError: string | null;
}

/** The AI tab opens only for a Deployment with an assistant profile and an AI model component. */
export function assistantAvailability(
  modes: StructuralResourceState<AssistantModeIndex>,
): AssistantAvailability {
  switch (modes.status) {
    case 'loading':
      return { unavailableReason: 'Loading assistant modes…', modeIndex: null, modeError: null };
    case 'absent':
      return { unavailableReason: modes.message, modeIndex: null, modeError: null };
    case 'error':
      return { unavailableReason: null, modeIndex: null, modeError: modes.message };
    case 'ready':
      if (modes.data.aiModelComponentId === null) {
        return { unavailableReason: NO_AI_MODEL_COMPONENT, modeIndex: null, modeError: null };
      }
      if (!modes.data.modes.some((mode) => mode.mode === ASSISTANT_MODE)) {
        return { unavailableReason: NO_ASSISTANT_CHAT_MODE, modeIndex: null, modeError: null };
      }
      return { unavailableReason: null, modeIndex: modes.data, modeError: null };
  }
}

interface TranscriptResponse {
  requestId: string;
  draftMutated: boolean;
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
  text: string;
  response?: TranscriptResponse;
  error?: string;
}

interface RetryIntent {
  message: string;
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
    if (code === 'assistant_ai_model_absent') return NO_AI_MODEL_COMPONENT;
    if (code === 'ai_model_token_unavailable') {
      return `AI model token unavailable: ${message} Set it in the environment of the ManyForge services.`;
    }
    if (code.includes('timeout')) {
      return `Assistant timed out: ${message} Try a narrower request or retry when the model is ready.`;
    }
    if (code.includes('projection') || code.includes('catalog')) {
      return `Assistant configuration changed: ${message} Refresh mode discovery before retrying.`;
    }
    if (code.includes('provider') || code.includes('upstream')) {
      return `Assistant provider unavailable: ${message} Check the AI model server of the Deployment before retrying.`;
    }
    if (code.includes('tool') || code === 'response_rejected') {
      return `Assistant tool request rejected: ${message} Check the tools available to the assistant.`;
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
  unavailableReason,
  modeIndex,
  modeError,
  onRetryModes,
  programController,
}: {
  unavailableReason: string | null;
  modeIndex: AssistantModeIndex | null;
  modeError: string | null;
  onRetryModes: () => void;
  programController: ProgramWorkspaceController;
}) {
  const ownerEpoch = useRef(identity('composer.page'));
  const recovery = useRef<AbortController | null>(null);
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
    message: operationalError,
    onDismiss: () => setOperationalError(null),
  });

  useEffect(() => () => recovery.current?.abort(), []);

  const busy = active?.state === 'running';
  const selected = modeIndex?.modes.find((mode) => mode.mode === ASSISTANT_MODE);
  const canSend = !!selected && !busy && input.trim().length > 0;
  const orientationSet = useMemo(() => new Set(orientationFields), [orientationFields]);

  const finish = useCallback(async (status: AssistantProviderStatus) => {
    setActive(status);
    if (status.state === 'completed' && status.response) {
      setOperationalError(null);
      const response = status.response;
      setTranscript((items) => appendTranscript(items, {
        id: `${status.requestId}.assistant`,
        role: 'assistant',
        text: response.message,
        response: presentationResponse(status),
      }));
      setRetryIntent(null);
      setNotice(
        response.draftMutated
          ? 'Assistant changes are in the Draft. Apply them, or Discard the Draft to undo them.'
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
    const mode = ASSISTANT_MODE;
    if (!message || busy) return;
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
    setRetryIntent({ message });
    setTranscript((items) => appendTranscript(
      items,
      { id: `${requestId}.user`, role: 'user', text: message },
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
  }, [busy, conversationId, finish, input, orientationFields, reconcile]);

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

      {unavailableReason && <p className="panel-intro">{unavailableReason}</p>}
      {modeError && (
        <div className="notice severity-error" role="alert">
          <span>{modeError}</span>
          <button type="button" onClick={onRetryModes}>Retry discovery</button>
        </div>
      )}

      {selected && (
        <>
          <p className="assistant-mode-description">{selected.description}</p>

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
              <p className="panel-intro">Assistant edits land in the Draft. Apply them, or Discard the Draft, as for your own edits.</p>
            )}
            {transcript.map((item) => (
              <article key={item.id} className={`assistant-message ${item.role}`}>
                <header>
                  <strong>{item.role === 'user' ? 'You' : 'Assistant'}</strong>
                </header>
                {item.text && <p>{item.text}</p>}
                {item.error && <p className="notice severity-error" role="alert">{item.error}</p>}
                {item.response?.draftMutated && (
                  <p className="assistant-draft-provenance">Draft changed by the assistant; not applied yet.</p>
                )}
                {item.response && item.response.toolCalls.length > 0 && (
                  <details>
                    <summary>{item.response.toolCalls.length} tool result(s)</summary>
                    <ul>
                      {item.response.toolCalls.map((call, index) => (
                        <li key={`${call.toolId}.${index}`}>
                          <code>{call.toolId}</code> · {call.status}
                          {call.draftMutated ? ' (Draft changed)' : ''}
                          {call.resultSummary ? `: ${call.resultSummary}` : ''}
                        </li>
                      ))}
                    </ul>
                  </details>
                )}
                {item.response && item.response.warnings.length > 0 && (
                  <ul className="inline-message severity-warning">
                    {item.response.warnings.map((warning, index) => (
                      <li key={`${item.id}.warning.${index}`}>{warning}</li>
                    ))}
                  </ul>
                )}
              </article>
            ))}
          </div>

          <label className="assistant-input">
            Message
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
          {notice && <p className="notice severity-info" role="status">{notice}</p>}
        </>
      )}
    </section>
  );
}
