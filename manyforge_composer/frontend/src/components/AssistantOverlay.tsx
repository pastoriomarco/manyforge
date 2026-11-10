import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type KeyboardEvent,
  type ReactNode,
  type RefObject,
} from 'react';

import { ApiError, api } from '../api/client';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { JsonValue } from '../types/api';
import type {
  AssistantModeDetail,
  AssistantModeIndex,
  AssistantProviderStatus,
  AssistantToolCallResult,
} from '../types/assistant';
import type { StructuralResourceState } from '../state/structuralResource';
import { LIFECYCLE_LABELS } from '../state/componentPresentation';
import { commandRefusalText } from '../state/commandRefusal';
import type { ComponentLifecycle, ComponentState } from '../types/components';
import {
  reconcileAssistantRequest,
  shouldReconcileAssistantRequest,
} from './assistantRequestRecovery';
import { useOperationalErrorMirror } from '../state/operationalErrors';

/** The orientation fields a request may use, with the 0.1.0 short labels. */
const ORIENTATION_FIELD_OPTIONS = [
  { key: 'rpy_deg', label: 'RPY', title: 'Absolute roll, pitch, yaw in degrees' },
  { key: 'rpy_delta_deg', label: 'Delta', title: 'Component-wise RPY delta in degrees' },
  { key: 'rotate_local_rpy_deg', label: 'Local', title: 'Physical local-frame RPY rotation in degrees' },
  { key: 'rotate_world_rpy_deg', label: 'World', title: 'Physical world-frame RPY rotation in degrees' },
  { key: 'xyzw', label: 'Quat', title: 'Absolute quaternion orientation' },
  { key: 'orientation_from', label: 'Ref', title: 'Copy orientation from another scene reference' },
] as const;
const ORIENTATION_FIELDS = ORIENTATION_FIELD_OPTIONS.map((option) => option.key);
const MAXIMUM_TRANSCRIPT_ITEMS = 200;
const MAXIMUM_PRESENTED_TOOL_RESULTS = 128;
/** The only chat mode: edits land in the Program Draft via the tool callback. */
const ASSISTANT_MODE = 'composer-assistant';
const ASSISTANT_MODE_LABEL = 'Read/Write';
export const NO_ASSISTANT_COMPONENT = 'Add an Assistant component';
export const PROGRAM_RUN_EXISTS = 'A Program run exists: the assistant runs only while no run exists';

export interface AssistantAvailability {
  /** Why the AI tab is disabled, or null when it can be opened. */
  unavailableReason: string | null;
  /** The mode index while the Deployment has an assistant component (the panel stays mounted). */
  modeIndex: AssistantModeIndex | null;
  modeError: string | null;
  /** The yellow notice of an enabled AI model whose `endpoint` stream is stale. */
  modelNotice: string | null;
}

function lifecycleText(lifecycle: ComponentLifecycle): string {
  return lifecycle === 'fault' ? 'faulted' : LIFECYCLE_LABELS[lifecycle].toLowerCase();
}

/**
 * The AI tab opens while the Deployment's assistant component and its AI model
 * are enabled and no Program run exists (paused or held runs included); the
 * Composer refuses a chat turn on the same facts.
 */
export function assistantAvailability(
  modes: StructuralResourceState<AssistantModeIndex>,
  components: readonly ComponentState[] | null,
  programRunExists: boolean,
): AssistantAvailability {
  const unavailable = (reason: string, modeIndex: AssistantModeIndex | null = null) => (
    { unavailableReason: reason, modeIndex, modeError: null, modelNotice: null });
  switch (modes.status) {
    case 'loading':
      return unavailable('Loading assistant modes…');
    case 'absent':
      return unavailable(modes.message);
    case 'error':
      return { unavailableReason: null, modeIndex: null, modeError: modes.message, modelNotice: null };
    case 'ready': {
      const index = modes.data;
      if (index.assistantComponentId === null) return unavailable(NO_ASSISTANT_COMPONENT);
      if (programRunExists) return unavailable(PROGRAM_RUN_EXISTS, index);
      const state = (id: string) => components?.find((component) => component.component_id === id);
      const assistant = state(index.assistantComponentId)?.lifecycle ?? 'offline';
      if (assistant === 'fault') return unavailable('Assistant faulted: Recover it', index);
      if (assistant === 'offline') return unavailable('Assistant is offline', index);
      if (assistant !== 'enabled') {
        return unavailable(`Assistant is ${lifecycleText(assistant)}: Enable it`, index);
      }
      const modelId = index.aiModelComponentId;
      const model = modelId === null ? undefined : state(modelId);
      if (modelId !== null && model?.lifecycle !== 'enabled') {
        return unavailable(`AI model ${modelId} is ${lifecycleText(model?.lifecycle ?? 'offline')}`, index);
      }
      const stale = model?.streams.some((stream) => stream.stream_id === 'endpoint' && stream.freshness === 'stale');
      return {
        unavailableReason: null,
        modeIndex: index,
        modeError: null,
        modelNotice: stale ? `AI model ${modelId}: endpoint not answering` : null,
      };
    }
  }
}

/** A reason or notice with the AI model's id in code style, as the panel shows it. */
function withComponentId(text: string, componentId: string | null): ReactNode {
  if (componentId === null) return text;
  const at = text.indexOf(` ${componentId}`);
  if (at < 0) return text;
  const end = at + 1 + componentId.length;
  return <>{text.slice(0, at + 1)}<code>{componentId}</code>{text.slice(end)}</>;
}

/** The Deployment's AI model component as the panel shows it. */
export interface AssistantModelSummary {
  componentId: string;
  model: string | null;
  baseUrl: string | null;
  /** The Deployment Draft differs from the Applied one: the assistant still uses the Applied model. */
  draftUnapplied: boolean;
}

interface PresentedToolCall {
  toolId: string;
  status: string;
  draftMutated: boolean;
  summary: string;
  arguments: string;
  result?: string;
}

interface TranscriptResponse {
  requestId: string;
  draftMutated: boolean;
  toolCalls: PresentedToolCall[];
  warnings: string[];
  turnCount: number;
  needsClarification: boolean;
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

/** The Composer's typed refusals of a chat turn by the component and run state. */
const CHAT_GATE_REFUSALS: ReadonlySet<string> = new Set([
  'assistant_component_absent',
  'assistant_component_not_enabled',
  'assistant_ai_model_not_enabled',
  'assistant_program_active',
]);

export function assistantFailureText(status: AssistantProviderStatus): string {
  if (status.error) {
    const { code, message } = status.error;
    if (CHAT_GATE_REFUSALS.has(code)) return commandRefusalText({ code, message, details: null, origin: null });
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
    if (code.includes('tool')) {
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

function jsonPreview(value: JsonValue | Record<string, JsonValue> | undefined): string {
  if (value === undefined) return '';
  try {
    return JSON.stringify(value, null, 2).slice(0, 8_192);
  } catch {
    return String(value);
  }
}

function record(value: unknown): Record<string, unknown> {
  return value && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown>
    : {};
}

function strings(value: unknown): string[] {
  return Array.isArray(value) ? value.filter((item): item is string => typeof item === 'string') : [];
}

function text(value: unknown): string | null {
  return typeof value === 'string' && value.length > 0 ? value : null;
}

function itemsLabel(action: string, noun: string, ids: string[], plural = `${noun}s`): string {
  if (ids.length === 0) return `${action} ${plural}`;
  const shown = ids.slice(0, 4).join(', ');
  const more = ids.length > 4 ? ` and ${ids.length - 4} more` : '';
  return `${action} ${ids.length === 1 ? noun : plural}: ${shown}${more}`;
}

/** One line per tool call, as the 0.1.0 panel showed it. */
export function assistantToolCallSummary(call: AssistantToolCallResult): string {
  const args = record(call.arguments);
  const result = record(call.result);
  const resultIds = [
    ...strings(result.objectIds), ...strings(result.ids), ...strings(result.names),
    ...strings(result.keys), ...strings(result.parameters),
  ];
  const objects = Array.isArray(args.objects) ? args.objects.map((item) => text(record(item).id)) : [];
  const sceneIds = resultIds.length > 0
    ? resultIds
    : [text(args.objectId), text(args.id), ...objects, ...strings(args.objectIds)]
      .filter((item): item is string => item !== null);
  const node = text(result.instanceId) ?? text(args.instanceId) ?? text(args.instance_id)
    ?? text(args.nodeName) ?? text(record(args.node).instance_id) ?? text(args.parentInstanceId);
  const named = (base: string) => (node ? `${base}: ${node}` : base);
  let summary: string;
  switch (call.toolId) {
    case 'program_read': summary = 'Read the Program Draft'; break;
    case 'program_validate': summary = 'Validated the Program Draft'; break;
    case 'catalog_read': summary = 'Read the node catalog'; break;
    case 'skills_read': summary = 'Read the skill catalog'; break;
    case 'deployment_capabilities_read': summary = 'Read the Deployment capabilities'; break;
    case 'status_read': summary = 'Read the runtime status'; break;
    case 'alarms_read': summary = 'Read the active alarms'; break;
    case 'history_read': summary = 'Read the history'; break;
    case 'scene_inspect': summary = 'Inspected the scene'; break;
    case 'inspect_isaac_scene': summary = 'Inspected the Isaac scene'; break;
    case 'scene_draft_add_object': summary = itemsLabel('Added', 'scene object', sceneIds); break;
    case 'scene_draft_update_object': summary = itemsLabel('Updated', 'scene object', sceneIds); break;
    case 'scene_draft_upsert_objects': summary = itemsLabel('Created or updated', 'scene object', sceneIds); break;
    case 'scene_draft_remove_objects': summary = itemsLabel('Removed', 'scene object', sceneIds); break;
    case 'tree_draft_insert_node': summary = named('Inserted node'); break;
    case 'tree_draft_update_node_params': summary = named('Updated node parameters'); break;
    case 'tree_draft_delete_node': summary = named('Deleted node'); break;
    case 'tree_draft_move_node': summary = named('Moved node'); break;
    case 'tree_draft_rename_node': summary = named('Renamed node'); break;
    case 'tree_draft_wrap_node': summary = named('Wrapped node'); break;
    case 'tree_draft_change_node_kind': summary = named('Changed node kind'); break;
    case 'tree_draft_replace_subtree': summary = named('Replaced subtree'); break;
    case 'program_draft_upsert_parameters': summary = itemsLabel('Created or updated', 'Program parameter', resultIds); break;
    case 'program_draft_remove_parameters': summary = itemsLabel('Removed', 'Program parameter', resultIds); break;
    case 'program_draft_upsert_contact_policies': summary = itemsLabel('Created or updated', 'contact policy', resultIds, 'contact policies'); break;
    case 'program_draft_remove_contact_policies': summary = itemsLabel('Removed', 'contact policy', resultIds, 'contact policies'); break;
    case 'blackboard_draft_upsert_namespaces': summary = itemsLabel('Created or updated', 'blackboard namespace', resultIds); break;
    case 'blackboard_draft_remove_namespaces': summary = itemsLabel('Removed', 'blackboard namespace', resultIds); break;
    case 'blackboard_draft_upsert_keys': summary = itemsLabel('Created or updated', 'blackboard key', resultIds); break;
    case 'blackboard_draft_remove_keys': summary = itemsLabel('Removed', 'blackboard key', resultIds); break;
    default: summary = call.toolId.split(/[._]/).join(' ');
  }
  if (call.status === 'failed') {
    const reason = summarizeAssistantToolResult(call.result);
    return reason ? `${summary} failed: ${reason}` : `${summary} failed`;
  }
  if (call.status === 'skipped' || call.status === 'rejected') return `${summary} (${call.status})`;
  return summary;
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
        summary: assistantToolCallSummary(call),
        arguments: jsonPreview(call.arguments),
        ...(call.result === undefined ? {} : { result: jsonPreview(call.result) }),
      })),
    warnings: response.warnings.slice(0, 32).map((warning) => warning.slice(0, 1_024)),
    turnCount: response.turnCount,
    needsClarification: response.needsClarification,
  };
}

function responseSummary(response: TranscriptResponse): string {
  const parts: string[] = [];
  if (response.toolCalls.length > 0) {
    parts.push(`${response.toolCalls.length} tool call${response.toolCalls.length === 1 ? '' : 's'}`);
  }
  parts.push(response.draftMutated ? 'Draft edited' : 'No Draft changes');
  if (response.warnings.length > 0) {
    parts.push(`${response.warnings.length} warning${response.warnings.length === 1 ? '' : 's'}`);
  }
  return parts.join(' · ');
}

/** The Draft edits of a reply, or all its actions when it edited nothing. */
function responseActions(response: TranscriptResponse): { heading: string; items: string[] } | null {
  if (response.toolCalls.length === 0) return null;
  const edits = response.toolCalls.filter((call) => call.draftMutated);
  return response.draftMutated && edits.length > 0
    ? { heading: 'Draft changes', items: edits.map((call) => call.summary) }
    : { heading: 'Actions', items: response.toolCalls.map((call) => call.summary) };
}

function formatElapsed(ms: number): string {
  const seconds = Math.max(0, ms) / 1_000;
  return seconds < 10 ? `${seconds.toFixed(1)} s` : `${Math.round(seconds)} s`;
}

type DockState = 'ready' | 'running' | 'reconciling' | 'unknown' | 'completed' | 'failed' | 'cancelled';

function dockStateLabel(state: DockState, draftMutated: boolean): string {
  switch (state) {
    case 'ready': return 'Ready';
    case 'running': return 'Working';
    case 'reconciling': return 'Checking the result';
    case 'unknown': return 'Outcome unknown';
    case 'completed': return draftMutated ? 'Completed · Draft changed' : 'Completed';
    case 'failed': return 'Failed';
    case 'cancelled': return 'Cancelled';
  }
}

export function AssistantOverlay({
  open,
  onClose,
  returnFocusRef,
  unavailableReason,
  modeIndex,
  modeError,
  modelNotice = null,
  onRetryModes,
  onTurnRunningChange,
  programController,
  model,
}: {
  open: boolean;
  onClose: () => void;
  returnFocusRef?: RefObject<HTMLElement | null>;
  unavailableReason: string | null;
  modeIndex: AssistantModeIndex | null;
  modeError: string | null;
  modelNotice?: string | null;
  onRetryModes: () => void;
  /** Whether this page's chat turn runs: Execute, Run and Resume wait for it. */
  onTurnRunningChange?: (running: boolean) => void;
  programController: ProgramWorkspaceController;
  model: AssistantModelSummary | null;
}) {
  const ownerEpoch = useRef(identity('composer.page'));
  const recovery = useRef<AbortController | null>(null);
  const transcriptRef = useRef<HTMLDivElement | null>(null);
  const inputRef = useRef<HTMLTextAreaElement | null>(null);
  const [conversationId, setConversationId] = useState(() => identity('conversation'));
  const [input, setInput] = useState('');
  const [transcript, setTranscript] = useState<TranscriptItem[]>([]);
  const [active, setActive] = useState<AssistantProviderStatus | null>(null);
  const [dockState, setDockState] = useState<DockState>('ready');
  const startedMs = useRef<number | null>(null);
  const [elapsedMs, setElapsedMs] = useState<number | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [draftEdited, setDraftEdited] = useState(false);
  const [operationalError, setOperationalError] = useState<string | null>(null);
  const [retryIntent, setRetryIntent] = useState<RetryIntent | null>(null);
  const [orientationFields, setOrientationFields] = useState<string[]>([...ORIENTATION_FIELDS]);
  const [detail, setDetail] = useState<AssistantModeDetail | null>(null);
  useOperationalErrorMirror({
    source: 'composer.assistant',
    title: 'Assistant request or response failed',
    message: operationalError,
    onDismiss: () => setOperationalError(null),
  });

  useEffect(() => () => recovery.current?.abort(), []);

  // The model and tools the mode uses now; read again whenever the projection changes.
  const projectionDigest = modeIndex?.assistantProjectionDigest ?? null;
  const modeAvailable = modeIndex?.modes.some((mode) => mode.mode === ASSISTANT_MODE) === true;
  useEffect(() => {
    if (!open || projectionDigest === null || !modeAvailable) return undefined;
    const controller = new AbortController();
    api.assistant.mode(ASSISTANT_MODE, controller.signal)
      .then((value) => setDetail(value))
      .catch(() => { if (!controller.signal.aborted) setDetail(null); });
    return () => controller.abort();
  }, [modeAvailable, open, projectionDigest]);

  const busy = dockState === 'running' || dockState === 'reconciling';
  useEffect(() => { onTurnRunningChange?.(busy); }, [busy, onTurnRunningChange]);
  useEffect(() => () => onTurnRunningChange?.(false), [onTurnRunningChange]);
  const selected = modeIndex?.modes.find((mode) => mode.mode === ASSISTANT_MODE);
  const canSend = !!selected && unavailableReason === null && !busy && input.trim().length > 0;
  const orientationSet = useMemo(() => new Set(orientationFields), [orientationFields]);
  const lastResponse = useMemo(
    () => [...transcript].reverse().find((item) => item.response)?.response ?? null,
    [transcript],
  );

  useEffect(() => {
    if (!busy) return undefined;
    const interval = window.setInterval(() => {
      if (startedMs.current !== null) setElapsedMs(Date.now() - startedMs.current);
    }, 250);
    return () => window.clearInterval(interval);
  }, [busy]);

  useEffect(() => {
    const element = transcriptRef.current;
    if (open && element) element.scrollTop = element.scrollHeight;
  }, [open, transcript]);

  useEffect(() => {
    if (!open) return undefined;
    window.requestAnimationFrame(() => inputRef.current?.focus());
    const handleKeyDown = (event: globalThis.KeyboardEvent) => {
      if (event.key !== 'Escape') return;
      event.preventDefault();
      onClose();
      window.requestAnimationFrame(() => returnFocusRef?.current?.focus());
    };
    document.addEventListener('keydown', handleKeyDown);
    return () => document.removeEventListener('keydown', handleKeyDown);
  }, [onClose, open, returnFocusRef]);

  const stopClock = useCallback(() => {
    if (startedMs.current !== null) setElapsedMs(Date.now() - startedMs.current);
    startedMs.current = null;
  }, []);

  const finish = useCallback(async (status: AssistantProviderStatus) => {
    setActive(status);
    stopClock();
    if (status.state === 'completed' && status.response) {
      setOperationalError(null);
      const response = status.response;
      setDockState('completed');
      setTranscript((items) => [...items, {
        id: `${status.requestId}.assistant`,
        role: 'assistant' as const,
        text: response.message,
        response: presentationResponse(status),
      }].slice(-MAXIMUM_TRANSCRIPT_ITEMS));
      setRetryIntent(null);
      setNotice(null);
      if (response.draftMutated) {
        setDraftEdited(true);
        await programController.refresh();
      }
      return;
    }
    const detail = assistantFailureText(status);
    setDockState(status.state === 'cancelled' ? 'cancelled' : 'failed');
    setOperationalError(status.state === 'cancelled' ? null : detail);
    setTranscript((items) => [...items, {
      id: `${status.requestId}.error`,
      role: 'assistant' as const,
      text: '',
      error: detail,
    }].slice(-MAXIMUM_TRANSCRIPT_ITEMS));
  }, [programController, stopClock]);

  const reconcile = useCallback(async (requestId: string) => {
    recovery.current?.abort();
    const controller = new AbortController();
    recovery.current = controller;
    setDockState((state) => (state === 'running' ? state : 'reconciling'));
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
    stopClock();
    const detail = result.kind === 'not_found'
      ? `No bridge record was found for ${requestId}; recheck before retrying.`
      : result.kind === 'unreachable'
        ? `The bridge status is unreachable: ${result.detail}`
        : 'The assistant result is still unknown; recheck before retrying.';
    setDockState('unknown');
    setNotice(detail);
    setOperationalError(detail);
  }, [finish, stopClock]);

  const send = useCallback(async (intent?: RetryIntent) => {
    const message = (intent?.message ?? input).trim();
    if (!message || busy || !selected || unavailableReason !== null) return;
    const requestId = identity('assistant.request');
    const request: Parameters<typeof api.assistant.submit>[0] = {
      requestId,
      requestOwnerEpoch: ownerEpoch.current,
      sourceLabel: 'composer_ui',
      correlationId: null,
      conversationId,
      assistantMode: ASSISTANT_MODE,
      message,
      requestedTools: [],
      fieldPolicy: { allowedOrientationFields: [...orientationFields].sort() },
    };
    setInput('');
    setNotice(null);
    setOperationalError(null);
    setRetryIntent({ message });
    setTranscript((items) => [...items, { id: `${requestId}.user`, role: 'user' as const, text: message }]
      .slice(-MAXIMUM_TRANSCRIPT_ITEMS));
    setDockState('running');
    startedMs.current = Date.now();
    setElapsedMs(0);
    setActive({
      requestId,
      requestOwnerEpoch: ownerEpoch.current,
      sourceLabel: 'composer_ui',
      correlationId: null,
      conversationId,
      assistantMode: ASSISTANT_MODE,
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
        setNotice(`Submission was interrupted; checking ${requestId} before any retry.`);
        await reconcile(requestId);
        return;
      }
      const detail = error instanceof Error ? error.message : 'Assistant request failed.';
      await finish({
        requestId: request.requestId,
        requestOwnerEpoch: request.requestOwnerEpoch,
        sourceLabel: request.sourceLabel,
        correlationId: request.correlationId,
        conversationId: request.conversationId,
        assistantMode: request.assistantMode,
        state: 'failed',
        response: null,
        error: error instanceof ApiError && error.structuredError !== null
          ? { code: error.structuredError.code, message: error.structuredError.message }
          : { code: error instanceof ApiError ? `http_${error.status}` : 'request_failed', message: detail },
      });
    }
  }, [busy, conversationId, finish, input, orientationFields, reconcile, selected, unavailableReason]);

  const cancel = async () => {
    if (!active || dockState !== 'running') return;
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
    setDockState('ready');
    setElapsedMs(null);
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

  const onKeyDown = (event: KeyboardEvent<HTMLTextAreaElement>) => {
    if (event.key === 'Enter' && !event.shiftKey && !event.nativeEvent.isComposing) {
      event.preventDefault();
      if (canSend) void send();
    }
  };

  const close = () => {
    onClose();
    window.requestAnimationFrame(() => returnFocusRef?.current?.focus());
  };

  const elapsed = elapsedMs;
  const stateLabel = dockStateLabel(dockState, active?.response?.draftMutated === true);
  const appliedModel = detail?.modelEndpoint ?? null;
  const modelName = appliedModel?.modelId ?? model?.model ?? null;
  const modelUrl = appliedModel?.baseUrl ?? model?.baseUrl ?? null;
  const modelLine = modelName || modelUrl
    ? `${modelName ?? 'model not set'}${modelUrl ? ` at ${modelUrl}` : ''}`
    : 'No AI model component';
  const modelComponentId = modeIndex?.aiModelComponentId ?? null;
  const draftDiffers = appliedModel !== null && model !== null && model.draftUnapplied
    && (model.model !== appliedModel.modelId || model.baseUrl !== appliedModel.baseUrl);

  return (
    <section
      id="assistant-dock"
      className="assistant-dock"
      aria-labelledby="assistant-dock-title"
      hidden={!open}
    >
      <header className="assistant-dock-header">
        <span className="assistant-badge" aria-hidden="true">AI</span>
        <div className="assistant-dock-heading">
          <div className="assistant-dock-title-row">
            <h2 id="assistant-dock-title">Assistant</h2>
            <span className={`assistant-state-dot state-${dockState}`} aria-hidden="true" />
            <span className="assistant-state-label" role="status">
              {stateLabel}
              {elapsed !== null && (busy || dockState === 'completed' || dockState === 'failed')
                ? ` · ${formatElapsed(elapsed)}` : ''}
            </span>
          </div>
          <p className="assistant-dock-subtitle" title={unavailableReason ?? modelLine}>
            {unavailableReason === null ? modelLine : withComponentId(unavailableReason, modelComponentId)}
          </p>
        </div>
        <div className="assistant-mode" aria-label="Assistant mode">
          <span className="assistant-mode-caption">Mode</span>
          <span className="assistant-mode-pill" title={selected?.description}>
            {selected ? ASSISTANT_MODE_LABEL : 'No mode'}
          </span>
        </div>
        <button
          type="button"
          className="quiet-button"
          disabled={busy || (transcript.length === 0 && dockState === 'ready')}
          title="Start a new conversation: clears the context the model sees."
          onClick={newConversation}
        >
          New conversation
        </button>
        <button
          type="button"
          className="icon-button"
          aria-label="Close assistant"
          title="Close assistant (the conversation is kept)"
          onClick={close}
        >
          ×
        </button>
      </header>

      <div className="assistant-dock-body">
        <div className="assistant-conversation">
          {draftEdited && (
            <div className="assistant-draft-banner" role="status">
              <span>
                The assistant edited the Program Draft. Apply to use the changes, or Discard the Draft
                to undo them.
              </span>
              <button type="button" className="text-button" onClick={() => setDraftEdited(false)}>
                Dismiss
              </button>
            </div>
          )}
          {modeError && (
            <div className="notice severity-error" role="alert">
              <span>{modeError}</span>
              <button type="button" onClick={onRetryModes}>Retry discovery</button>
            </div>
          )}
          {modelNotice && (
            <div className="notice severity-warning" role="status">
              <span>{withComponentId(modelNotice, modelComponentId)}</span>
            </div>
          )}
          <div ref={transcriptRef} className="assistant-transcript" aria-live="polite">
            {transcript.length === 0 ? (
              <p className="assistant-empty">
                {unavailableReason !== null
                  ? withComponentId(unavailableReason, modelComponentId)
                  : selected
                    ? 'Ask for a Scene or Program change. The assistant edits the Program Draft; you Apply or Discard it.'
                    : 'The assistant is not available.'}
              </p>
            ) : transcript.map((item) => {
              const actions = item.response ? responseActions(item.response) : null;
              return (
                <div key={item.id} className={`assistant-row ${item.role}`}>
                  <article className={`assistant-bubble ${item.role}${item.error ? ' error' : ''}`}>
                    {item.role === 'user' && <div className="assistant-bubble-mode">{ASSISTANT_MODE_LABEL}</div>}
                    <div className="assistant-bubble-text">{item.error ?? item.text}</div>
                    {item.response && (
                      <div className="assistant-bubble-meta">
                        {responseSummary(item.response)}
                        {item.response.needsClarification ? ' · needs your answer' : ''}
                      </div>
                    )}
                    {actions && (
                      <div className="assistant-actions-box">
                        <div className="assistant-actions-heading">{actions.heading}</div>
                        <ul>
                          {actions.items.map((detail, index) => (
                            <li key={`${item.id}.action.${index}`}>{detail}</li>
                          ))}
                        </ul>
                      </div>
                    )}
                    {item.response && item.response.warnings.length > 0 && (
                      <ul className="inline-message severity-warning">
                        {item.response.warnings.map((warning, index) => (
                          <li key={`${item.id}.warning.${index}`}>{warning}</li>
                        ))}
                      </ul>
                    )}
                  </article>
                </div>
              );
            })}
            {busy && (
              <div className="assistant-row assistant">
                <div className="assistant-bubble assistant pending">
                  {dockState === 'reconciling' ? 'Checking the result…' : 'Working…'}
                </div>
              </div>
            )}
          </div>

          <div className="assistant-composer">
            {notice && <p className="notice severity-info" role="status">{notice}</p>}
            <div className="assistant-composer-row">
              <textarea
                ref={inputRef}
                value={input}
                disabled={busy || !selected || unavailableReason !== null}
                maxLength={65_536}
                rows={2}
                aria-label="Message to the assistant"
                placeholder={selected && unavailableReason === null
                  ? 'Ask the assistant… (Enter sends, Shift+Enter adds a line)' : 'Assistant unavailable'}
                onChange={(event) => setInput(event.target.value)}
                onKeyDown={onKeyDown}
              />
              <div className="assistant-composer-buttons">
                {dockState === 'running' ? (
                  <button type="button" className="danger-button" onClick={() => void cancel()}>Cancel</button>
                ) : dockState === 'unknown' && active ? (
                  <button type="button" className="primary-action" onClick={() => void reconcile(active.requestId)}>
                    Recheck
                  </button>
                ) : (
                  <button type="button" className="primary-action" disabled={!canSend} onClick={() => void send()}>
                    Send
                  </button>
                )}
                <button
                  type="button"
                  className="quiet-button"
                  disabled={busy || !retryIntent || dockState === 'completed' || unavailableReason !== null}
                  title="Send the last message again as a new request"
                  onClick={() => void send(retryIntent ?? undefined)}
                >
                  Retry
                </button>
              </div>
            </div>
          </div>
        </div>

        <aside className="assistant-side">
          <section>
            <h3>Model</h3>
            <dl className="assistant-facts">
              <div>
                <dt>Component</dt>
                <dd>{appliedModel?.aiModelComponentId ?? model?.componentId ?? modeIndex?.aiModelComponentId ?? 'none'}</dd>
              </div>
              <div><dt>Model</dt><dd title={modelName ?? undefined}>{modelName ?? 'not set'}</dd></div>
              <div><dt>Endpoint</dt><dd title={modelUrl ?? undefined}>{modelUrl ?? 'not set'}</dd></div>
              {appliedModel?.timeoutS != null && <div><dt>Timeout</dt><dd>{appliedModel.timeoutS} s</dd></div>}
              {lastResponse && <div><dt>Model turns</dt><dd>{lastResponse.turnCount}</dd></div>}
            </dl>
            {draftDiffers && (
              <p className="inline-message severity-warning">
                The Deployment Draft names {model?.model ?? 'another model'}; Apply it to use it.
              </p>
            )}
          </section>

          <section>
            <h3>Mode</h3>
            <div className="assistant-side-card">
              <strong>{selected ? ASSISTANT_MODE_LABEL : 'No mode'}</strong>
              <p>{selected?.description ?? 'This Deployment offers no assistant chat mode.'}</p>
              {detail && (
                <p className="muted">{detail.toolCount} tools · {detail.nodeCount} nodes in its catalog</p>
              )}
            </div>
          </section>

          <section>
            <h3>Orientation</h3>
            <div className="assistant-orientation">
              {ORIENTATION_FIELD_OPTIONS.map((option) => {
                const checked = orientationSet.has(option.key);
                const disabled = busy || (checked && orientationSet.size === 1);
                return (
                  <label key={option.key} title={option.title} className={checked ? 'checked' : undefined}>
                    <input
                      type="checkbox"
                      checked={checked}
                      disabled={disabled}
                      onChange={() => toggleOrientation(option.key)}
                    />
                    <span>{option.label}</span>
                  </label>
                );
              })}
            </div>
          </section>

          <section>
            <h3>Tool calls</h3>
            {!lastResponse || lastResponse.toolCalls.length === 0 ? (
              <p className="muted">{busy ? 'Tool calls appear when the reply arrives.' : 'No tool calls yet.'}</p>
            ) : (
              <ul className="assistant-tool-calls">
                {lastResponse.toolCalls.map((call, index) => (
                  <li key={`${lastResponse.requestId}.${index}`}>
                    <details className={`assistant-tool-call status-${call.status}`}>
                      <summary>
                        <span className="assistant-tool-summary">{call.summary}</span>
                        <span className="assistant-tool-status">{call.status}</span>
                      </summary>
                      <div className="assistant-tool-id"><code>{call.toolId}</code></div>
                      {call.arguments && call.arguments !== '{}' && (
                        <>
                          <div className="assistant-tool-label">Arguments</div>
                          <pre>{call.arguments}</pre>
                        </>
                      )}
                      {call.result !== undefined && (
                        <>
                          <div className="assistant-tool-label">Result</div>
                          <pre>{call.result}</pre>
                        </>
                      )}
                    </details>
                  </li>
                ))}
              </ul>
            )}
          </section>
        </aside>
      </div>
    </section>
  );
}
