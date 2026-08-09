import { describe, expect, it } from 'vitest';

import type { AssistantProviderStatus } from '../types/assistant';
import {
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
      .toContain('selected mode');
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
