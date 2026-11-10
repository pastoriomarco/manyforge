import { act } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { reactPropsHost, type MountedProps } from '../test/reactPropsHost';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { AssistantModeIndex, AssistantProviderStatus } from '../types/assistant';
import { AssistantOverlay, assistantToolCallSummary } from './AssistantOverlay';

const api = vi.hoisted(() => ({ submit: vi.fn(), status: vi.fn(), cancel: vi.fn(), mode: vi.fn() }));

vi.mock('../api/client', async (original) => ({
  ...(await original<typeof import('../api/client')>()),
  api: { assistant: { submit: api.submit, status: api.status, cancel: api.cancel, mode: api.mode } },
}));

afterEach(() => { vi.unstubAllGlobals(); vi.clearAllMocks(); });

const modeIndex: AssistantModeIndex = {
  retainedBundleId: 'bundle',
  graphRevision: 'graph',
  assistantProjectionDigest: 'projection',
  instrumentRegistryDigest: 'registry',
  aiModelComponentId: 'assistant_model',
  assistantComponentId: 'assistant',
  modes: [{
    mode: 'composer-assistant',
    description: 'Scene and Program authoring mode for Composer: edits land in the Program Draft.',
    grantPolicy: { approval_mode: null, review_policy: 'required', session_kind: null },
    runsIn: ['no_program_active'],
  }],
};

function completed(requestId: string): AssistantProviderStatus {
  return {
    requestId,
    requestOwnerEpoch: 'page',
    sourceLabel: 'composer_ui',
    correlationId: null,
    conversationId: 'conversation',
    assistantMode: 'composer-assistant',
    state: 'completed',
    error: null,
    response: {
      requestId,
      requestOwnerEpoch: 'page',
      sourceLabel: 'composer_ui',
      correlationId: null,
      conversationId: 'conversation',
      assistantMode: 'composer-assistant',
      catalogHash: 'catalog',
      message: 'Added a box on the table.',
      toolCalls: [
        { toolId: 'program_read', status: 'completed', effect: 'read_only', arguments: {} },
        {
          toolId: 'scene_draft_add_object', status: 'completed', effect: 'composer_draft_mutating',
          arguments: { objectId: 'box_1' }, result: { objectIds: ['box_1'] }, draftMutated: true,
        },
      ],
      warnings: [],
      requiresReview: true,
      mutated: false,
      draftMutated: true,
      needsClarification: false,
      exitReason: 'completed',
      turnCount: 3,
    },
  };
}

function texts(all: (match: (props: MountedProps) => boolean) => MountedProps[], className: string): string[] {
  return all((props) => props.className === className).map((props) => String(props.children));
}

describe('AssistantOverlay', () => {
  it('summarizes tool calls the way the 0.1.0 panel did', () => {
    expect(assistantToolCallSummary({
      toolId: 'scene_draft_remove_objects', status: 'completed', effect: 'composer_draft_mutating',
      arguments: {}, result: { objectIds: ['a', 'b'] },
    })).toBe('Removed scene objects: a, b');
    expect(assistantToolCallSummary({
      toolId: 'tree_draft_insert_node', status: 'failed', effect: 'composer_draft_mutating',
      arguments: { instanceId: 'pick_1' }, result: { kind: 'invalid', message: 'unknown node' },
    })).toBe('Inserted node: pick_1 failed: invalid: unknown node');
    expect(assistantToolCallSummary({
      toolId: 'program_draft_upsert_contact_policies', status: 'completed', effect: 'composer_draft_mutating',
      arguments: {}, result: { ids: ['grasp_a', 'grasp_b'] },
    })).toBe('Created or updated contact policies: grasp_a, grasp_b');
    expect(assistantToolCallSummary({
      toolId: 'program_draft_remove_contact_policies', status: 'completed', effect: 'composer_draft_mutating',
      arguments: {}, result: { ids: ['grasp_a'] },
    })).toBe('Removed contact policy: grasp_a');
    expect(assistantToolCallSummary({
      toolId: 'program_validate', status: 'completed', effect: 'read_only', arguments: {},
    })).toBe('Validated the Program Draft');
  });

  it('sends a message, shows the reply with its Draft changes and refreshes the Program', async () => {
    const { host, find, all } = reactPropsHost();
    vi.stubGlobal('window', {
      ...(globalThis.window as object),
      requestAnimationFrame: () => 0,
      setInterval: () => 0,
      clearInterval: () => undefined,
    });
    api.mode.mockResolvedValue({
      mode: 'composer-assistant',
      modelEndpoint: {
        aiModelComponentId: 'assistant_model', modelId: 'qwen38-27b', baseUrl: 'http://127.0.0.1:8001/v1',
        timeoutS: 180, maxOutputTokens: 8192,
      },
      toolCount: 24,
      nodeCount: 23,
    });
    api.submit.mockImplementation(async (request: { requestId: string }) => completed(request.requestId));
    const refresh = vi.fn(async () => undefined);
    const controller = { refresh } as unknown as ProgramWorkspaceController;
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(
      <AssistantOverlay
        open
        onClose={() => undefined}
        unavailableReason={null}
        modeIndex={modeIndex}
        modeError={null}
        onRetryModes={() => undefined}
        programController={controller}
        model={{ componentId: 'assistant_model', model: 'nemotron35-lightning', baseUrl: 'http://127.0.0.1:8000/v1', draftUnapplied: true }}
      />,
    ));
    try {
      await act(() => find((props) => props['aria-label'] === 'Message to the assistant')
        .onChange?.({ target: { value: 'Add a box on the table' } }));
      await act(async () => find((props) => props.type === 'button' && props.children === 'Send').onClick?.());

      expect(api.submit).toHaveBeenCalledOnce();
      expect(api.submit.mock.calls[0][0]).toMatchObject({
        assistantMode: 'composer-assistant',
        message: 'Add a box on the table',
        fieldPolicy: { allowedOrientationFields: expect.arrayContaining(['rpy_deg', 'xyzw']) },
      });
      expect(texts(all, 'assistant-bubble-text')).toEqual(['Add a box on the table', 'Added a box on the table.']);
      expect(refresh).toHaveBeenCalledOnce();
      expect(find((props) => props.className === 'assistant-draft-banner').role).toBe('status');
      expect(all((props) => typeof props.children === 'string'
        && props.children.includes('Apply to use the changes'))).toHaveLength(1);
      // The Applied model, not the Draft's, and the Draft's difference is pointed out.
      expect(find((props) => props.title === 'qwen38-27b').children).toBe('qwen38-27b');
      expect(texts(all, 'assistant-tool-summary')).toEqual(['Read the Program Draft', 'Added scene object: box_1']);
    } finally { await act(() => root.unmount()); }
  });

  it('reports a running chat turn so Execute waits, and clears it when the turn ends', async () => {
    const { host, find } = reactPropsHost();
    vi.stubGlobal('window', {
      ...(globalThis.window as object),
      requestAnimationFrame: () => 0,
      setInterval: () => 0,
      clearInterval: () => undefined,
    });
    api.mode.mockReturnValue(new Promise(() => undefined));
    let answer: (status: AssistantProviderStatus) => void = () => undefined;
    api.submit.mockImplementation(() => new Promise((resolve) => { answer = resolve; }));
    const turnRunning = vi.fn();
    const controller = { refresh: vi.fn(async () => undefined) } as unknown as ProgramWorkspaceController;
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(
      <AssistantOverlay open onClose={() => undefined} unavailableReason={null} modeIndex={modeIndex}
        modeError={null} onRetryModes={() => undefined} onTurnRunningChange={turnRunning}
        programController={controller} model={null} />,
    ));
    try {
      expect(turnRunning).toHaveBeenLastCalledWith(false);
      await act(() => find((props) => props['aria-label'] === 'Message to the assistant')
        .onChange?.({ target: { value: 'Add a box' } }));
      await act(async () => { void find((props) => props.type === 'button' && props.children === 'Send').onClick?.(); });
      expect(turnRunning).toHaveBeenLastCalledWith(true);
      await act(async () => { answer(completed(String(api.submit.mock.calls[0][0].requestId))); });
      expect(turnRunning).toHaveBeenLastCalledWith(false);
    } finally { await act(() => root.unmount()); }
  });

  it('keeps the conversation mounted but refuses to send while unavailable, with the model id in code style', async () => {
    const { host, find } = reactPropsHost();
    vi.stubGlobal('window', { ...(globalThis.window as object), requestAnimationFrame: () => 0 });
    api.mode.mockReturnValue(new Promise(() => undefined));
    const controller = { refresh: vi.fn(async () => undefined) } as unknown as ProgramWorkspaceController;
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    const render = (reason: string | null, notice: string | null) => root.render(
      <AssistantOverlay open onClose={() => undefined} unavailableReason={reason} modeIndex={modeIndex}
        modeError={null} modelNotice={notice} onRetryModes={() => undefined}
        programController={controller} model={null} />,
    );
    await act(() => render('AI model assistant_model is prepared', null));
    try {
      expect(find((props) => props['aria-label'] === 'Message to the assistant').disabled).toBe(true);
      expect(find((props) => props.type === 'button' && props.children === 'Send').disabled).toBe(true);
      expect(find((props) => props.className === 'assistant-dock-subtitle').title)
        .toBe('AI model assistant_model is prepared');
      // The text with the id as a <code> element, not a plain string.
      expect(typeof find((props) => props.className === 'assistant-dock-subtitle').children).toBe('object');
      await act(() => render(null, 'AI model assistant_model: endpoint not answering'));
      expect(find((props) => props['aria-label'] === 'Message to the assistant').disabled).toBe(false);
      expect(find((props) => props.className === 'notice severity-warning').role).toBe('status');
    } finally { await act(() => root.unmount()); }
  });
});
