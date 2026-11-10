import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { reactPropsHost } from '../test/reactPropsHost';
import type { ComponentState } from '../types/components';
import { CellComponentsCard, ComponentStatePanel } from './ComponentStatePanel';

const api = vi.hoisted(() => ({
  action: vi.fn(),
  groupAction: vi.fn(),
  recoverEnablePreflight: vi.fn(),
  recoverEnable: vi.fn(),
}));

vi.mock('../api/client', async (original) => ({
  ...(await original<typeof import('../api/client')>()),
  api: { components: {
    action: api.action, groupAction: api.groupAction,
    recoverEnablePreflight: api.recoverEnablePreflight, recoverEnable: api.recoverEnable,
  } },
}));

afterEach(() => { vi.unstubAllGlobals(); vi.clearAllMocks(); });

function state(patch: Partial<ComponentState> = {}): ComponentState {
  return {
    component_id: 'lite6_robot',
    lifecycle: 'prepared',
    activity: 'idle',
    streams: [],
    capabilities: [],
    observed_at_ns: 1,
    ...patch,
  };
}

describe('ComponentStatePanel', () => {
  it('offers the four lifecycle actions plus the declared action capabilities, never behaviours', () => {
    const html = renderToStaticMarkup(<ComponentStatePanel enabled
      state={state({ capabilities: ['reset_position', 'power_off', 'io_manual_write', 'io_output_readback'] })} />);
    for (const label of ['Prepare', 'Enable', 'Disable', 'Recover', 'Reset position', 'Power off']) {
      expect(html).toContain(`>${label}</button>`);
    }
    expect(html).not.toContain('io manual write');
    expect(html).not.toContain('io output readback');
    // No local state-to-action map: Prepare is offered on a prepared component too.
    expect(html).not.toContain('disabled=""');
  });

  it('offers no Prepare on an enabled component', () => {
    const html = renderToStaticMarkup(<ComponentStatePanel enabled state={state({ lifecycle: 'enabled' })} />);
    expect(html).not.toContain('>Prepare</button>');
    for (const label of ['Enable', 'Disable', 'Recover']) expect(html).toContain(`>${label}</button>`);
  });

  it('shows the fault with its catalogue text, operator action and vendor detail', () => {
    const html = renderToStaticMarkup(<ComponentStatePanel enabled state={state({
      lifecycle: 'fault',
      fault: { code: 'fault_protective_stop', message: 'C31', clearable: true, operator_action: 'recover',
        vendor_code: 'C31', vendor_text: 'Abnormal current', since_ns: 1 },
    })} />);
    expect(html).toContain('Robot stopped itself');
    expect(html).toContain('Check for an obstruction, then Recover.');
    expect(html).toContain('Operator action: Recover');
    expect(html).toContain('Vendor: C31 · Abnormal current');
    expect(html).toContain('status-error');
  });

  it('shows reason, action in progress and the last result; buttons wait for the action', () => {
    const html = renderToStaticMarkup(<ComponentStatePanel enabled state={state({
      lifecycle: 'offline',
      reason: { code: 'shutdown_by_operator', message: 'shut down by the operator' },
      action: { action: 'enable', started_at_ns: 5 },
      last_action: { action: 'prepare', outcome: 'unknown', started_at_ns: 1, finished_at_ns: 2 },
      streams: [{ stream_id: 'joint_states', freshness: 'stale', age_ns: 2_500_000_000 }],
    })} />);
    // The reason's text is the catalogue's title for its code.
    expect(html).toContain('<dt>Reason</dt><dd>Shut down by the operator</dd>');
    expect(html).toContain('<dt>In progress</dt><dd>Enable</dd>');
    expect(html).toContain('Prepare: outcome unknown (not resent)');
    expect(html).toContain('stale · 2.5 s');
    expect(html).toMatch(/<button type="button" disabled="">Prepare<\/button>/);
  });

  it('sends the pressed action once and shows a refusal by its catalogue text', async () => {
    api.action.mockRejectedValueOnce(new Error('busy'));
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<ComponentStatePanel enabled state={state()} />));
    try {
      await act(() => find((props) => props.type === 'button' && props.children === 'Enable').onClick?.());
      expect(api.action).toHaveBeenCalledExactlyOnceWith('lite6_robot', 'enable');
      expect(find((props) => props.role === 'alert').children).toBe('busy');
      // Released on the refusal.
      expect(find((props) => props.type === 'button' && props.children === 'Enable').disabled).toBe(false);
    } finally { await act(() => root.unmount()); }
  });
});

describe('ComponentStatePanel in a machine', () => {
  const owner = state({
    component_id: 'lite6', lifecycle: 'fault', capabilities: ['power_off', 'reboot'],
    fault: { code: 'fault_estop', message: 'C2', clearable: true, operator_action: 'recover',
      concerns: ['lite6_robot_controller_io'], since_ns: 1 },
  });
  const io = state({
    component_id: 'lite6_robot_controller_io', owner_id: 'lite6', role: 'controller_io', lifecycle: 'offline',
    reason: { code: 'owner_unavailable', message: 'through lite6: C2' },
  });
  const arm = state({ component_id: 'lite6_robot', owner_id: 'lite6', role: 'arm', lifecycle: 'offline',
    reason: { code: 'owner_unavailable', message: 'through lite6: C2' } });

  it('offers an owner Prepare, Recover and its power actions only', () => {
    const html = renderToStaticMarkup(<ComponentStatePanel enabled state={owner} states={[owner, io]} />);
    for (const label of ['Prepare', 'Recover', 'Power off', 'Reboot']) expect(html).toContain(`>${label}</button>`);
    expect(html).not.toContain('>Enable</button>');
    expect(html).not.toContain('>Disable</button>');
  });

  it('embedded: rows only, no card, title or State row (the host card shows the state)', () => {
    const html = renderToStaticMarkup(<ComponentStatePanel enabled embedded state={owner} states={[owner, io]} />);
    expect(html).toMatch(/^<div class="component-state-rows" role="group" aria-label="lite6 status">/);
    expect(html).not.toContain('dcard');
    expect(html).not.toContain('<dt>State</dt>');
    expect(html).toContain('<dt>Activity</dt>');
    for (const label of ['Prepare', 'Recover', 'Power off']) expect(html).toContain(`>${label}</button>`);
  });

  it('shows the owner fault as a banner on a concerned member, with the owner\'s reason', () => {
    const html = renderToStaticMarkup(<ComponentStatePanel enabled state={io} states={[owner, io, arm]} />);
    expect(html).toContain('lite6: Emergency stop active');
    expect(html).toContain('Operator action: Recover on lite6');
    expect(html).toContain('<dt>Reason</dt><dd>through lite6: Emergency stop active</dd>');
    // A member it doesn't concern: no banner, still offline through the owner.
    const other = renderToStaticMarkup(<ComponentStatePanel enabled state={arm} states={[owner, io, arm]} />);
    expect(other).not.toContain('role="alert"');
    expect(other).toContain('through lite6: Emergency stop active');
  });
});

describe('ComponentActions power confirmation', () => {
  const owner = state({ component_id: 'lite6', capabilities: ['power_off', 'reboot'] });
  const arm = state({ component_id: 'lite6_robot', owner_id: 'lite6', role: 'arm', lifecycle: 'enabled' });
  const io = state({ component_id: 'lite6_io', owner_id: 'lite6', role: 'controller_io' });

  async function mounted() {
    const { host, find, all } = reactPropsHost();
    (window as unknown as { requestAnimationFrame: (callback: () => void) => number }).requestAnimationFrame = () => 1;
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<ComponentStatePanel enabled state={owner} states={[owner, arm, io]} />));
    const button = (label: string) => find((props) => props.type === 'button' && props.children === label);
    const dialogs = () => all((props) => props.role === 'dialog');
    return { root, find, all, button, dialogs };
  }

  for (const [label, action] of [['Power off', 'power_off'], ['Reboot', 'reboot']] as const) {
    it(`${label} asks first; Cancel sends nothing`, async () => {
      const { root, all, button, dialogs } = await mounted();
      try {
        expect(dialogs()).toHaveLength(0);
        await act(() => button(label).onClick?.());
        expect(api.action).not.toHaveBeenCalled();
        expect(dialogs()).toHaveLength(1);
        expect(all((props) => props.id === `component-lite6-${action}-confirmation-title`)[0]?.children)
          .toBe(`${label} lite6?`);
        const copy = all((props) => typeof props.children === 'string'
          && (props.children as string).startsWith(`${label} disables`)).at(-1);
        // Only the enabled member is named.
        expect(copy?.children).toContain('disables lite6_robot first');
        expect(copy?.children).not.toContain('lite6_io');
        await act(() => button('Cancel').onClick?.());
        expect(dialogs()).toHaveLength(0);
        expect(api.action).not.toHaveBeenCalled();
      } finally { await act(() => root.unmount()); }
    });

    it(`${label} confirmed sends the action once`, async () => {
      api.action.mockResolvedValueOnce({ action });
      const { root, find, button, dialogs } = await mounted();
      try {
        await act(() => button(label).onClick?.());
        await act(() => find((props) => props.type === 'button' && props.className === 'danger-button'
          && props.children === label).onClick?.());
        expect(api.action).toHaveBeenCalledExactlyOnceWith('lite6', action);
        expect(dialogs()).toHaveLength(0);
      } finally { await act(() => root.unmount()); }
    });
  }

  it('other actions run without a confirmation; a machine recovers as a group', async () => {
    // A machine's Recover runs as a group so its unprepared members prepare after it.
    api.groupAction.mockResolvedValueOnce({ action: 'recover' });
    const { root, button, dialogs } = await mounted();
    try {
      await act(() => button('Recover').onClick?.());
      expect(api.groupAction).toHaveBeenCalledExactlyOnceWith('recover', ['lite6']);
      expect(api.action).not.toHaveBeenCalled();
      expect(dialogs()).toHaveLength(0);
    } finally { await act(() => root.unmount()); }
  });
});

describe('ComponentActions busy label', () => {
  it('shows the busy label at once on click and names it on the other actions', async () => {
    let resolve!: () => void;
    api.action.mockReturnValueOnce(new Promise<void>((done) => { resolve = done; }));
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<ComponentStatePanel enabled state={state()} />));
    try {
      act(() => { void find((props) => props.type === 'button' && props.children === 'Disable').onClick?.(); });
      expect(find((props) => props.type === 'button' && props.children === 'Disabling…').disabled).toBe(true);
      const enable = find((props) => props.type === 'button' && props.children === 'Enable');
      expect(enable.disabled).toBe(true);
      expect(enable.title).toBe('Disable lite6_robot is in progress.');
      await act(async () => resolve());
    } finally { await act(() => root.unmount()); }
  });
});

describe('CellComponentsCard', () => {
  it('enables the prepared members and owner-less components, never an owner, and groups members', async () => {
    api.groupAction.mockResolvedValueOnce({ action: 'enable' });
    const { host, find, all } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<CellComponentsCard enabled states={[
      state({ component_id: 'lite6' }),
      state({ component_id: 'lite6_robot', owner_id: 'lite6', role: 'arm' }),
      state({ component_id: 'plc' }),
    ]} />));
    try {
      const summary = all((props) => props.className === 'muted').map((props) => String(props.children)).join();
      expect(summary).toContain('lite6: Prepared (arm lite6_robot: Prepared); plc: Prepared');
      await act(() => find((props) => props.type === 'button' && props.children === 'Enable all').onClick?.());
      expect(api.groupAction).toHaveBeenCalledExactlyOnceWith('enable', ['lite6_robot', 'plc']);
    } finally { await act(() => root.unmount()); }
  });

  it('enables every prepared component with one group action', async () => {
    api.groupAction.mockResolvedValueOnce({ action: 'enable' });
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<CellComponentsCard enabled states={[
      state({ component_id: 'robot' }),
      state({ component_id: 'gripper', lifecycle: 'enabled' }),
      state({ component_id: 'io', lifecycle: 'prepared' }),
    ]} />));
    try {
      await act(() => find((props) => props.type === 'button' && props.children === 'Enable all').onClick?.());
      expect(api.groupAction).toHaveBeenCalledExactlyOnceWith('enable', ['robot', 'io']);
    } finally { await act(() => root.unmount()); }
  });

  it('prepares every not prepared or offline component with one group action', async () => {
    api.groupAction.mockResolvedValueOnce({ action: 'prepare' });
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<CellComponentsCard enabled states={[
      state({ component_id: 'machine', lifecycle: 'not_prepared' }),
      state({ component_id: 'machine_arm', owner_id: 'machine', role: 'arm', lifecycle: 'offline' }),
      state({ component_id: 'gripper', lifecycle: 'prepared' }),
      state({ component_id: 'camera', lifecycle: 'enabled' }),
    ]} />));
    try {
      await act(() => find((props) => props.type === 'button' && props.children === 'Prepare all').onClick?.());
      expect(api.groupAction).toHaveBeenCalledExactlyOnceWith('prepare', ['machine', 'machine_arm']);
    } finally { await act(() => root.unmount()); }
  });

  it('stays busy after the 202 until the pushed state shows the action', async () => {
    api.groupAction.mockResolvedValueOnce({ action: 'enable' });
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    const before = [state({ component_id: 'robot' }), state({ component_id: 'io' })];
    const button = (label: string) => find((props) => props.type === 'button' && props.children === label);
    const enableAll = () => button('Enable all');
    await act(() => root.render(<CellComponentsCard enabled states={before} />));
    try {
      await act(() => enableAll().onClick?.());
      // The pressed button shows its busy label; the other names it.
      expect(button('Enabling…').disabled).toBe(true);
      expect(button('Prepare all').title).toBe('Enable all is in progress.');
      await act(() => root.render(<CellComponentsCard enabled states={[...before]} />));
      expect(button('Enabling…').disabled).toBe(true);
      await act(() => root.render(<CellComponentsCard enabled states={[
        state({ component_id: 'robot', action: { action: 'enable', started_at_ns: 5 } }), before[1]!,
      ]} />));
      expect(enableAll().disabled).toBe(false);
      expect(api.groupAction).toHaveBeenCalledOnce();
    } finally { await act(() => root.unmount()); }
  });

  it('has nothing to prepare when every component is prepared', () => {
    const html = renderToStaticMarkup(<CellComponentsCard enabled states={[state({ lifecycle: 'prepared' })]} />);
    expect(html).toMatch(/disabled=""[^>]*>Prepare all<\/button>/);
  });

  it('has nothing to enable without a prepared component', () => {
    const html = renderToStaticMarkup(<CellComponentsCard enabled states={[state({ lifecycle: 'enabled' })]} />);
    expect(html).toMatch(/disabled=""[^>]*>Enable all<\/button>/);
    expect(renderToStaticMarkup(<CellComponentsCard enabled states={null} />)).toContain('Component states unavailable');
  });
});

describe('Recover and enable (OD-54)', () => {
  const stop = { code: 'estop' as const, message: 'E-stop pressed or not yet cleared', since_ns: 1, vendor_code: 'C2' };
  const machine = state({ component_id: 'lite6', stop });
  const arm = state({ owner_id: 'lite6', reason: { code: 'stopped_by_estop', message: '' } });

  it('labels Enable "Recover and enable" while the machine reports an e-stop, and shows the e-stop', () => {
    const armHtml = renderToStaticMarkup(<ComponentStatePanel enabled state={arm} states={[machine, arm]} />);
    expect(armHtml).toContain('>Recover and enable</button>');
    expect(armHtml).toContain('Stopped by e-stop');
    const machineHtml = renderToStaticMarkup(<ComponentStatePanel enabled state={machine} states={[machine, arm]} />);
    expect(machineHtml).toContain('<dd>E-stop</dd>');
    expect(machineHtml).toContain('E-stop pressed or not yet cleared (C2)');
    // Without a fault or e-stop it stays plain Enable.
    expect(renderToStaticMarkup(<ComponentStatePanel enabled state={state()} />)).toContain('>Enable</button>');
  });

  it('asks with the causes first, then sends exactly what was shown', async () => {
    api.recoverEnablePreflight.mockResolvedValue({ snapshot_id: 's1', components: [
      { component_id: 'lite6', owner_id: null, stop, eligibility: 'recover',
        cause: { code: 'estop', message: 'E-stop', vendor_code: 'C2', vendor_text: null, clearable: true,
          operator_action: 'recover', concerns: ['lite6_robot'] } },
      { component_id: 'lite6_robot', owner_id: 'lite6', cause: null, stop: null, eligibility: 'unknown' }] });
    api.recoverEnable.mockResolvedValue({ action: 'enable' });
    const { host, find, all } = reactPropsHost();
    (window as unknown as { requestAnimationFrame: (callback: () => void) => number }).requestAnimationFrame = () => 1;
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    try {
      await act(() => root.render(<ComponentStatePanel enabled state={arm} states={[machine, arm]} />));
      await act(() => find((props) => props.type === 'button' && props.children === 'Recover and enable').onClick?.());
      await act(async () => { await new Promise((resolve) => { setTimeout(resolve, 0); }); });
      expect(api.action).not.toHaveBeenCalled();
      expect(all((props) => props.id === 'component-lite6_robot-recover-enable-title')[0]?.children)
        .toBe('Recover and enable lite6_robot?');
      expect(all((props) => props.children === 'lite6: E-stop pressed or not yet cleared (C2)')).toHaveLength(1);
      await act(() => find((props) => props.type === 'button' && props.className === 'transport-run'
        && props.children === 'Recover and enable').onClick?.());
      expect(api.recoverEnable).toHaveBeenCalledExactlyOnceWith('lite6_robot', expect.objectContaining({ snapshot_id: 's1' }));
      expect(api.action).not.toHaveBeenCalled();
    } finally { await act(() => root.unmount()); }
  });
});
