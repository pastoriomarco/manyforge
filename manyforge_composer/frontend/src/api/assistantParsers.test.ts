import { describe, expect, it } from 'vitest';

import {
  parseAssistantModeIndex,
  parseAssistantProviderStatus,
} from './assistantParsers';

function source() {
  return {
    available: true,
    freshness: 'fresh',
    known_gaps: [],
    projection_generation: 1,
    projection_owner_epoch: 'headless.1',
    receiver_age_ns: 1,
    selection_projection_generation: 1,
    selection_source_content_digest: `sha256:${'1'.repeat(64)}`,
    selection_source_generation: 1,
    selection_source_owner_epoch: 'manager.1',
    selection_source_owner_id: 'manager',
    source_content_digest: `sha256:${'2'.repeat(64)}`,
    source_generation: 1,
    source_owner_epoch: 'headless.1',
    source_owner_id: 'headless.assistant-mode-index',
    source_owner_kind: 'assistant_mode_index',
  };
}

function nodeEnvironment() {
  return {
    catalog_digest: `sha256:${'6'.repeat(64)}`,
    catalog_id: 'manyforge',
    content_version: '2',
    environment_digest: `sha256:${'7'.repeat(64)}`,
  };
}

describe('assistant wire parsers', () => {
  it('accepts the exact owner-issued enabled-mode index', () => {
    const parsed = parseAssistantModeIndex({
      schema_version: 'manyforge.assistant_mode_index.v1',
      retained_bundle_id: 'bundle.1',
      graph_revision: 'graph.1',
      assistant_projection_digest: `sha256:${'3'.repeat(64)}`,
      instrument_registry_digest: `sha256:${'4'.repeat(64)}`,
      ai_model_component_id: 'assistant_model',
      node_environment: nodeEnvironment(),
      modes: [{
        mode: 'composer-assistant',
        description: 'Draft authoring',
        grant_policy: {
          approval_mode: null,
          review_policy: 'required',
          session_kind: null,
        },
      }],
      source: source(),
    });
    expect(parsed.modes[0]?.mode).toBe('composer-assistant');
    expect(parsed.aiModelComponentId).toBe('assistant_model');
  });

  it('reports a Deployment without an AI model component as null', () => {
    const parsed = parseAssistantModeIndex({
      schema_version: 'manyforge.assistant_mode_index.v1',
      retained_bundle_id: 'bundle.1',
      graph_revision: 'graph.1',
      assistant_projection_digest: `sha256:${'3'.repeat(64)}`,
      instrument_registry_digest: `sha256:${'4'.repeat(64)}`,
      ai_model_component_id: null,
      node_environment: nodeEnvironment(),
      modes: [],
      source: source(),
    });
    expect(parsed.aiModelComponentId).toBeNull();
  });

  it('rejects stale mode discovery and undeclared fields', () => {
    const stale = source();
    stale.freshness = 'stale';
    expect(() => parseAssistantModeIndex({
      schema_version: 'manyforge.assistant_mode_index.v1',
      retained_bundle_id: 'bundle.1',
      graph_revision: 'graph.1',
      assistant_projection_digest: `sha256:${'3'.repeat(64)}`,
      instrument_registry_digest: `sha256:${'4'.repeat(64)}`,
      ai_model_component_id: null,
      node_environment: nodeEnvironment(),
      modes: [],
      source: stale,
    })).toThrow(/fresh complete/);
  });

  it('keeps terminal response attribution and Draft provenance closed', () => {
    const status = parseAssistantProviderStatus({
      schemaVersion: 'manyforge.assistant.provider_status.v1',
      requestId: 'request.1',
      requestOwnerEpoch: 'page.1',
      sourceLabel: 'composer_ui',
      correlationId: null,
      conversationId: 'conversation.1',
      assistantMode: 'composer-assistant',
      state: 'completed',
      error: null,
      response: {
        schemaVersion: 'manyforge.assistant.provider_response.v1',
        requestId: 'request.1',
        requestOwnerEpoch: 'page.1',
        sourceLabel: 'composer_ui',
        correlationId: null,
        conversationId: 'conversation.1',
        assistantMode: 'composer-assistant',
        catalogHash: `sha256:${'5'.repeat(64)}`,
        message: 'Draft updated.',
        toolCalls: [{
          toolId: 'scene_draft_add_object',
          status: 'completed',
          effect: 'composer_draft_mutating',
          arguments: {},
          result: { draftRevision: 2 },
          draftMutated: true,
        }],
        warnings: [],
        requiresReview: true,
        mutated: false,
        draftMutated: true,
        needsClarification: false,
        exitReason: 'ok',
        turnCount: 2,
      },
    });
    expect(status.response?.draftMutated).toBe(true);
    expect(status.response?.toolCalls[0]?.toolId).toBe('scene_draft_add_object');

    const mismatched = structuredClone(status);
    if (mismatched.response) mismatched.response.assistantMode = 'other-mode';
    expect(() => parseAssistantProviderStatus({
      schemaVersion: 'manyforge.assistant.provider_status.v1',
      requestId: mismatched.requestId,
      requestOwnerEpoch: mismatched.requestOwnerEpoch,
      sourceLabel: mismatched.sourceLabel,
      correlationId: mismatched.correlationId,
      conversationId: mismatched.conversationId,
      assistantMode: mismatched.assistantMode,
      state: mismatched.state,
      response: mismatched.response && {
        schemaVersion: 'manyforge.assistant.provider_response.v1',
        ...mismatched.response,
      },
      error: mismatched.error,
    })).toThrow(/attributed/);

    const excessiveTurns = structuredClone(status);
    if (excessiveTurns.response) excessiveTurns.response.turnCount = 65;
    expect(() => parseAssistantProviderStatus({
      schemaVersion: 'manyforge.assistant.provider_status.v1',
      requestId: excessiveTurns.requestId,
      requestOwnerEpoch: excessiveTurns.requestOwnerEpoch,
      sourceLabel: excessiveTurns.sourceLabel,
      correlationId: excessiveTurns.correlationId,
      conversationId: excessiveTurns.conversationId,
      assistantMode: excessiveTurns.assistantMode,
      state: excessiveTurns.state,
      response: excessiveTurns.response && {
        schemaVersion: 'manyforge.assistant.provider_response.v1',
        ...excessiveTurns.response,
      },
      error: excessiveTurns.error,
    })).toThrow(/between 1 and 64/);
  });
});
