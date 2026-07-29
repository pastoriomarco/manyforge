import { useEffect, useMemo, useRef, useState } from 'react';
import type { KeyboardEvent } from 'react';
import { ApiError, api } from '../api/client';
import { useCycleStore } from '../store/cycleStore';
import { useDeploymentStore } from '../store/deploymentStore';
import { useProgramStore } from '../store/programStore';
import { useViewerSelectionStore } from '../store/viewerSelectionStore';
import {
  reconcileAssistantRequest,
  shouldReconcileAssistantRequest,
} from './assistantRequestRecovery';
import type {
  AssistantChatResponse,
  AssistantModeDefinition,
  AssistantProviderCatalog,
  AssistantRequestStatus,
  AssistantToolCall,
  AssistantToolDefinition,
  ModelEndpointCatalog,
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

type AssistantRunState =
  | 'idle'
  | 'running'
  | 'cancel_requested'
  | 'reconciling'
  | 'outcome_unknown';

const EMPTY_ASSISTANT_MODES: Record<string, AssistantModeDefinition> = {};
const EMPTY_TOOL_CATALOG: AssistantToolDefinition[] = [];
const NS_PER_MS = 1_000_000;
const NS_PER_S = 1_000_000_000;
const ORIENTATION_FIELD_OPTIONS = [
  {
    key: 'orientation_rpy_deg',
    label: 'RPY',
    title: 'Absolute roll, pitch, yaw in degrees',
  },
  {
    key: 'rpy_delta_deg',
    label: 'Delta',
    title: 'Component-wise RPY delta in degrees',
  },
  {
    key: 'rotate_local_rpy_deg',
    label: 'Local',
    title: 'Physical local-frame RPY rotation in degrees',
  },
  {
    key: 'rotate_world_rpy_deg',
    label: 'World',
    title: 'Physical world-frame RPY rotation in degrees',
  },
  {
    key: 'orientation_xyzw',
    label: 'Quat',
    title: 'Absolute quaternion orientation',
  },
  {
    key: 'orientation_from',
    label: 'Ref',
    title: 'Copy orientation from another scene reference',
  },
] as const;
const DEFAULT_ASSISTANT_ORIENTATION_FIELDS = ORIENTATION_FIELD_OPTIONS.map((option) => option.key);

type OrientationFieldKey = typeof ORIENTATION_FIELD_OPTIONS[number]['key'];

function createRequestId(): string {
  const suffix = Math.random().toString(16).slice(2, 8);
  return `composer-ui-${Date.now()}-${suffix}`;
}

// Friendly labels for known assistant modes. Pill toggle uses the short
// form; the subtitle and the mode-card use the long form. Any modeId not
// listed here falls back to title-cased (`error-recovery` →
// `Error Recovery`) so adding a mode in the deployment manifest doesn't
// require a UI change.
const FRIENDLY_MODE_PILL: Record<string, string> = {
  'query': 'Read',
  'composer-assistant': 'Compose',
  'error-recovery': 'Recover',
};
const FRIENDLY_MODE_LONG: Record<string, string> = {
  'query': 'Read-only',
  'composer-assistant': 'Read/Write',
  'error-recovery': 'Recovery',
};

function titleCase(modeId: string): string {
  return modeId
    .split('-')
    .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
    .join(' ');
}

function modeLabel(modeId: string): string {
  return FRIENDLY_MODE_PILL[modeId] ?? titleCase(modeId);
}

function modeLongLabel(modeId: string): string {
  return FRIENDLY_MODE_LONG[modeId] ?? titleCase(modeId);
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

function liveToolStatusClasses(status: 'running' | 'completed' | 'failed'): string {
  if (status === 'completed') {
    return 'border-emerald-700/70 bg-emerald-950/40 text-emerald-100';
  }
  if (status === 'failed') {
    return 'border-red-700/70 bg-red-950/40 text-red-100';
  }
  // running — pulsing-blue look so operators see it's still in flight
  return 'border-blue-600 bg-blue-950/60 text-blue-100 animate-pulse';
}

function assistantErrorTitle(error?: { code: string; detail: string } | null, fallback = ''): string {
  const code = error?.code ?? '';
  const detail = error?.detail || fallback;
  const normalized = `${code} ${detail}`.toLowerCase();
  if (code === 'cancelled' || normalized.includes('cancelled')) {
    return 'Request cancelled';
  }
  if (code === 'timeout' || normalized.includes('timeout') || normalized.includes('timed out')) {
    return 'Model timed out';
  }
  if (code === 'model_unresolved' || normalized.includes('could not resolve a model')) {
    return 'Model unavailable';
  }
  if (
    code === 'upstream_http_error'
    || code === 'upstream_call_error'
    || normalized.includes('bad gateway')
    || normalized.includes('bridge')
  ) {
    return 'Bridge or model call failed';
  }
  if (
    code === 'response_rejected'
    || normalized.includes('out-of')
    || normalized.includes('outside the active')
    || normalized.includes('tool rejected')
  ) {
    return 'Tool rejected';
  }
  if (
    code === 'envelope_rejected'
    || code === 'invalid_envelope'
    || code === 'invalid_json'
    || normalized.includes('malformed')
  ) {
    return 'Invalid assistant request';
  }
  if (normalized.includes('provider') && normalized.includes('configured')) {
    return 'Provider not configured';
  }
  return 'Assistant request failed';
}

function assistantErrorMessage(error?: { code: string; detail: string } | null, fallback = ''): string {
  const title = assistantErrorTitle(error, fallback);
  const detail = error?.detail || fallback;
  const guidance: Record<string, string> = {
    'Bridge or model call failed': 'Check that the bridge and local model endpoint are running, then retry.',
    'Invalid assistant request': 'Reload the deployment and retry the request.',
    'Model timed out': 'Try a narrower request, or cancel and retry after the model is ready.',
    'Model unavailable': 'Start the local model endpoint or set the bridge model configuration.',
    'Provider not configured': 'Start the known-good assistant demo or configure the assistant provider.',
    'Tool rejected': 'The model attempted an unauthorized tool call; use the correct mode or adjust the tool catalog.',
  };
  const suffix = guidance[title] ? ` ${guidance[title]}` : '';
  if (!detail.trim()) {
    return `${title}.${suffix ? ` ${suffix}` : ''}`.trim();
  }
  if (detail.toLowerCase().startsWith(title.toLowerCase())) {
    return `${detail}${suffix ? ` ${suffix}` : ''}`;
  }
  return `${title}: ${detail}${suffix ? ` ${suffix}` : ''}`;
}

function requestStatusLabel(status: AssistantRequestStatus | null, runState: AssistantRunState): string {
  if (runState === 'reconciling') {
    return 'checking server result';
  }
  if (runState === 'outcome_unknown') {
    return 'outcome unknown';
  }
  if (status?.status) {
    if (status.status === 'completed' && status.draftMutated) {
      return 'completed · draft changed';
    }
    if (status.status === 'failed' && status.error) {
      return `failed · ${assistantErrorTitle(status.error).toLowerCase()}`;
    }
    if (status.status === 'cancelled') {
      return 'cancelled';
    }
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

function stringArray(value: unknown): string[] {
  if (!Array.isArray(value)) {
    return [];
  }
  return value.filter((item): item is string => typeof item === 'string' && item.trim().length > 0);
}

function recordArray(value: unknown): Record<string, unknown>[] {
  if (!Array.isArray(value)) {
    return [];
  }
  return value.filter((item): item is Record<string, unknown> =>
    !!item && typeof item === 'object' && !Array.isArray(item),
  );
}

function recordField(value: unknown): Record<string, unknown> | null {
  return !!value && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
}

function stringField(record: Record<string, unknown>, field: string): string | null {
  const value = record[field];
  return typeof value === 'string' && value.trim().length > 0 ? value : null;
}

function compactList(items: string[]): string {
  if (items.length <= 3) {
    return items.join(', ');
  }
  return `${items.slice(0, 3).join(', ')} +${items.length - 3} more`;
}

function summarizeItems(action: string, itemLabel: string, ids: string[]): string {
  if (ids.length === 0) {
    return action;
  }
  const plural = ids.length === 1 ? itemLabel : `${itemLabel}s`;
  return `${action} ${ids.length} ${plural}: ${compactList(ids)}`;
}

function sceneObjectIdsFromArguments(argumentsValue: Record<string, unknown>): string[] {
  const directIds = [
    stringField(argumentsValue, 'objectId'),
    stringField(argumentsValue, 'id'),
  ].filter((value): value is string => !!value);
  const sceneResources = recordArray(argumentsValue.sceneResources);
  const resourceIds = sceneResources
    .map((resource) => stringField(resource, 'objectId'))
    .filter((value): value is string => !!value);
  const objectIds = stringArray(argumentsValue.objectIds);
  return [...directIds, ...resourceIds, ...objectIds];
}

function toolCallSummary(call: AssistantToolCall): string {
  const args = call.arguments ?? {};
  const result = call.result ?? {};
  const resultIds = stringArray(result.objectIds)
    .concat(stringArray(result.ids))
    .concat(stringArray(result.names));
  const sceneIds = resultIds.length > 0 ? resultIds : sceneObjectIdsFromArguments(args);
  const nodeName =
    stringField(result, 'nodeName')
    ?? stringField(args, 'nodeName')
    ?? stringField(args, 'parentName')
    ?? stringField(args, 'newParentName')
    ?? stringField(recordField(args.node) ?? {}, 'name');

  let summary: string;
  switch (call.name) {
    case 'program_read':
      summary = 'Read the current program';
      break;
    case 'program_validate':
      summary = 'Validated the current program';
      break;
    case 'catalog_read':
      summary = 'Read the node catalog';
      break;
    case 'skills_read':
      summary = 'Read the skill catalog';
      break;
    case 'deployment_capabilities_read':
      summary = 'Read deployment capabilities';
      break;
    case 'scene_draft_add_object':
      summary = summarizeItems('Added', 'scene object', sceneIds);
      break;
    case 'scene_draft_update_object':
      summary = summarizeItems('Updated', 'scene object', sceneIds);
      break;
    case 'scene_draft_upsert_objects':
      summary = summarizeItems('Created or updated', 'scene object', sceneIds);
      break;
    case 'scene_draft_remove_objects':
      summary = summarizeItems('Removed', 'scene object', sceneIds);
      break;
    case 'tree_draft_insert_node':
      summary = nodeName ? `Inserted tree node: ${nodeName}` : 'Inserted a tree node';
      break;
    case 'tree_draft_update_node_params':
      summary = nodeName ? `Updated tree node parameters: ${nodeName}` : 'Updated tree node parameters';
      break;
    case 'tree_draft_delete_node':
      summary = nodeName ? `Deleted tree node: ${nodeName}` : 'Deleted a tree node';
      break;
    case 'tree_draft_move_node':
      summary = nodeName ? `Moved tree node: ${nodeName}` : 'Moved a tree node';
      break;
    case 'tree_draft_replace_subtree':
      summary = nodeName ? `Replaced subtree: ${nodeName}` : 'Replaced a subtree';
      break;
    case 'program_draft_upsert_parameters':
      summary = summarizeItems('Created or updated', 'program parameter', resultIds);
      break;
    case 'program_draft_remove_parameters':
      summary = summarizeItems('Removed', 'program parameter', resultIds);
      break;
    case 'blackboard_draft_upsert_namespaces':
      summary = summarizeItems('Created or updated', 'blackboard namespace', resultIds);
      break;
    case 'blackboard_draft_remove_namespaces':
      summary = summarizeItems('Removed', 'blackboard namespace', resultIds);
      break;
    case 'blackboard_draft_upsert_keys':
      summary = summarizeItems('Created or updated', 'blackboard key', resultIds);
      break;
    case 'blackboard_draft_remove_keys':
      summary = summarizeItems('Removed', 'blackboard key', resultIds);
      break;
    default:
      summary = call.name.split('.').join(' ');
      break;
  }

  if (call.status === 'failed') {
    return call.error ? `${summary} failed: ${call.error}` : `${summary} failed`;
  }
  if (call.status === 'skipped') {
    return `${summary} skipped`;
  }
  if (call.status === 'proposed') {
    return `${summary} proposed`;
  }
  return summary;
}

function responseActionDetails(response: AssistantChatResponse): { heading: string; items: string[] } | null {
  if (response.toolCalls.length === 0) {
    return null;
  }
  const draftCalls = response.toolCalls.filter((call) => call.name.includes('.draft.'));
  const relevantCalls = response.draftMutated && draftCalls.length > 0 ? draftCalls : response.toolCalls;
  return {
    heading: response.draftMutated ? 'Draft changes' : 'Actions',
    items: relevantCalls.map(toolCallSummary),
  };
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

function isAssistantChatResponse(value: unknown): value is AssistantChatResponse {
  if (!value || typeof value !== 'object') {
    return false;
  }
  const data = value as Partial<AssistantChatResponse>;
  return (
    typeof data.requestId === 'string'
    && typeof data.conversationId === 'string'
    && typeof data.providerId === 'string'
    && Array.isArray(data.proposals)
    && Array.isArray(data.toolCalls)
    && Array.isArray(data.warnings)
    && typeof data.draftMutated === 'boolean'
  );
}

export function AssistantOverlay() {
  const deployment = useDeploymentStore((state) => state.deployment);
  const hydrateLoadedProgram = useProgramStore((state) => state.hydrateLoadedProgram);
  const cycleState = useCycleStore((state) => state.state);

  const [open, setOpen] = useState(false);
  const panelOpen = useViewerSelectionStore((s) => s.activePanel !== null);
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
  const [notice, setNotice] = useState<string | null>(null);
  const [providerCatalog, setProviderCatalog] = useState<AssistantProviderCatalog | null>(null);
  const [providerError, setProviderError] = useState<string | null>(null);
  const [modelEndpointCatalog, setModelEndpointCatalog] = useState<ModelEndpointCatalog | null>(null);
  const [orientationFieldPolicy, setOrientationFieldPolicy] = useState<OrientationFieldKey[]>(
    DEFAULT_ASSISTANT_ORIENTATION_FIELDS,
  );
  const transcriptRef = useRef<HTMLDivElement | null>(null);
  const recoveryAbortRef = useRef<AbortController | null>(null);

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
  // Model the configured provider's bridge is actually running, read live at
  // runtime from the bridge /healthz (decoupled from the deployment model_id /
  // profile_ref). null when the backend's live read was unavailable.
  const liveBridgeModel = providerCatalog?.liveModel ?? null;
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
  // Configured profile (static): the profile_ref / model_id declared in the
  // deployment. This is what the panel used to show as "Model" — hence it was
  // always the deployment's profile_ref label regardless of what was loaded.
  const configuredProfileLabel = modelEndpoint?.profileRef || modelEndpoint?.modelId || 'model not declared';
  // Actually-served model id(s): live-probed via /v1/models (same source the
  // diagnostics panel uses). The assistant endpoint is the nemoclaw bridge,
  // which /model-endpoints does not probe directly, so surface the served id
  // from whichever openai/local endpoint the probe reached — that is the model
  // the bridge's upstream proxy is serving. (llama.cpp/GGUF profiles serve only
  // their own id; vLLM also serves the static cosmos-reason2-8b back-compat
  // alias, so more than one id can appear — both are genuinely served.)
  const liveServedModelIds = useMemo(() => {
    if (!modelEndpointCatalog) return [] as string[];
    const ids = new Set<string>();
    for (const probe of Object.values(modelEndpointCatalog.endpoints)) {
      if (probe.probeStatus === 'ok') {
        for (const id of probe.availableModelIds) {
          ids.add(id);
        }
      }
    }
    return Array.from(ids).sort();
  }, [modelEndpointCatalog]);
  const liveModelLabel = liveServedModelIds.length > 0 ? liveServedModelIds.join(', ') : null;
  // The live bridge model (read at runtime from the bridge /healthz via the
  // providers catalog) is authoritative for what is ACTUALLY running. Prefer
  // it; then the /v1/models probe; then the resolved model_id (the id the
  // bridge requests); then the profile label. Each fallback only applies when
  // the more authoritative source is null, so behavior is unchanged when the
  // live read is unavailable.
  const modelLabel =
    liveBridgeModel
    ?? liveModelLabel
    ?? modelEndpoint?.modelId
    ?? modelEndpoint?.profileRef
    ?? 'model not declared';
  // Whether the displayed model came from a live source (bridge /healthz or the
  // /v1/models probe) rather than a static deployment label.
  const modelIsLive = !!(liveBridgeModel ?? liveModelLabel);
  const activeDuration =
    runState !== 'idle' && activeRequestStartedMs
      ? formatDurationSeconds((nowMs - activeRequestStartedMs) / 1000)
      : formatDurationFromNs(activeStatus?.startedAtNs, activeStatus?.finishedAtNs);
  const orientationFieldSet = useMemo(
    () => new Set<OrientationFieldKey>(orientationFieldPolicy),
    [orientationFieldPolicy],
  );
  const assistantFieldPolicy = useMemo(
    () => ({
      allowedOrientationFields: orientationFieldPolicy,
    }),
    [orientationFieldPolicy],
  );

  const toggleOrientationField = (field: OrientationFieldKey) => {
    if (runState !== 'idle') {
      return;
    }
    const active = orientationFieldSet.has(field);
    if (active && orientationFieldPolicy.length <= 1) {
      return;
    }
    const next = new Set<OrientationFieldKey>(orientationFieldPolicy);
    if (active) {
      next.delete(field);
    } else {
      next.add(field);
    }
    setOrientationFieldPolicy(
      ORIENTATION_FIELD_OPTIONS
        .map((option) => option.key)
        .filter((option): option is OrientationFieldKey => next.has(option)),
    );
  };

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

  // Live model probe — reuses /api/infra/model-endpoints (the same source the
  // diagnostics panel uses) so the assistant panel reflects the actually-served
  // model, not just the configured profile_ref. Refreshed when the overlay
  // opens and whenever the deployment changes.
  useEffect(() => {
    if (!open || !deployment || Object.keys(deployment.modelEndpoints ?? {}).length === 0) {
      setModelEndpointCatalog(null);
      return;
    }
    let cancelled = false;
    void api.infra.modelEndpoints()
      .then((result) => {
        if (!cancelled) {
          setModelEndpointCatalog(result);
        }
      })
      .catch(() => {
        if (!cancelled) {
          setModelEndpointCatalog(null);
        }
      });
    return () => {
      cancelled = true;
    };
  }, [open, deployment]);

  useEffect(() => {
    if (selectedModeId && assistantModes[selectedModeId]?.enabled !== false) {
      return;
    }
    // Compose is the default — operators usually open the assistant
    // to edit, not just to read. Read (query) is one click away.
    const preferred = modeEntries.find(([modeId]) => modeId === 'composer-assistant')
      ?? modeEntries.find(([modeId]) => modeId === 'query')
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
    return () => {
      recoveryAbortRef.current?.abort();
    };
  }, []);

  useEffect(() => {
    if (
      !activeRequestId
      || (runState !== 'running' && runState !== 'cancel_requested')
    ) {
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

  const finishActiveRequest = () => {
    setRunState('idle');
    setActiveRequestId(null);
    setActiveRequestStartedMs(null);
  };

  const refreshMutatedDraft = async () => {
    setAssistantEditedDraft(true);
    try {
      await hydrateLoadedProgram({ retries: 0 });
    } catch (error) {
      const detail = error instanceof Error ? error.message : 'draft refresh failed';
      setNotice(
        `The assistant changed the draft, but the UI could not refresh it: ${detail}. Reload the program before continuing.`,
      );
    }
  };

  const finishDefinitiveRequestFailure = (
    requestId: string,
    error: unknown,
    startedAtNs: number,
  ) => {
    const rawDetail = error instanceof Error ? error.message : 'Assistant request failed';
    const code = (
      (error instanceof ApiError && error.status === 504)
      || rawDetail.toLowerCase().includes('timeout')
    )
      ? 'timeout'
      : 'request_failed';
    const detail = assistantErrorMessage({ code, detail: rawDetail }, rawDetail);
    const finishedAtNs = Date.now() * NS_PER_MS;
    setActiveStatus({
      requestId,
      conversationId: conversationId ?? '',
      providerId,
      assistantMode: selectedModeId,
      status: 'failed',
      message: '',
      error: { code, detail: rawDetail },
      warnings: [],
      draftMutated: false,
      toolCallCount: 0,
      startedAtNs,
      updatedAtNs: finishedAtNs,
      finishedAtNs,
    });
    setNotice(null);
    setLastError(detail);
    setTranscript((items) => items.some((item) => item.id === `${requestId}-error`)
      ? items
      : [
          ...items,
          { kind: 'assistant', id: `${requestId}-error`, text: '', error: detail },
        ]);
    finishActiveRequest();
  };

  const finishRecoveredStatus = async (status: AssistantRequestStatus) => {
    setConversationId(status.conversationId);
    setActiveStatus(status);

    if (status.status === 'completed') {
      const message = status.message || 'Assistant request completed.';
      setLastError(null);
      setNotice(`Connection restored; server result recovered for ${status.requestId}.`);
      setTranscript((items) => items.some(
        (item) => item.id === `${status.requestId}-assistant`,
      )
        ? items
        : [
            ...items,
            {
              kind: 'assistant',
              id: `${status.requestId}-assistant`,
              text: message,
            },
          ]);
      if (status.draftMutated) {
        await refreshMutatedDraft();
      }
    } else {
      const fallback = status.message
        || (status.status === 'cancelled' ? 'Request cancelled' : 'Assistant request failed');
      const error = status.error ?? {
        code: status.status,
        detail: fallback,
      };
      const detail = assistantErrorMessage(error, fallback);
      setNotice(`Connection restored; server result recovered for ${status.requestId}.`);
      setLastError(detail);
      setTranscript((items) => items.some(
        (item) => item.id === `${status.requestId}-error`,
      )
        ? items
        : [
            ...items,
            {
              kind: 'assistant',
              id: `${status.requestId}-error`,
              text: status.message,
              error: detail,
            },
          ]);
      if (status.draftMutated) {
        await refreshMutatedDraft();
      }
    }

    finishActiveRequest();
  };

  const recoverAssistantRequest = async (
    requestId: string,
    originalError?: unknown,
    startedAtNs?: number,
  ) => {
    recoveryAbortRef.current?.abort();
    const controller = new AbortController();
    recoveryAbortRef.current = controller;
    setLastError(null);
    setNotice(`Connection interrupted; checking the server result for ${requestId}.`);
    setRunState('reconciling');

    const result = await reconcileAssistantRequest({
      readStatus: (signal) => api.assistant.status(requestId, signal),
      signal: controller.signal,
      maxNotFoundAttempts: originalError instanceof ApiError ? 1 : undefined,
    });
    if (recoveryAbortRef.current !== controller || result.kind === 'aborted') {
      return;
    }
    recoveryAbortRef.current = null;

    if (result.kind === 'terminal') {
      await finishRecoveredStatus(result.status);
      return;
    }

    if (
      result.kind === 'not_found'
      && !result.requestObserved
      && originalError instanceof ApiError
    ) {
      finishDefinitiveRequestFailure(
        requestId,
        originalError,
        startedAtNs ?? Date.now() * NS_PER_MS,
      );
      return;
    }

    setRunState('outcome_unknown');
    if (result.kind === 'not_found') {
      setNotice(
        `No server record could be found for ${requestId}. The outcome is unknown; use Recheck before resetting or sending the request again.`,
      );
      return;
    }
    if (result.kind === 'exhausted') {
      setNotice(
        `The server did not report a final result for ${requestId} within five minutes. The outcome is unknown; use Recheck, or Reset to cancel and clear it.`,
      );
      return;
    }
    setNotice(
      `The server result for ${requestId} could not be reached: ${result.detail} The outcome is unknown; use Recheck before resetting.`,
    );
  };

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

    let response: AssistantChatResponse;
    try {
      response = await api.assistant.chat({
        requestId,
        conversationId,
        message,
        mode: 'provider',
        providerId,
        assistantMode: selectedModeId,
        fieldPolicy: assistantFieldPolicy,
      });
    } catch (error) {
      if (error instanceof ApiError && isAssistantChatResponse(error.payload)) {
        const errorResponse = error.payload;
        const detail = assistantErrorMessage(errorResponse.error, error.message);
        const finishedAtNs = Date.now() * NS_PER_MS;
        const terminalStatus = errorResponse.error?.code === 'cancelled' ? 'cancelled' : 'failed';
        setConversationId(errorResponse.conversationId);
        setActiveStatus({
          requestId: errorResponse.requestId,
          conversationId: errorResponse.conversationId,
          providerId: errorResponse.providerId,
          assistantMode: errorResponse.assistantMode ?? selectedModeId,
          status: terminalStatus,
          message: errorResponse.message,
          error: errorResponse.error ?? { code: 'request_failed', detail },
          warnings: errorResponse.warnings,
          draftMutated: errorResponse.draftMutated,
          toolCallCount: errorResponse.toolCalls.length,
          startedAtNs,
          updatedAtNs: finishedAtNs,
          finishedAtNs,
        });
        setLastError(detail);
        setTranscript((items) => [
          ...items,
          {
            kind: 'assistant',
            id: `${requestId}-error`,
            text: errorResponse.message,
            response: errorResponse,
            error: detail,
          },
        ]);
        if (errorResponse.draftMutated) {
          await refreshMutatedDraft();
        }
        finishActiveRequest();
        return;
      }

      if (!shouldReconcileAssistantRequest(error)) {
        finishDefinitiveRequestFailure(requestId, error, startedAtNs);
        return;
      }
      await recoverAssistantRequest(requestId, error, startedAtNs);
      return;
    }

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
      await refreshMutatedDraft();
    }
    finishActiveRequest();
  };

  const recheck = async () => {
    if (!activeRequestId || runState !== 'outcome_unknown') {
      return;
    }
    await recoverAssistantRequest(activeRequestId);
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
      const detail = error instanceof Error ? error.message : 'Assistant cancellation failed';
      setLastError(assistantErrorMessage({ code: 'request_failed', detail }, detail));
    }
  };

  // Reset the conversation: cancel any in-flight request (closes ongoing
  // model work), drop the conversationId so the next turn starts a FRESH
  // gateway session (no summary ported), and clear the transcript. Signals the
  // user via a notice banner.
  const resetConversation = async () => {
    const outcomeWasUnknown = runState === 'outcome_unknown';
    recoveryAbortRef.current?.abort();
    recoveryAbortRef.current = null;
    if (runState !== 'idle' && activeRequestId) {
      setRunState('cancel_requested');
      try {
        await api.assistant.cancel(activeRequestId);
      } catch {
        // best-effort: reset proceeds even if the cancel call fails
      }
    }
    setConversationId(undefined);
    setActiveRequestId(null);
    setActiveStatus(null);
    setActiveRequestStartedMs(null);
    setRunState('idle');
    setTranscript([]);
    setLastError(null);
    setNotice(
      outcomeWasUnknown
        ? 'Conversation reset — context cleared; the previous request outcome was not confirmed.'
        : 'Conversation reset — context cleared; any in-flight request was cancelled.',
    );
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
          // Shift left while a right-side overlay panel (Tools / Robot Control /
          // Object) is open so the floating button never overlaps it.
          style={{ transform: panelOpen ? 'translateX(-18.75rem)' : undefined }}
          className="fixed bottom-20 right-20 z-40 flex h-12 w-12 items-center justify-center rounded-full border border-blue-400/70 bg-blue-600 text-sm font-semibold text-white shadow-xl shadow-black/40 transition hover:bg-blue-500 focus:outline-none focus:ring-2 focus:ring-blue-300"
          title="Open assistant"
        >
          AI
        </button>
      )}

      {open && (
        <div className="pointer-events-none fixed inset-x-0 bottom-16 z-40 px-6 pb-4">
          <section className="pointer-events-auto mx-auto flex h-[min(50vh,520px)] min-h-[320px] max-w-[1440px] flex-col overflow-hidden rounded-lg border border-gray-700/90 bg-gray-950/60 text-white shadow-2xl shadow-black/50 backdrop-blur-md">
            <header className="flex shrink-0 flex-wrap items-center gap-3 border-b border-gray-800 bg-gray-950/50 px-4 py-3">
              <div className="flex h-9 w-9 shrink-0 items-center justify-center rounded-full bg-blue-600 text-xs font-semibold">
                AI
              </div>
              <div className="min-w-0 flex-1">
                <div className="flex items-center gap-2">
                  <h2 className="truncate text-sm font-semibold text-gray-100">Assistant</h2>
                  <span className={`h-2 w-2 rounded-full ${statusTone(
                    runState === 'outcome_unknown'
                      ? runState
                      : (activeStatus?.status ?? runState),
                  )}`} />
                  <span className="text-xs text-gray-400">
                    {requestStatusLabel(activeStatus, runState)}
                  </span>
                </div>
                <p className="truncate text-xs text-gray-500">
                  {assistantConfigured
                    ? `${providerSummary} / ${modeStatus.detail}`
                    : providerCatalogReady
                      ? providerSummary
                      : 'Loading assistant provider'}
                </p>
              </div>

              <div
                role="radiogroup"
                aria-label="Assistant mode"
                className="flex max-w-full flex-col gap-1"
              >
                <span className="text-xs text-gray-400">
                  Mode
                  {selectedModeId && (
                    <span className="text-gray-300">
                      {': '}{modeLongLabel(selectedModeId)}
                    </span>
                  )}
                </span>
                {modeEntries.length === 0 ? (
                  <span className="rounded border border-gray-800 bg-gray-900 px-3 py-1 text-xs text-gray-500">
                    No modes
                  </span>
                ) : (
                  <div className="inline-flex rounded border border-gray-700 bg-gray-900 p-0.5">
                    {modeEntries.map(([modeId, modeDef]) => {
                      const active = modeId === selectedModeId;
                      const enabled = modeDef?.enabled !== false;
                      const disabled = runState !== 'idle' || !enabled;
                      return (
                        <button
                          key={modeId}
                          type="button"
                          role="radio"
                          aria-checked={active}
                          onClick={() => {
                            if (!disabled) setSelectedModeId(modeId);
                          }}
                          disabled={disabled}
                          title={modeDef?.description || modeLabel(modeId)}
                          className={`rounded-sm px-3 py-1 text-xs font-medium transition-colors ${
                            active
                              ? 'bg-blue-600 text-white shadow-inner'
                              : 'text-gray-300 hover:bg-gray-800 hover:text-white'
                          } ${disabled ? 'cursor-not-allowed opacity-50 hover:bg-transparent' : ''}`}
                        >
                          {modeLabel(modeId)}
                        </button>
                      );
                    })}
                  </div>
                )}
              </div>

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
                {/* Pinned above the transcript (not inside the scroll area) so this
                    actionable Save/Reload notice stays visible while a draft edit
                    is pending instead of scrolling away with the conversation. */}
                {assistantEditedDraft && (
                  <div className="shrink-0 border-b border-amber-700/60 bg-amber-950/60 px-4 py-2 text-sm text-amber-100">
                    Assistant edited the program. The changes are already applied to the active draft and ready to run; use Save to persist them, or Reload to revert to the last saved version.
                  </div>
                )}
                <div ref={transcriptRef} className="min-h-0 flex-1 space-y-3 overflow-y-auto px-4 py-3">
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
                      const actionDetails =
                        item.kind === 'assistant' && item.response
                          ? responseActionDetails(item.response)
                          : null;
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
                            {actionDetails && (
                              <div className="mt-2 rounded border border-gray-700/70 bg-black/20 px-2 py-2 text-left text-[11px] text-gray-300">
                                <div className="mb-1 font-semibold uppercase tracking-wide text-gray-500">
                                  {actionDetails.heading}
                                </div>
                                <ul className="space-y-1">
                                  {actionDetails.items.map((detail, detailIndex) => (
                                    <li key={`${item.id}-detail-${detailIndex}`}>{detail}</li>
                                  ))}
                                </ul>
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
                  {(notice || (activeStatus?.warnings?.length ?? 0) > 0) && (
                    <div className="mb-2 space-y-1">
                      {notice && (
                        <div className="rounded border border-blue-700/50 bg-blue-950/40 px-3 py-1.5 text-xs text-blue-200">
                          {notice}
                        </div>
                      )}
                      {activeStatus?.warnings?.map((warning, index) => (
                        <div
                          key={`warn-${index}`}
                          className="rounded border border-amber-700/50 bg-amber-950/40 px-3 py-1.5 text-xs text-amber-200"
                        >
                          ⚠ {warning}
                        </div>
                      ))}
                    </div>
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
                    <div className="flex w-24 flex-col gap-2">
                      {runState === 'idle' ? (
                        <button
                          type="button"
                          onClick={() => { setNotice(null); void send(); }}
                          disabled={!canSend}
                          className="rounded bg-blue-600 px-3 py-2 text-sm font-medium text-white hover:bg-blue-500 disabled:cursor-not-allowed disabled:bg-gray-700 disabled:text-gray-400"
                        >
                          Send
                        </button>
                      ) : runState === 'outcome_unknown' ? (
                        <button
                          type="button"
                          onClick={() => void recheck()}
                          className="rounded bg-blue-600 px-3 py-2 text-sm font-medium text-white hover:bg-blue-500"
                        >
                          Recheck
                        </button>
                      ) : (
                        <button
                          type="button"
                          onClick={() => void cancel()}
                          disabled={runState === 'cancel_requested'}
                          className="rounded bg-red-700 px-3 py-2 text-sm font-medium text-white hover:bg-red-600 disabled:cursor-not-allowed disabled:bg-gray-700 disabled:text-gray-400"
                        >
                          Cancel
                        </button>
                      )}
                      <button
                        type="button"
                        onClick={() => void resetConversation()}
                        disabled={runState === 'cancel_requested'
                          || (runState === 'idle' && !conversationId && transcript.length === 0)}
                        title="Reset the conversation: clears context and cancels any in-flight request"
                        className="rounded border border-gray-600 bg-gray-800 px-3 py-1.5 text-xs font-medium text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
                      >
                        Reset
                      </button>
                    </div>
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
                      <span>Profile</span>
                      <span className="truncate text-gray-300">{configuredProfileLabel}</span>
                    </div>
                    <div className="mt-2 flex items-center justify-between gap-2">
                      <span>{modelIsLive ? 'Model (live)' : 'Model'}</span>
                      <span className="truncate text-gray-300">{modelLabel}</span>
                    </div>
                    {activeStatus && (
                      <div className="mt-2 flex items-center justify-between gap-2">
                        <span>Request</span>
                        <span className="truncate text-gray-300">
                          {activeDuration
                            ? `${requestStatusLabel(activeStatus, runState)} / ${activeDuration}`
                            : requestStatusLabel(activeStatus, runState)}
                        </span>
                      </div>
                    )}
                  </div>

                  <div className="mb-2 text-xs font-semibold uppercase tracking-wide text-gray-500">
                    Mode
                  </div>
                  <div className="rounded border border-gray-800 bg-gray-900/70 p-3">
                    <div className="text-sm font-medium text-gray-100">
                      {selectedModeId ? modeLongLabel(selectedModeId) : 'No mode'}
                    </div>
                    <p className="mt-1 text-xs leading-relaxed text-gray-400">
                      {selectedMode?.description || 'No mode is available in this deployment.'}
                    </p>
                    <div className="mt-3 text-xs text-gray-500">
                      {summarizeMode(selectedMode, toolCatalog)}
                    </div>
                  </div>
                </section>

                <section className="mb-4">
                  <div className="mb-2 text-xs font-semibold uppercase tracking-wide text-gray-500">
                    Orientation
                  </div>
                  <div className="grid grid-cols-2 gap-2 rounded border border-gray-800 bg-gray-900/70 p-3">
                    {ORIENTATION_FIELD_OPTIONS.map((option) => {
                      const checked = orientationFieldSet.has(option.key);
                      const disabled = runState !== 'idle' || (checked && orientationFieldPolicy.length <= 1);
                      return (
                        <label
                          key={option.key}
                          title={option.title}
                          className={`flex items-center gap-2 rounded border px-2 py-1.5 text-xs ${
                            checked
                              ? 'border-blue-700/70 bg-blue-950/50 text-blue-100'
                              : 'border-gray-800 bg-gray-950/50 text-gray-400'
                          } ${disabled ? 'cursor-not-allowed opacity-70' : 'cursor-pointer hover:border-gray-600 hover:text-gray-100'}`}
                        >
                          <input
                            type="checkbox"
                            checked={checked}
                            disabled={disabled}
                            onChange={() => toggleOrientationField(option.key)}
                            className="h-3.5 w-3.5 rounded border-gray-600 bg-gray-950 text-blue-600 focus:ring-blue-500"
                          />
                          <span>{option.label}</span>
                        </label>
                      );
                    })}
                  </div>
                </section>

                <section>
                  <div className="mb-2 text-xs font-semibold uppercase tracking-wide text-gray-500">
                    Tool Calls
                  </div>
                  {/* Live tool calls from the in-flight assistant request.
                      The /api/assistant/requests/{id} poll appends entries
                      as the bridge endpoint executes each tool, so the
                      operator sees activity while the run is still
                      generating.
                      2026-05-06: removed the `runState !== 'idle'` guard so
                      the calls remain visible *after* the run completes —
                      until the user submits the next prompt, which resets
                      activeStatus. Without this, OpenClaw-lane runs (whose
                      response.toolCalls is always [] because that lane
                      doesn't surface tool calls in the response envelope)
                      would lose the entire tool-call list as soon as the
                      run finished, leaving the operator unable to inspect
                      what just happened. */}
                  {(activeStatus?.recentToolCalls?.length ?? 0) > 0 && (
                    <>
                      {(activeStatus?.recentToolCalls ?? []).slice(-12).map((call, index) => (
                        <details
                          key={`live-${call.toolId}-${call.startedAtNs}-${index}`}
                          className={`mb-2 rounded border px-2 py-2 text-xs ${liveToolStatusClasses(call.status)}`}
                        >
                          <summary className="flex cursor-pointer list-none items-center justify-between gap-2">
                            <span className="truncate font-medium">{call.toolId}</span>
                            <span className="shrink-0 text-[11px] uppercase">
                              {call.status}{call.draftMutated ? ' • mutated' : ''}
                            </span>
                          </summary>
                          {call.error && (
                            <div className="mt-1 line-clamp-2 text-[11px] opacity-90">
                              {call.error}
                            </div>
                          )}
                          <div className="mt-2 space-y-2 border-t border-current/20 pt-2">
                            {call.argsSummary && (
                              <div>
                                <div className="mb-1 text-[11px] uppercase opacity-70">Args</div>
                                <pre className="max-h-32 overflow-auto whitespace-pre-wrap rounded bg-black/30 p-2 text-[11px] leading-relaxed">
                                  {call.argsSummary}
                                </pre>
                              </div>
                            )}
                            {call.resultSummary && (
                              <div>
                                <div className="mb-1 text-[11px] uppercase opacity-70">Result</div>
                                <pre className="max-h-32 overflow-auto whitespace-pre-wrap rounded bg-black/30 p-2 text-[11px] leading-relaxed">
                                  {call.resultSummary}
                                </pre>
                              </div>
                            )}
                          </div>
                        </details>
                      ))}
                    </>
                  )}
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
                          <span className="truncate font-medium">{toolCallSummary(call)}</span>
                          <span className="shrink-0 text-[11px] uppercase">{call.status}</span>
                        </summary>
                        <div className="mt-1 truncate text-[11px] opacity-70">{call.name}</div>
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
                  )
                    && (activeStatus?.recentToolCalls?.length ?? 0) === 0
                    && (
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
