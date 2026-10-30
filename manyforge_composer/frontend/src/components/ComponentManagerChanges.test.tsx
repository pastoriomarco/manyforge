import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  DeploymentDraftChangedError,
  type DeploymentWorkspaceController,
} from '../hooks/useDeploymentWorkspace';
import { reactPropsHost } from '../test/reactPropsHost';
import type { ComponentManagerRow } from '../utils/componentManager';
import { ComponentList, ComponentManagerChanges } from './ComponentManager';

afterEach(() => vi.unstubAllGlobals());

const preview = {
  schemaVersion: 'manyforge.composer_deployment_candidate_operation.v1',
  workspace: {},
  result: {
    status: 'previewed',
    confirmationToken: null,
    preparation: null,
    managerApply: null,
    phasesMs: {},
    applyId: null,
    candidatePreview: {
      converged: false,
      diff: { model_instances: { added: ['lite6_b.model'], changed: [], removed: [] } },
      impact: {
        destructive: false,
        components: [{ controllable_component_id: 'lite6_b', disposition: 'add', replacement_reasons: [] }],
        attachmentChangedComponents: [],
        detachedComponents: [],
        program: { executable: true, unresolvedReferences: [] },
      },
    },
  },
};

const rows: ComponentManagerRow[] = [
  { componentId: 'lite6_b', profileId: 'lite6_mock_commanded_v0', kind: 'robot', mount: null, change: 'added' },
];

function workspace(revision: number) {
  return {
    draft: {
      id: 'draft', revision, document: { api_version: 'manyforge.deployment_intent.v2' },
      deploymentRevision: null, differsFromSaved: true,
    },
    saved: { path: '/projects/cell.scenario', deploymentRevision: null, differsFromApplied: false },
    managerApplied: { activationOrder: null, deploymentRevision: 'sha256:applied', scenarioId: 'cell' },
    runAdmission: { ready: false, reason: 'Deployment Draft differs from Manager Applied' },
  };
}

async function mount() {
  const controller = {
    workspace: workspace(3),
    busy: false,
    applying: false,
    documentUnavailableReason: null,
    mutationUnavailableReason: null,
    previewDraft: vi.fn().mockResolvedValue(preview),
    applyDraft: vi.fn().mockResolvedValue(null),
  };
  const { host, find, all } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  const render = () => act(() => root.render(<ComponentManagerChanges
    controller={controller as unknown as DeploymentWorkspaceController} rows={rows} />));
  await render();
  const button = (text: string) => find((props) => props.type === 'button' && props.children === text);
  return {
    controller, all, button, render,
    click: (text: string) => act(() => button(text).onClick?.()),
    unmount: () => act(() => root.unmount()),
  };
}

/** Every text child of the mounted elements (React elements are skipped). */
function texts(all: ReturnType<typeof reactPropsHost>['all']): string {
  const found: string[] = [];
  const visit = (value: unknown) => {
    if (typeof value === 'string' || typeof value === 'number') found.push(String(value));
    else if (Array.isArray(value)) value.forEach(visit);
  };
  for (const props of all(() => true)) visit(props.children);
  return found.join(' ');
}

describe('Component Manager Changes', () => {
  it('requires a preview of the current Draft before the one Apply', async () => {
    const mounted = await mount();
    try {
      expect(mounted.button('Apply…').disabled).toBe(true);
      expect(String(mounted.button('Apply…').title)).toBe('Preview the current Draft first.');
      await mounted.click('Preview changes');
      expect(mounted.controller.previewDraft).toHaveBeenCalledOnce();
      // The preview lists Manager's dispositions and the compiled record diff.
      expect(texts(mounted.all)).toContain('lite6_b');
      expect(texts(mounted.all)).toContain('model_instances');
      expect(mounted.button('Apply…').disabled).toBe(false);
      await mounted.click('Apply…');
      expect(mounted.controller.applyDraft).toHaveBeenCalledExactlyOnceWith({
        expectedDraft: { draftId: 'draft', draftRevision: 3 },
      });
    } finally { await mounted.unmount(); }
  });

  it('asks for a new preview when the Draft changed after it', async () => {
    const mounted = await mount();
    try {
      await mounted.click('Preview changes');
      mounted.controller.workspace = workspace(4);
      await mounted.render();
      expect(mounted.button('Apply…').disabled).toBe(true);
      expect(texts(mounted.all)).toContain('Preview again before Apply');
    } finally { await mounted.unmount(); }
  });

  it('reports a Draft that changed under the Apply instead of applying it', async () => {
    const mounted = await mount();
    try {
      await mounted.click('Preview changes');
      mounted.controller.applyDraft
        .mockRejectedValueOnce(new DeploymentDraftChangedError('The Deployment Draft changed (revision 4, expected 3).'));
      await mounted.click('Apply…');
      expect(texts(mounted.all)).toContain('Preview again.');
    } finally { await mounted.unmount(); }
  });

  it('shows a refused preview as an error and keeps Apply disabled', async () => {
    const mounted = await mount();
    try {
      mounted.controller.previewDraft
        .mockRejectedValueOnce(new Error('activation input incomplete'));
      await mounted.click('Preview changes');
      expect(texts(mounted.all)).toContain('activation input incomplete');
      expect(mounted.button('Apply…').disabled).toBe(true);
    } finally { await mounted.unmount(); }
  });
});

describe('Component list', () => {
  const listRows: ComponentManagerRow[] = [
    { componentId: 'lite6_robot', profileId: 'lite6_mock_commanded_v0', kind: 'robot', mount: null, change: 'applied' },
    { componentId: 'lite_gripper', profileId: 'lite_gripper_v0', kind: 'tool', mount: 'lite6_robot.flange', change: 'added' },
  ];

  it('shows a tool with its mount on the robot flange, and each row\'s lifecycle and health', () => {
    const markup = renderToStaticMarkup(<ComponentList
      rows={listRows}
      health={{ lite6_robot: { dotClass: 'status-ready', state: 'active', message: 'Ready' } }}
      selectedId="lite6_robot"
      onSelect={vi.fn()} />);
    expect(markup).toContain('lite_gripper → lite6_robot.flange');
    expect(markup).toContain('Tool · lite_gripper_v0');
    expect(markup).toContain('Robot · lite6_mock_commanded_v0');
    expect(markup).toContain('component-dot status-ready');
    expect(markup).toContain('active · Ready');
    expect(markup).toContain('Added in Draft · runs after Apply');
    expect(markup).toContain('aria-pressed="true"');
  });

  it('selects any row, also one that is only in the Draft', async () => {
    const onSelect = vi.fn();
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<ComponentList rows={listRows} selectedId={null} onSelect={onSelect} />));
    try {
      const row = find((props) => props.type === 'button' && props['aria-pressed'] === false
        && String(props.title).includes('Added in Draft'));
      expect(row.disabled).toBeUndefined();
      await act(() => row.onClick?.());
      expect(onSelect).toHaveBeenCalledExactlyOnceWith('lite_gripper');
    } finally { await act(() => root.unmount()); }
  });
});
