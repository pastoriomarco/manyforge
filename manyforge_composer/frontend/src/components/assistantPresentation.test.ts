import { describe, expect, it } from 'vitest';

import type { AssistantProviderStatus } from '../types/assistant';
import {
  NO_ASSISTANT_CHAT_MODE,
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
    expect(assistantFailureText(failed('response_rejected', 'tool was outside mode')))
      .toContain('tools available');
    expect(assistantFailureText(failed('assistant_ai_model_absent', 'absent')))
      .toBe('No AI model component in this Deployment');
    expect(assistantFailureText(failed('ai_model_token_unavailable', 'MY_TOKEN is not set')))
      .toContain('MY_TOKEN is not set');
  });

  it('opens the AI tab only with an assistant profile and an AI model component', () => {
    const index = (aiModelComponentId: string | null, modes = ['composer-assistant']) => ({
      status: 'ready' as const,
      data: {
        retainedBundleId: 'bundle.1',
        graphRevision: 'graph.1',
        assistantProjectionDigest: `sha256:${'1'.repeat(64)}`,
        instrumentRegistryDigest: `sha256:${'2'.repeat(64)}`,
        aiModelComponentId,
        modes: modes.map((mode) => ({
          mode,
          description: 'Draft authoring',
          grantPolicy: { approval_mode: null, review_policy: 'required', session_kind: null },
        })),
      },
    });
    expect(assistantAvailability(index('assistant_model')).unavailableReason).toBeNull();
    expect(assistantAvailability(index(null)).unavailableReason)
      .toBe('No AI model component in this Deployment');
    expect(assistantAvailability(index('assistant_model', [])).unavailableReason)
      .toBe(NO_ASSISTANT_CHAT_MODE);
    expect(assistantAvailability({
      status: 'absent',
      message: 'No AI model component in this Deployment',
    }).unavailableReason).toBe('No AI model component in this Deployment');
    const failedRead = assistantAvailability({ status: 'error', message: 'read failed' });
    expect(failedRead.unavailableReason).toBeNull();
    expect(failedRead.modeError).toBe('read failed');
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
