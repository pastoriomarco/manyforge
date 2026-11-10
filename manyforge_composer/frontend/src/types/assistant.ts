import type { JsonValue } from './api';

export type AssistantRequestState = 'running' | 'completed' | 'failed' | 'cancelled';

export interface AssistantModeSummary {
  mode: string;
  description: string;
  grantPolicy: {
    approval_mode: string | null;
    review_policy: string;
    session_kind: string | null;
  };
  /** The cell states the mode may run in, e.g. `no_program_active`. */
  runsIn: string[];
}

export interface AssistantModeIndex {
  retainedBundleId: string;
  graphRevision: string;
  /** Null when the bundle has no assistant settings (the index is then empty). */
  assistantProjectionDigest: string | null;
  instrumentRegistryDigest: string;
  /** The active Deployment's AI model component, or null when it has none. */
  aiModelComponentId: string | null;
  /** The active Deployment's assistant component, or null when it has none. */
  assistantComponentId: string | null;
  modes: AssistantModeSummary[];
}

/** The model and catalog the active mode uses (the Applied Deployment's). */
export interface AssistantModeDetail {
  mode: string;
  modelEndpoint: {
    aiModelComponentId: string;
    modelId: string;
    baseUrl: string;
    timeoutS: number | null;
    maxOutputTokens: number | null;
  };
  toolCount: number;
  nodeCount: number;
}

export interface AssistantToolCallResult {
  toolId: string;
  status: string;
  effect: string;
  arguments: Record<string, JsonValue>;
  result?: JsonValue;
  draftMutated?: boolean;
}

export interface AssistantProviderResponse {
  requestId: string;
  requestOwnerEpoch: string;
  sourceLabel: string;
  correlationId: string | null;
  conversationId: string;
  assistantMode: string;
  catalogHash: string;
  message: string;
  toolCalls: AssistantToolCallResult[];
  warnings: string[];
  requiresReview: true;
  mutated: false;
  draftMutated: boolean;
  needsClarification: boolean;
  exitReason: string;
  turnCount: number;
}

export interface AssistantProviderStatus {
  requestId: string;
  requestOwnerEpoch: string;
  sourceLabel: string;
  correlationId: string | null;
  conversationId: string;
  assistantMode: string;
  state: AssistantRequestState;
  response: AssistantProviderResponse | null;
  error: { code: string; message: string } | null;
}

export interface AssistantRequestInput {
  requestId: string;
  requestOwnerEpoch: string;
  sourceLabel: 'composer_ui';
  correlationId: string | null;
  conversationId: string;
  assistantMode: string;
  message: string;
  requestedTools: string[];
  fieldPolicy: { allowedOrientationFields: string[] } | Record<string, never>;
}
