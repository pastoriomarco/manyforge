import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { ApiError } from '../api/client';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import { authoringOperation } from '../state/busyOperation';
import { applyPreviewFixture } from '../test/applyPreview';
import { reactPropsHost } from '../test/reactPropsHost';
import type { ComponentManagerRow } from '../utils/componentManager';
import { ComponentList, ComponentManagerChanges } from './ComponentManager';

afterEach(() => vi.unstubAllGlobals());

const preview = applyPreviewFixture([
  { component_id: 'lite6_b', disposition: 'add', reasons: [], restarts: false },
  { component_id: 'lite6_a', disposition: 'replace', reasons: ['description'], restarts: true },
  { component_id: 'gripper', disposition: 'retain', reasons: [], restarts: false },
]);

const rows: ComponentManagerRow[] = [
  { componentId: 'lite6_b', profileId: 'lite6_mock_commanded_v0', kind: 'robot', mount: null, owner: null, role: null, active: true, change: 'added', depth: 0 },
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
    previewApply: vi.fn().mockResolvedValue(preview),
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
  it('shows the Preview\'s per-component impact; Apply opens the one confirmation', async () => {
    const mounted = await mount();
    try {
      await mounted.click('Preview changes');
      expect(mounted.controller.previewApply).toHaveBeenCalledOnce();
      const shown = texts(mounted.all);
      expect(shown).toContain('Restarts lite6_a; adds lite6_b; the others keep running.');
      expect(shown).toContain('description');
      expect(mounted.button('Apply…').disabled).toBe(false);
      await mounted.click('Apply…');
      expect(mounted.controller.applyDraft).toHaveBeenCalledExactlyOnceWith();
    } finally { await mounted.unmount(); }
  });

  it('shows the running Preview\'s busy label and names it on Apply…, until it returns', async () => {
    const mounted = await mount();
    try {
      Object.assign(mounted.controller, { busy: true, busyOperation: authoringOperation('preview') });
      await mounted.render();
      expect(mounted.button('Previewing…').disabled).toBe(true);
      expect(mounted.button('Apply…').disabled).toBe(true);
      expect(mounted.button('Apply…').title).toBe('Preview is in progress.');
      Object.assign(mounted.controller, { busy: false, busyOperation: null });
      await mounted.render();
      expect(mounted.button('Preview changes').disabled).toBe(false);
    } finally { await mounted.unmount(); }
  });

  it('asks for a new preview when the Draft changed after it', async () => {
    const mounted = await mount();
    try {
      await mounted.click('Preview changes');
      mounted.controller.workspace = workspace(4);
      await mounted.render();
      expect(texts(mounted.all)).toContain('Preview again before Apply');
    } finally { await mounted.unmount(); }
  });

  it('shows a refused preview with its typed text', async () => {
    const mounted = await mount();
    try {
      mounted.controller.previewApply.mockRejectedValueOnce(new ApiError('activation input incomplete', 409,
        { detail: { code: 'activation_input_incomplete', message: 'activation input incomplete' } }));
      await mounted.click('Preview changes');
      expect(texts(mounted.all)).toContain('Scenario is incomplete');
    } finally { await mounted.unmount(); }
  });
});

describe('Component list', () => {
  const listRows: ComponentManagerRow[] = [
    { componentId: 'lite6_robot', profileId: 'lite6_mock_commanded_v0', kind: 'robot', mount: null, owner: null, role: null, active: true, change: 'applied', depth: 0 },
    { componentId: 'lite_gripper', profileId: 'lite_gripper_v0', kind: 'tool', mount: 'lite6_robot.flange', owner: null, role: null, active: true, change: 'added', depth: 1 },
  ];

  it('shows a tool under the robot it is mounted on, and each row\'s pushed state as a badge', () => {
    const markup = renderToStaticMarkup(<ComponentList
      rows={listRows}
      states={{ lite6_robot: { component_id: 'lite6_robot', lifecycle: 'enabled', activity: 'busy',
        streams: [], capabilities: [], observed_at_ns: 1 } }}
      selectedId="lite6_robot"
      onSelect={vi.fn()} />);
    expect(markup).toContain('<strong>lite6_robot</strong><span><small>Robot · lite6_mock_commanded_v0</small><small>busy</small></span>'
      + '<span class="status-pill status-pill-connected">Enabled</span>');
    expect(markup).toContain('style="--component-depth:1"');
    expect(markup).toContain('<small>Tool on flange · lite_gripper_v0</small>');
    expect(markup).toContain('<span class="status-pill">Added</span>');
    expect(markup).toContain('title="Added in Draft · runs after Apply"');
    expect(markup).toContain('aria-pressed="true"');
  });

  it('shows a machine with its members, a deactivated member greyed with its badge', () => {
    const rows: ComponentManagerRow[] = [
      { componentId: 'lite6', profileId: 'ufactory.lite6_machine', kind: 'machine', mount: null, owner: null, role: null, active: true, change: 'applied', depth: 0 },
      { componentId: 'lite6_robot_tool_io', profileId: 'ufactory.lite6_tool_io', kind: 'io_device', mount: null, owner: 'lite6', role: 'tool_io', active: false, change: 'applied', depth: 1 },
    ];
    const markup = renderToStaticMarkup(<ComponentList rows={rows}
      states={{ lite6: { component_id: 'lite6', lifecycle: 'prepared', activity: 'idle',
        streams: [], capabilities: [], observed_at_ns: 1 } }}
      selectedId={null} onSelect={vi.fn()} />);
    expect(markup).toContain('<strong>lite6</strong><span><small>Machine · ufactory.lite6_machine</small></span>'
      + '<span class="status-pill">Prepared</span>');
    expect(markup).toMatch(/class="dpick component-row component-inactive" style="--component-depth:1"/);
    // Deactivated: a dashed Off badge, its meaning in the row's title.
    expect(markup).toContain('<small>I/O device · tool_io</small></span><span class="status-pill status-pill-off">Off</span>');
    expect(markup).toContain('title="Deactivated: it stays in the Deployment and is left out of the cell at the next Apply."');
    expect(markup).toContain('class="drows component-list"');
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
