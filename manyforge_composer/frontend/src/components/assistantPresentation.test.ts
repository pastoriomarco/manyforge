import { describe, expect, it } from 'vitest';

import { errorCatalogRow } from '../state/commandRefusal';
import type { AssistantProviderStatus } from '../types/assistant';
import type { ComponentState } from '../types/components';
import {
  NO_ASSISTANT_COMPONENT,
  PROGRAM_RUN_EXISTS,
  assistantAvailability,
  assistantFailureText,
  summarizeAssistantToolResult,
} from './AssistantOverlay';

function failed(code: string, message: string): AssistantProviderStatus {
  return {
    requestId: 'request.1',
    requestOwnerEpoch: 'page.1',
    sourceLabel: 'composer_ui',
    correlationId: null,
    conversationId: 'conversation.1',
    assistantMode: 'composer-assistant',
    state: 'failed',
    response: null,
    error: { code, message },
  };
}

describe('assistant operator presentation', () => {
  it('keeps actionable historical error guidance over the current wire', () => {
    expect(assistantFailureText(failed('upstream_timeout', 'model did not answer')))
      .toContain('Try a narrower request');
    expect(assistantFailureText(failed('assistant_projection_stale', 'bundle changed')))
      .toContain('Refresh mode discovery');
    expect(assistantFailureText(failed('ai_model_token_unavailable', 'MY_TOKEN is not set')))
      .toContain('MY_TOKEN is not set');
  });

  it('presents the Composer refusals of a chat turn through the error catalog', () => {
    for (const code of [
      'assistant_component_absent', 'assistant_component_not_enabled',
      'assistant_ai_model_not_enabled', 'assistant_program_active',
    ]) {
      const text = assistantFailureText(failed(code, 'refused by the Composer'));
      expect(text).not.toMatch(new RegExp(`^${code}:`));
      expect(text).toContain(errorCatalogRow(code)?.[0] ?? 'refused by the Composer');
    }
  });

  describe('AI tab availability', () => {
    const index = (assistantComponentId: string | null = 'assistant') => ({
      status: 'ready' as const,
      data: {
        retainedBundleId: 'bundle.1',
        graphRevision: 'graph.1',
        assistantProjectionDigest: assistantComponentId === null ? null : `sha256:${'1'.repeat(64)}`,
        instrumentRegistryDigest: `sha256:${'2'.repeat(64)}`,
        aiModelComponentId: assistantComponentId === null ? null : 'assistant_model',
        assistantComponentId,
        modes: assistantComponentId === null ? [] : [{
          mode: 'composer-assistant',
          description: 'Draft authoring',
          grantPolicy: { approval_mode: null, review_policy: 'required', session_kind: null },
          runsIn: ['no_program_active'],
        }],
      },
    });
    const state = (
      componentId: string,
      lifecycle: ComponentState['lifecycle'],
      endpoint: 'fresh' | 'stale' | null = null,
    ): ComponentState => ({
      component_id: componentId,
      lifecycle,
      activity: 'idle',
      streams: endpoint === null ? [] : [{ stream_id: 'endpoint', freshness: endpoint }],
      capabilities: [],
      observed_at_ns: 1,
    });
    const reason = (components: ComponentState[] | null, runExists = false, modes = index()) => (
      assistantAvailability(modes, components, runExists).unavailableReason);
    const enabled = [state('assistant', 'enabled'), state('assistant_model', 'enabled', 'fresh')];

    it('opens with the assistant and its AI model enabled and no Program run', () => {
      const ready = assistantAvailability(index(), enabled, false);
      expect(ready.unavailableReason).toBeNull();
      expect(ready.modelNotice).toBeNull();
      expect(ready.modeIndex?.assistantComponentId).toBe('assistant');
    });

    it('asks for an Assistant component when the Deployment has none', () => {
      const absent = assistantAvailability(index(null), enabled, false);
      expect(absent.unavailableReason).toBe(NO_ASSISTANT_COMPONENT);
      expect(absent.modeIndex).toBeNull();
      // The absence wins over the run state.
      expect(reason(enabled, true, index(null))).toBe('Add an Assistant component');
    });

    it('is disabled while a Program run exists, before any component reason', () => {
      expect(reason(enabled, true)).toBe(PROGRAM_RUN_EXISTS);
      expect(reason([state('assistant', 'fault')], true))
        .toBe('A Program run exists: the assistant runs only while no run exists');
      // The panel stays mounted (the conversation is kept).
      expect(assistantAvailability(index(), enabled, true).modeIndex).not.toBeNull();
    });

    it('names the assistant component state and the action', () => {
      const model = state('assistant_model', 'enabled');
      expect(reason([state('assistant', 'fault'), model])).toBe('Assistant faulted: Recover it');
      expect(reason([state('assistant', 'prepared'), model])).toBe('Assistant is prepared: Enable it');
      expect(reason([state('assistant', 'not_prepared'), model])).toBe('Assistant is not prepared: Enable it');
      expect(reason([state('assistant', 'offline'), model])).toBe('Assistant is offline');
      expect(reason([model])).toBe('Assistant is offline');
      expect(reason(null)).toBe('Assistant is offline');
    });

    it('names the AI model and its lifecycle while it is not enabled', () => {
      const assistant = state('assistant', 'enabled');
      expect(reason([assistant, state('assistant_model', 'prepared')])).toBe('AI model assistant_model is prepared');
      expect(reason([assistant, state('assistant_model', 'not_prepared')]))
        .toBe('AI model assistant_model is not prepared');
      expect(reason([assistant, state('assistant_model', 'fault')])).toBe('AI model assistant_model is faulted');
      expect(reason([assistant])).toBe('AI model assistant_model is offline');
    });

    it('stays open with a notice while the enabled AI model endpoint is stale', () => {
      const stale = assistantAvailability(index(),
        [state('assistant', 'enabled'), state('assistant_model', 'enabled', 'stale')], false);
      expect(stale.unavailableReason).toBeNull();
      expect(stale.modelNotice).toBe('AI model assistant_model: endpoint not answering');
    });

    it('keeps loading, absent and failed mode reads', () => {
      expect(assistantAvailability({ status: 'loading' }, enabled, false).unavailableReason)
        .toBe('Loading assistant modes…');
      expect(assistantAvailability({ status: 'absent', message: NO_ASSISTANT_COMPONENT }, enabled, false)
        .unavailableReason).toBe(NO_ASSISTANT_COMPONENT);
      const failedRead = assistantAvailability({ status: 'error', message: 'read failed' }, enabled, false);
      expect(failedRead.unavailableReason).toBeNull();
      expect(failedRead.modeError).toBe('read failed');
    });
  });

  it('retains a bounded useful tool-result summary without transcript payloads', () => {
    expect(summarizeAssistantToolResult({
      kind: 'validation_error',
      message: 'Draft revision changed',
      private: 'not copied into the summary',
    })).toBe('validation_error: Draft revision changed');
    expect(summarizeAssistantToolResult('x'.repeat(2_000))).toHaveLength(1_024);
  });
});
