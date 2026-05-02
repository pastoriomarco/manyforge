import { useEffect, useMemo, useRef, useState } from 'react';
import type { KeyboardEvent } from 'react';
import { api } from '../api/client';
import { useCycleStore } from '../store/cycleStore';
import { useDeploymentStore } from '../store/deploymentStore';
import { useProgramStore } from '../store/programStore';
import type {
  AssistantChatResponse,
  AssistantModeDefinition,
  AssistantProviderCatalog,
  AssistantRequestStatus,
  AssistantToolCall,
  AssistantToolDefinition,
} from '../types/api';

type TranscriptItem =
  | {
      kind: 'user';
      id: string;
      text: string;
      modeId: string;
    }
  | {
      kind: 'assistant';
      id: string;
      text: string;
      response?: AssistantChatResponse;
      error?: string;
    };

type AssistantRunState = 'idle' | 'running' | 'cancel_requested';

const EMPTY_ASSISTANT_MODES: Record<string, AssistantModeDefinition> = {};
const EMPTY_TOOL_CATALOG: AssistantToolDefinition[] = [];
const NS_PER_MS = 1_000_000;
const NS_PER_S = 1_000_000_000;

function createRequestId(): string {
  const suffix = Math.random().toString(16).slice(2, 8);
  return `composer-ui-${Date.now()}-${suffix}`;
}

function modeLabel(modeId: string): string {
  return modeId
    .split('-')
    .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
    .join(' ');
}

function modeEntryStatus(modeId: string, cycleState: string): { allowed: boolean; detail: string } {
  if (modeId === 'error-recovery') {
    return cycleState === 'stopped'
      ? { allowed: true, detail: 'Ready while stopped' }
      : { allowed: false, detail: 'Requires stopped execution' };
  }
  return cycleState === 'idle'
    ? { allowed: true, detail: 'Ready while idle' }
    : { allowed: false, detail: 'Requires idle execution' };
}

function summarizeMode(mode: AssistantModeDefinition | undefined, tools: AssistantToolDefinition[]): string {
  if (!mode) {
    return 'No active assistant mode';
  }
  const toolIds = new Set(mode.catalog?.tools ?? []);
  const effects = tools
    .filter((tool) => toolIds.has(tool.id))
    .reduce<Record<string, number>>((counts, tool) => {
      counts[tool.effect] = (counts[tool.effect] ?? 0) + 1;
      return counts;
    }, {});
  const summary = Object.entries(effects)
    .sort(([left], [right]) => left.localeCompare(right))
    .map(([effect, count]) => `${effect}: ${count}`)
    .join(' / ');
  return summary || `${toolIds.size} tool(s)`;
}

function statusTone(status: string): string {
  if (status === 'completed') {
    return 'bg-emerald-500';
  }
  if (status === 'failed' || status === 'cancelled') {
    return 'bg-red-500';
  }
  if (status === 'running' || status === 'cancel_requested') {
    return 'bg-blue-400';
  }
  return 'bg-gray-500';
}

function toolStatusClasses(status: AssistantToolCall['status']): string {
  if (status === 'completed') {
    return 'border-emerald-700/70 bg-emerald-950/40 text-emerald-100';
  }
  if (status === 'failed') {
    return 'border-red-700/70 bg-red-950/40 text-red-100';
  }
  if (status === 'skipped') {
    return 'border-gray-700 bg-gray-900/70 text-gray-300';
  }
  return 'border-blue-700/70 bg-blue-950/40 text-blue-100';
}

function requestStatusLabel(status: AssistantRequestStatus | null, runState: AssistantRunState): string {
  if (status?.status) {
    return status.status.replace('_', ' ');
  }
  if (runState === 'running') {
    return 'running';
  }
  if (runState === 'cancel_requested') {
    return 'cancel requested';
  }
  return 'ready';
}

function formatDurationSeconds(durationS: number): string {
  const normalized = Math.max(0, durationS);
  if (normalized < 10) {
    return `${normalized.toFixed(1)}s`;
  }
  return `${Math.round(normalized)}s`;
}

function formatDurationFromNs(startedAtNs?: number | null, finishedAtNs?: number | null): string | null {
  if (!startedAtNs || !finishedAtNs) {
    return null;
  }
  return formatDurationSeconds((finishedAtNs - startedAtNs) / NS_PER_S);
}

function formatJsonPreview(value: unknown): string {
  try {
    return JSON.stringify(value ?? {}, null, 2);
  } catch {
    return String(value);
  }
}

function responseSummary(response: AssistantChatResponse): string {
  const parts = [];
  if (response.toolCalls.length > 0) {
    parts.push(`${response.toolCalls.length} tool call${response.toolCalls.length === 1 ? '' : 's'}`);
  }
  if (response.draftMutated) {
    parts.push('Composer draft edited');
  }
  if (response.warnings.length > 0) {
    parts.push(`${response.warnings.length} warning${response.warnings.length === 1 ? '' : 's'}`);
  }
  return parts.join(' / ');
}

export function AssistantOverlay() {
  const deployment = useDeploymentStore((state) => state.deployment);
  const hydrateLoadedProgram = useProgramStore((state) => state.hydrateLoadedProgram);
  const cycleState = useCycleStore((state) => state.state);

  const [open, setOpen] = useState(false);
  const [input, setInput] = useState('');
  const [selectedModeId, setSelectedModeId] = useState('');
  const [conversationId, setConversationId] = useState<string | undefined>();
  const [activeRequestId, setActiveRequestId] = useState<string | null>(null);
  const [activeStatus, setActiveStatus] = useState<AssistantRequestStatus | null>(null);
  const [activeRequestStartedMs, setActiveRequestStartedMs] = useState<number | null>(null);
  const [nowMs, setNowMs] = useState(() => Date.now());
  const [runState, setRunState] = useState<AssistantRunState>('idle');
  const [transcript, setTranscript] = useState<TranscriptItem[]>([]);
  const [assistantEditedDraft, setAssistantEditedDraft] = useState(false);
  const [lastError, setLastError] = useState<string | null>(null);
  const [providerCatalog, setProviderCatalog] = useState<AssistantProviderCatalog | null>(null);
  const [providerError, setProviderError] = useState<string | null>(null);
  const transcriptRef = useRef<HTMLDivElement | null>(null);

  const assistant = deployment?.assistant ?? null;
  const assistantModes = deployment?.assistantModes ?? EMPTY_ASSISTANT_MODES;
  const toolCatalog = deployment?.toolCatalog ?? EMPTY_TOOL_CATALOG;
  const modeEntries = useMemo(
    () =>
      Object.entries(assistantModes)
        .filter(([, mode]) => mode.enabled !== false)
        .sort(([left], [right]) => left.localeCompare(right)),
    [assistantModes],
  );
  const selectedMode = selectedModeId ? assistantModes[selectedModeId] : undefined;
  const modeStatus = modeEntryStatus(selectedModeId, cycleState);
  const deploymentProviderId = assistant?.provider ?? null;
  const configuredProviderId = providerCatalog?.configuredProviderId ?? null;
  const providerId = configuredProviderId ?? deploymentProviderId ?? 'mock';
  const configuredProvider = providerCatalog?.providers.find((provider) => provider.id === providerId);
  const providerCatalogReady = !!providerCatalog || !!providerError;
  const providerConfigured = providerCatalog ? configuredProvider?.configured === true : false;
  const providerSummary = providerCatalog
    ? deploymentProviderId && deploymentProviderId !== providerId
      ? `${providerId} configured / ${deploymentProviderId} deployment`
      : `${providerId} configured`
    : providerError
      ? 'Assistant provider unavailable'
      : 'Loading assistant provider';
  const assistantConfigured =
    !!assistant?.enabled
    && modeEntries.length > 0
    && providerConfigured
    && !providerError;
  const canSend =
    assistantConfigured
    && providerCatalogReady
    && !!selectedModeId
    && modeStatus.allowed
    && runState === 'idle'
    && input.trim().length > 0;
  const endpointAlias = assistant?.endpointAlias ?? null;
  const modelEndpoint = endpointAlias ? deployment?.modelEndpoints?.[endpointAlias] : undefined;
  const modelLabel = modelEndpoint?.profileRef || modelEndpoint?.modelId || 'model not declared';
  const activeDuration =
    runState !== 'idle' && activeRequestStartedMs
      ? formatDurationSeconds((nowMs - activeRequestStartedMs) / 1000)
      : formatDurationFromNs(activeStatus?.startedAtNs, activeStatus?.finishedAtNs);

  useEffect(() => {
    if (!open) {
      return;
    }
    let cancelled = false;
    setProviderError(null);
    void api.assistant.providers()
      .then((catalog) => {
        if (!cancelled) {
          setProviderCatalog(catalog);
        }
      })
      .catch((error) => {
        if (!cancelled) {
          setProviderCatalog(null);
          setProviderError(
            error instanceof Error
              ? error.message
              : 'Unable to load configured assistant provider',
          );
        }
      });
    return () => {
      cancelled = true;
    };
  }, [open, deploymentProviderId]);

  useEffect(() => {
    if (selectedModeId && assistantModes[selectedModeId]?.enabled !== false) {
      return;
    }
    const preferred = modeEntries.find(([modeId]) => modeId === 'query')
      ?? modeEntries.find(([modeId]) => modeId === 'composer-assistant')
      ?? modeEntries[0];
    setSelectedModeId(preferred?.[0] ?? '');
  }, [assistantModes, modeEntries, selectedModeId]);

  useEffect(() => {
    if (!transcriptRef.current) {
      return;
    }
    transcriptRef.current.scrollTop = transcriptRef.current.scrollHeight;
  }, [transcript, open]);

  useEffect(() => {
    if (!activeRequestId || runState === 'idle') {
      return;
    }
    let cancelled = false;
    const poll = async () => {
      try {
        const status = await api.assistant.status(activeRequestId);
        if (!cancelled) {
          setActiveStatus(status);
        }
      } catch {
        // The chat response path still reports terminal failures. Status polling
        // should not replace the primary result with a transient read failure.
      }
    };
    void poll();
    const interval = window.setInterval(() => {
      void poll();
    }, 1000);
    return () => {
      cancelled = true;
      window.clearInterval(interval);
    };
  }, [activeRequestId, runState]);

  useEffect(() => {
    if (runState === 'idle') {
      return;
    }
    const interval = window.setInterval(() => {
      setNowMs(Date.now());
    }, 250);
    return () => window.clearInterval(interval);
  }, [runState]);

  const send = async () => {
    const message = input.trim();
    if (!canSend || !selectedModeId) {
      return;
    }
    const requestId = createRequestId();
    const startedAtMs = Date.now();
    const startedAtNs = startedAtMs * NS_PER_MS;
    setInput('');
    setLastError(null);
    setActiveRequestId(requestId);
    setActiveRequestStartedMs(startedAtMs);
    setNowMs(startedAtMs);
    setActiveStatus({
      requestId,
      conversationId: conversationId ?? '',
      providerId,
      assistantMode: selectedModeId,
      status: 'running',
      message: 'Assistant request is running',
      error: null,
      warnings: [],
      draftMutated: false,
      toolCallCount: 0,
      startedAtNs,
      updatedAtNs: startedAtNs,
      finishedAtNs: null,
    });
    setRunState('running');
    setTranscript((items) => [
      ...items,
      { kind: 'user', id: `${requestId}-user`, text: message, modeId: selectedModeId },
    ]);

    try {
      const response = await api.assistant.chat({
        requestId,
        conversationId,
        message,
        mode: 'provider',
        providerId,
        assistantMode: selectedModeId,
      });
      setConversationId(response.conversationId);
      const finishedAtNs = Date.now() * NS_PER_MS;
      setActiveStatus({
        requestId: response.requestId,
        conversationId: response.conversationId,
        providerId: response.providerId,
        assistantMode: response.assistantMode ?? selectedModeId,
        status: response.error ? 'failed' : 'completed',
        message: response.message,
        error: response.error ?? null,
        warnings: response.warnings,
        draftMutated: response.draftMutated,
        toolCallCount: response.toolCalls.length,
        startedAtNs,
        updatedAtNs: finishedAtNs,
        finishedAtNs,
      });
      setTranscript((items) => [
        ...items,
        {
          kind: 'assistant',
          id: `${requestId}-assistant`,
          text: response.message,
          response,
          error: response.error?.detail,
        },
      ]);
      if (response.draftMutated) {
        setAssistantEditedDraft(true);
        await hydrateLoadedProgram({ retries: 0 });
      }
    } catch (error) {
      const rawDetail = error instanceof Error ? error.message : 'Assistant request failed';
      const detail = rawDetail.toLowerCase().includes('timeout')
        ? `${rawDetail}. The model or bridge did not finish within the configured timeout; try a narrower request or cancel and retry.`
        : rawDetail;
      const finishedAtNs = Date.now() * NS_PER_MS;
      setActiveStatus({
        requestId,
        conversationId: conversationId ?? '',
        providerId,
        assistantMode: selectedModeId,
        status: 'failed',
        message: '',
        error: { code: 'request_failed', detail },
        warnings: [],
        draftMutated: false,
        toolCallCount: 0,
        startedAtNs,
        updatedAtNs: finishedAtNs,
        finishedAtNs,
      });
      setLastError(detail);
      setTranscript((items) => [
        ...items,
        { kind: 'assistant', id: `${requestId}-error`, text: '', error: detail },
      ]);
    } finally {
      setRunState('idle');
      setActiveRequestId(null);
      setActiveRequestStartedMs(null);
    }
  };

  const cancel = async () => {
    if (!activeRequestId || runState === 'idle') {
      return;
    }
    setRunState('cancel_requested');
    try {
      const response = await api.assistant.cancel(activeRequestId);
      setActiveStatus((current) => current
        ? {
          ...current,
          status: response.status === 'unknown' ? current.status : response.status,
          updatedAtNs: response.timestampNs,
          finishedAtNs: response.status === 'cancelled' ? response.timestampNs : current.finishedAtNs,
        }
        : current);
      if (response.cancelled) {
        setLastError(null);
      }
    } catch (error) {
      setLastError(error instanceof Error ? error.message : 'Assistant cancellation failed');
    }
  };

  const onKeyDown = (event: KeyboardEvent<HTMLTextAreaElement>) => {
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault();
      void send();
    }
  };

  return (
    <>
      {!open && (
        <button
          type="button"
          onClick={() => setOpen(true)}
          className="fixed bottom-20 right-6 z-40 flex h-12 w-12 items-center justify-center rounded-full border border-blue-400/70 bg-blue-600 text-sm font-semibold text-white shadow-xl shadow-black/40 transition hover:bg-blue-500 focus:outline-none focus:ring-2 focus:ring-blue-300"
          title="Open assistant"
        >
          AI
        </button>
      )}

      {open && (
        <div className="pointer-events-none fixed inset-x-0 bottom-16 z-40 px-6 pb-4">
          <section className="pointer-events-auto mx-auto flex h-[min(50vh,520px)] min-h-[320px] max-w-[1440px] flex-col overflow-hidden rounded-lg border border-gray-700/90 bg-gray-950/85 text-white shadow-2xl shadow-black/50 backdrop-blur-md">
            <header className="flex shrink-0 flex-wrap items-center gap-3 border-b border-gray-800 bg-gray-950/70 px-4 py-3">
              <div className="flex h-9 w-9 shrink-0 items-center justify-center rounded-full bg-blue-600 text-xs font-semibold">
                AI
              </div>
              <div className="min-w-0 flex-1">
                <div className="flex items-center gap-2">
                  <h2 className="truncate text-sm font-semibold text-gray-100">Assistant</h2>
                  <span className={`h-2 w-2 rounded-full ${statusTone(activeStatus?.status ?? runState)}`} />
                  <span className="text-xs text-gray-400">
                    {requestStatusLabel(activeStatus, runState)}
                  </span>
                </div>
                <p className="truncate text-xs text-gray-500">
                  {assistantConfigured
                    ? `${providerSummary} / ${selectedModeId || 'no mode'} / ${modeStatus.detail}`
                    : providerCatalogReady
                      ? providerSummary
                      : 'Loading assistant provider'}
                </p>
              </div>

              <label className="flex w-52 max-w-full flex-col gap-1 text-xs text-gray-400">
                Mode
                <select
                  value={selectedModeId}
                  onChange={(event) => setSelectedModeId(event.target.value)}
                  disabled={runState !== 'idle' || modeEntries.length === 0}
                  className="rounded border border-gray-700 bg-gray-900 px-2 py-1 text-sm text-white disabled:cursor-not-allowed disabled:opacity-50"
                >
                  {modeEntries.length === 0 && <option value="">No modes</option>}
                  {modeEntries.map(([modeId]) => (
                    <option key={modeId} value={modeId}>
                      {modeLabel(modeId)}
                    </option>
                  ))}
                </select>
              </label>

              <button
                type="button"
                onClick={() => setOpen(false)}
                className="h-9 w-9 rounded border border-gray-700 bg-gray-900 text-sm text-gray-200 hover:bg-gray-800"
                title="Close assistant"
              >
                x
              </button>
            </header>

            <div className="grid min-h-0 flex-1 grid-cols-1 gap-0 lg:grid-cols-[minmax(0,1fr)_320px]">
              <div className="flex min-h-0 flex-col border-gray-800 lg:border-r">
                <div ref={transcriptRef} className="min-h-0 flex-1 space-y-3 overflow-y-auto px-4 py-3">
                  {assistantEditedDraft && (
                    <div className="rounded border border-amber-700/80 bg-amber-950/50 px-3 py-2 text-sm text-amber-100">
                      Assistant edited the current Composer draft. Use the normal apply, save, or reload controls to keep or discard it.
                    </div>
                  )}

                  {lastError && (
                    <div className="rounded border border-red-700/80 bg-red-950/50 px-3 py-2 text-sm text-red-100">
                      {lastError}
                    </div>
                  )}

                  {providerError && (
                    <div className="rounded border border-red-700/80 bg-red-950/50 px-3 py-2 text-sm text-red-100">
                      {providerError}
                    </div>
                  )}

                  {transcript.length === 0 ? (
                    <div className="flex h-full items-center justify-center text-sm text-gray-500">
                      Select a mode and enter an assistant request.
                    </div>
                  ) : (
                    transcript.map((item) => {
                      const isUser = item.kind === 'user';
                      const bubbleClass = isUser
                        ? 'bg-blue-700 text-white'
                        : item.error
                          ? 'border border-red-800 bg-red-950/60 text-red-100'
                          : 'border border-gray-800 bg-gray-900/85 text-gray-100';
                      const text = isUser ? item.text : item.error || item.text;
                      return (
                        <div key={item.id} className={isUser ? 'text-right' : 'text-left'}>
                          <div className={`inline-block max-w-[82%] rounded px-3 py-2 text-sm ${bubbleClass}`}>
                            {isUser && (
                              <div className="mb-1 text-[11px] uppercase tracking-wide text-blue-200">
                                {modeLabel(item.modeId)}
                              </div>
                            )}
                            <div className="whitespace-pre-wrap leading-relaxed">
                              {text}
                            </div>
                            {!isUser && item.response && (
                              <div className="mt-2 border-t border-gray-700/70 pt-2 text-[11px] text-gray-400">
                                {responseSummary(item.response) || 'No draft changes'}
                              </div>
                            )}
                          </div>
                        </div>
                      );
                    })
                  )}
                </div>

                <div className="shrink-0 border-t border-gray-800 bg-gray-950/80 p-3">
                  {!modeStatus.allowed && selectedModeId && (
                    <div className="mb-2 text-xs text-amber-300">{modeStatus.detail}</div>
                  )}
                  <div className="flex gap-2">
                    <textarea
                      value={input}
                      onChange={(event) => setInput(event.target.value)}
                      onKeyDown={onKeyDown}
                      disabled={!assistantConfigured || runState !== 'idle'}
                      rows={2}
                      placeholder={assistantConfigured ? 'Ask the assistant...' : 'Assistant provider unavailable'}
                      className="min-h-14 flex-1 resize-none rounded border border-gray-700 bg-gray-900 px-3 py-2 text-sm text-white placeholder:text-gray-500 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
                    />
                    {runState === 'idle' ? (
                      <button
                        type="button"
                        onClick={() => void send()}
                        disabled={!canSend}
                        className="w-24 rounded bg-blue-600 px-3 py-2 text-sm font-medium text-white hover:bg-blue-500 disabled:cursor-not-allowed disabled:bg-gray-700 disabled:text-gray-400"
                      >
                        Send
                      </button>
                    ) : (
                      <button
                        type="button"
                        onClick={() => void cancel()}
                        disabled={runState === 'cancel_requested'}
                        className="w-24 rounded bg-red-700 px-3 py-2 text-sm font-medium text-white hover:bg-red-600 disabled:cursor-not-allowed disabled:bg-gray-700 disabled:text-gray-400"
                      >
                        Cancel
                      </button>
                    )}
                  </div>
                </div>
              </div>

              <aside className="min-h-0 overflow-y-auto border-t border-gray-800 bg-gray-950/60 px-3 py-3 lg:border-t-0">
                <section className="mb-4">
                  <div className="mb-2 text-xs font-semibold uppercase tracking-wide text-gray-500">
                    Provider
                  </div>
                  <div className="mb-4 rounded border border-gray-800 bg-gray-900/70 p-3 text-xs text-gray-400">
                    <div className="flex items-center justify-between gap-2">
                      <span>Configured</span>
                      <span className="truncate text-gray-100">{providerId}</span>
                    </div>
                    <div className="mt-2 flex items-center justify-between gap-2">
                      <span>Deployment</span>
                      <span className="truncate text-gray-300">{deploymentProviderId ?? 'none'}</span>
                    </div>
                    <div className="mt-2 flex items-center justify-between gap-2">
                      <span>Endpoint</span>
                      <span className="truncate text-gray-300">{endpointAlias ?? 'none'}</span>
                    </div>
                    <div className="mt-2 flex items-center justify-between gap-2">
                      <span>Model</span>
                      <span className="truncate text-gray-300">{modelLabel}</span>
                    </div>
                    {activeStatus && (
                      <div className="mt-2 flex items-center justify-between gap-2">
                        <span>Request</span>
                        <span className="truncate text-gray-300">
                          {activeDuration ? `${activeStatus.status} / ${activeDuration}` : activeStatus.status}
                        </span>
                      </div>
                    )}
                  </div>

                  <div className="mb-2 text-xs font-semibold uppercase tracking-wide text-gray-500">
                    Mode
                  </div>
                  <div className="rounded border border-gray-800 bg-gray-900/70 p-3">
                    <div className="text-sm font-medium text-gray-100">
                      {selectedModeId ? modeLabel(selectedModeId) : 'No mode'}
                    </div>
                    <p className="mt-1 text-xs leading-relaxed text-gray-400">
                      {selectedMode?.description || 'No mode is available in this deployment.'}
                    </p>
                    <div className="mt-3 text-xs text-gray-500">
                      {summarizeMode(selectedMode, toolCatalog)}
                    </div>
                  </div>
                </section>

                <section>
                  <div className="mb-2 text-xs font-semibold uppercase tracking-wide text-gray-500">
                    Tool Calls
                  </div>
                  {transcript
                    .filter((item): item is Extract<TranscriptItem, { kind: 'assistant' }> =>
                      item.kind === 'assistant' && !!item.response,
                    )
                    .flatMap((item) => item.response?.toolCalls ?? [])
                    .slice(-12)
                    .map((call, index) => (
                      <details
                        key={`${call.name}-${index}`}
                        className={`mb-2 rounded border px-2 py-2 text-xs ${toolStatusClasses(call.status)}`}
                      >
                        <summary className="flex cursor-pointer list-none items-center justify-between gap-2">
                          <span className="truncate font-medium">{call.name}</span>
                          <span className="shrink-0 text-[11px] uppercase">{call.status}</span>
                        </summary>
                        {call.error && (
                          <div className="mt-1 line-clamp-2 text-[11px] opacity-90">
                            {call.error}
                          </div>
                        )}
                        <div className="mt-2 space-y-2 border-t border-current/20 pt-2">
                          <div>
                            <div className="mb-1 text-[11px] uppercase opacity-70">Arguments</div>
                            <pre className="max-h-32 overflow-auto whitespace-pre-wrap rounded bg-black/30 p-2 text-[11px] leading-relaxed">
                              {formatJsonPreview(call.arguments)}
                            </pre>
                          </div>
                          {call.result && (
                            <div>
                              <div className="mb-1 text-[11px] uppercase opacity-70">Result</div>
                              <pre className="max-h-32 overflow-auto whitespace-pre-wrap rounded bg-black/30 p-2 text-[11px] leading-relaxed">
                                {formatJsonPreview(call.result)}
                              </pre>
                            </div>
                          )}
                        </div>
                      </details>
                    ))}
                  {transcript.every((item) =>
                    item.kind !== 'assistant' || !(item.response?.toolCalls.length),
                  ) && (
                    <div className="rounded border border-gray-800 bg-gray-900/60 p-3 text-xs text-gray-500">
                      No tool calls yet.
                    </div>
                  )}
                </section>
              </aside>
            </div>
          </section>
        </div>
      )}
    </>
  );
}
