import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import { reactPropsHost } from '../test/reactPropsHost';
import type { JsonValue } from '../types/api';
import type { MachineCatalogEntry } from '../types/componentCatalog';
import type { ComponentState } from '../types/components';
import { componentManagerRows } from '../utils/componentManager';
import { ComponentPlacementEditor } from './ComponentPlacementEditor';
import { MachineCard } from './MachineCard';

afterEach(() => { vi.unstubAllGlobals(); vi.clearAllMocks(); });

const machine: MachineCatalogEntry = {
  modelId: 'ufactory.lite6_machine', title: 'UFactory Lite 6', lanes: ['mock', 'real'],
  defaultOwnerId: 'lite6', ownerRow: { component_id: 'lite6', component_profile_id: 'ufactory.lite6_machine', configuration: {} },
  members: [
    { role: 'arm', modelId: 'ufactory.lite6', title: 'Arm', row: { component_id: 'lite6_arm', component_profile_id: 'ufactory.lite6' } },
    { role: 'controller_io', modelId: 'ufactory.lite6_controller_io', title: 'Controller I/O',
      row: { component_id: 'lite6_controller_io', component_profile_id: 'ufactory.lite6_controller_io' } },
    { role: 'tool_io', modelId: 'ufactory.lite6_tool_io', title: 'Tool I/O',
      row: { component_id: 'lite6_tool_io', component_profile_id: 'ufactory.lite6_tool_io' } },
  ],
};

function draft(): Record<string, JsonValue> {
  return {
    api_version: 'manyforge.deployment_intent.v2',
    components: [
      { component_id: 'gripper_io', component_profile_id: 'io_device_v0',
        configuration: { runtime_kind: 'io_provider' } },
      { component_id: 'lite6', component_profile_id: 'ufactory.lite6_machine', lane: 'real',
        configuration: { device_address_ref: 'lite6_main' } },
      { component_id: 'lite6_robot', component_profile_id: 'ufactory.lite6', owner: 'lite6', role: 'arm',
        model_source: { urdf: { project_path: 'lite6.urdf' } } },
      { component_id: 'lite6_robot_controller_io', component_profile_id: 'ufactory.lite6_controller_io',
        owner: 'lite6', role: 'controller_io' },
      { component_id: 'lite6_robot_tool_io', component_profile_id: 'ufactory.lite6_tool_io',
        owner: 'lite6', role: 'tool_io', active: false },
    ],
    attachments: [
      { parent: { component_id: 'lite6_robot', interface: 'flange' }, child: { component_id: 'hand', interface: 'mount' } },
    ],
  };
}

function state(patch: Partial<ComponentState>): ComponentState {
  return { component_id: 'x', lifecycle: 'prepared', activity: 'idle', streams: [], capabilities: [], observed_at_ns: 1, ...patch };
}

const owner = state({
  component_id: 'lite6', lifecycle: 'fault', capabilities: ['power_off', 'reboot'],
  fault: { code: 'fault_estop', message: 'C2', clearable: true, operator_action: 'recover',
    concerns: ['lite6_robot_controller_io'], since_ns: 1 },
});
const unavailable = { code: 'owner_unavailable', message: 'through lite6: C2' };
const states: ComponentState[] = [
  owner,
  state({ component_id: 'lite6_robot', owner_id: 'lite6', role: 'arm', lifecycle: 'offline', reason: unavailable }),
  state({ component_id: 'lite6_robot_controller_io', owner_id: 'lite6', role: 'controller_io',
    lifecycle: 'offline', reason: unavailable }),
];

function card(document = draft()) {
  const controller = {
    workspace: { draft: { id: 'draft', revision: 1, document } },
    mutationAvailable: true,
    busy: false,
    loading: false,
    mutate: vi.fn().mockResolvedValue({}),
  } as unknown as DeploymentWorkspaceController;
  const rows = componentManagerRows(document, ['lite6', 'lite6_robot', 'lite6_robot_controller_io']);
  const onRemoved = vi.fn();
  const onSelect = vi.fn();
  const element = (
    <MachineCard owner={rows.find((row) => row.componentId === 'lite6')!}
      members={rows.filter((row) => row.owner === 'lite6')} machine={machine} profiles={{}}
      controller={controller} states={states} enabled selectedId="lite6" onSelect={onSelect} onRemoved={onRemoved} />
  );
  return { controller, element, onRemoved, onSelect };
}

describe('MachineCard', () => {
  it('shows the owner with lane, address and machine actions, and its members inside', () => {
    const html = renderToStaticMarkup(card().element);
    // Header: name, model, the machine-wide state as a badge.
    expect(html).toContain('id="machine-card-title">lite6</h3><small class="machine-card-model">UFactory Lite 6 · ufactory.lite6_machine</small>'
      + '<span class="status-pill status-pill-error">Fault</span>');
    // Fields stacked with their help; no nested cards (the owner's state is rows of this card).
    expect(html).toContain('class="deployment-focused-field stacked"');
    expect(html).toMatch(/role="group" aria-label="lite6 status"/);
    expect(html.match(/class="dcard/g)).toHaveLength(1);
    expect(html).toContain('aria-label="lite6 lane"');
    expect(html).toContain('value="lite6_main"');
    // The owner offers Prepare, Recover and its capabilities; no Enable.
    expect(html).toMatch(/aria-label="lite6 status"[\s\S]*?>Power off<\/button>/);
    for (const id of ['lite6_robot', 'lite6_robot_controller_io', 'lite6_robot_tool_io']) {
      expect(html).toContain(`aria-label="${id} member"`);
    }
    // Members have no lane of their own.
    expect(html).not.toContain('aria-label="lite6_robot lane"');
  });

  it('banners the owner fault on a concerned member; members show the owner\'s reason', () => {
    const html = renderToStaticMarkup(card().element);
    const io = html.slice(html.indexOf('aria-label="lite6_robot_controller_io member"'));
    expect(io).toContain('Operator action: Recover on lite6');
    expect(html).toContain('through lite6: Emergency stop active');
    const arm = html.slice(html.indexOf('aria-label="lite6_robot member"'), html.indexOf('aria-label="lite6_robot_controller_io member"'));
    expect(arm).not.toContain('Recover on lite6');
  });

  it('shows the controller status details read-only on the owner card', () => {
    const plain = renderToStaticMarkup(card().element);
    expect(plain).not.toContain('Controller status');
    states[0] = { ...owner, details: [{ name: 'Mode', value: '1' }, { name: 'Error code', value: 'C2' }] };
    try {
      const html = renderToStaticMarkup(card().element);
      const status = html.slice(html.indexOf('aria-label="lite6 controller status"'));
      expect(status).toContain('<dt>Mode</dt><dd>1</dd>');
      expect(status).toContain('<dt>Error code</dt><dd>C2</dd>');
      expect(html.match(/controller status"/g)).toHaveLength(1);
    } finally { states[0] = owner; }
  });

  it('shows each member as a house row: name over kind, state badge, inline reason and its actions', () => {
    const html = renderToStaticMarkup(card().element);
    const arm = html.slice(html.indexOf('aria-label="lite6_robot member"'), html.indexOf('aria-label="lite6_robot_controller_io member"'));
    expect(arm).toContain('class="dpick component-row" aria-pressed="false"');
    expect(arm).toContain('<strong>lite6_robot</strong><span><small>');
    // Offline through its owner: a yellow badge and the owner's reason inline.
    expect(arm).toContain('<span class="status-pill status-pill-danger">Offline</span>');
    expect(arm).toContain('<p class="inline-message severity-warning">through lite6: Emergency stop active</p>');
    for (const label of ['Prepare', 'Enable', 'Disable', 'Recover']) expect(arm).toContain(`>${label}</button>`);
    expect(arm).toContain('class="dactions machine-member-actions"');
  });

  it('selects a member\'s settings from its row', async () => {
    const { element, onSelect } = card();
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(element));
    try {
      const pick = find((props) => props.type === 'button' && props['aria-pressed'] === false
        && props.title === 'Show its settings below.' && Array.isArray(props.children)
        && props.children.some((child) => (child as { props?: { children?: unknown } })?.props?.children === 'lite6_robot'));
      await act(() => pick.onClick?.());
      expect(onSelect).toHaveBeenCalledWith('lite6_robot');
    } finally { await act(() => root.unmount()); }
  });

  it('takes a site-settings key as the address and refuses an address itself', async () => {
    const { controller, element } = card();
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(element));
    const address = () => find((props) => props['aria-label'] === 'lite6 address');
    try {
      await act(() => address().onChange?.({ target: { value: '192.168.1.10' } }));
      expect(address()['aria-invalid']).toBe(true);
      await act(() => (address().onBlur as () => void)());
      expect(controller.mutate).not.toHaveBeenCalled();
      await act(() => address().onChange?.({ target: { value: 'lite6_cell_b' } }));
      await act(() => (address().onBlur as () => void)());
      expect(controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['components', 1, 'configuration', 'device_address_ref'], 'set', 'lite6_cell_b');
    } finally { await act(() => root.unmount()); }
  });

  it('greys a deactivated member; every member may be deactivated and none removed', () => {
    const html = renderToStaticMarkup(card().element);
    expect(html).toMatch(/class="machine-member component-inactive" role="group" aria-label="lite6_robot_tool_io member"/);
    const tool = html.slice(html.indexOf('aria-label="lite6_robot_tool_io member"'));
    expect(tool).toContain('<span class="status-pill status-pill-off">Off</span>');
    expect(tool).toContain('Deactivated: left out of the cell at the next Apply.');
    expect(tool).not.toContain('>Enable</button>');
    for (const id of ['lite6_robot', 'lite6_robot_controller_io', 'lite6_robot_tool_io']) {
      expect(html).toContain(`aria-label="${id} active"`);
      expect(html).not.toMatch(new RegExp(`aria-label="${id} active"[^>]*disabled=""`));
    }
    expect(html).not.toContain('Remove from Draft');
    expect(html).not.toContain(' · required');
  });

  it('offers no generic Remove on a member\'s page, only on an owner-less component\'s', () => {
    const { controller } = card();
    const withReason = { ...controller, documentUnavailableReason: 'test' } as DeploymentWorkspaceController;
    const rows = componentManagerRows(draft(), []);
    const page = (id: string) => renderToStaticMarkup(
      <ComponentPlacementEditor row={rows.find((row) => row.componentId === id)!} controller={withReason}
        profiles={{}} onRemoved={() => undefined} />);
    expect(page('lite6_robot')).not.toContain('Remove from Draft');
    expect(page('gripper_io')).toContain('Remove from Draft');
  });

  it('deactivates and reactivates a member as Draft edits', async () => {
    const { controller, element } = card();
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(element));
    try {
      await act(() => find((props) => props['aria-label'] === 'lite6_robot_controller_io active')
        .onChange?.({ target: { checked: false } }));
      expect(controller.mutate).toHaveBeenLastCalledWith(['components', 3, 'active'], 'set', false);
      await act(() => find((props) => props['aria-label'] === 'lite6_robot_tool_io active')
        .onChange?.({ target: { checked: true } }));
      expect(controller.mutate).toHaveBeenLastCalledWith(['components', 4, 'active'], 'remove');
    } finally { await act(() => root.unmount()); }
  });

  it('removes the machine with its members and their attachments', async () => {
    const { controller, element, onRemoved } = card();
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(element));
    try {
      await act(() => find((props) => props.type === 'button' && props.children === 'Remove machine from Draft').onClick?.());
      await vi.waitFor(() => expect(onRemoved).toHaveBeenCalledOnce());
      expect(controller.mutate).toHaveBeenNthCalledWith(1, ['attachments'], 'set', []);
      expect(controller.mutate).toHaveBeenNthCalledWith(2, ['components'], 'set',
        [(draft().components as JsonValue[])[0]]);
    } finally { await act(() => root.unmount()); }
  });
});
