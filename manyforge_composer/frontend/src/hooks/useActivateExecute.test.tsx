import { act, useLayoutEffect } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { ActivateExecuteDialog } from '../components/ActivateExecuteDialog';
import { activationOffered, inactiveComponentIds } from '../state/activateExecute';
import { reactPropsHost } from '../test/reactPropsHost';
import type { ComponentState, RunBlockedReason, RunReadiness } from '../types/components';
import { useActivateExecute, type ActivateExecuteController } from './useActivateExecute';

const api = vi.hoisted(() => ({ groupAction: vi.fn(), recoverEnablePreflight: vi.fn(), recoverEnable: vi.fn() }));

vi.mock('../api/client', async (original) => ({
  ...(await original<typeof import('../api/client')>()),
  api: { components: {
    groupAction: api.groupAction, recoverEnablePreflight: api.recoverEnablePreflight, recoverEnable: api.recoverEnable,
  } },
}));

afterEach(() => { vi.unstubAllGlobals(); vi.clearAllMocks(); });

function state(component_id: string, lifecycle: ComponentState['lifecycle'], patch: Partial<ComponentState> = {}): ComponentState {
  return { component_id, lifecycle, activity: 'idle', streams: [], capabilities: [], observed_at_ns: 1, ...patch };
}

// A machine not prepared yet (its arm offline through it) and a tool not enabled.
const member = { owner_id: 'lite6', role: 'arm' };
const down = [
  state('lite6', 'not_prepared'),
  state('lite6_robot', 'offline', { ...member, reason: { code: 'owner_unavailable', message: 'through lite6' } }),
  state('lite_gripper', 'prepared'),
];
const prepared = [state('lite6', 'prepared'), state('lite6_robot', 'prepared', member), state('lite_gripper', 'prepared')];
const enabled = [state('lite6', 'prepared'), state('lite6_robot', 'enabled', member), state('lite_gripper', 'enabled')];

const blocked: RunReadiness = { can_execute: false, can_resume: false, motion_clear: false, blocked_reasons: [
  { code: 'component_not_enabled', message: 'lite6 is not enabled', component_id: 'lite6' },
  { code: 'component_unhealthy', message: 'lite6_robot is offline', component_id: 'lite6_robot' },
  { code: 'component_not_enabled', message: 'lite_gripper is not enabled', component_id: 'lite_gripper' },
] };
const ready: RunReadiness = { can_execute: true, can_resume: false, motion_clear: true, blocked_reasons: [] };

async function mount(states: ComponentState[], readiness: RunReadiness, stopGeneration: number | null = null) {
  const { host } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  const execute = vi.fn(async () => undefined);
  let controller: ActivateExecuteController | null = null;
  function Probe(props: { states: ComponentState[]; readiness: RunReadiness; stopGeneration: number | null; execute: () => Promise<void> }) {
    const value = useActivateExecute(props.states, props.readiness, props.execute, props.stopGeneration);
    useLayoutEffect(() => { controller = value; }, [value]);
    return null;
  }
  let stop = stopGeneration;
  let currentExecute: () => Promise<void> = execute;
  const render = (next: ComponentState[], nextReadiness: RunReadiness = readiness, nextStop: number | null = stop, nextExecute: () => Promise<void> = currentExecute) => act(() => {
    stop = nextStop;
    currentExecute = nextExecute;
    root.render(<Probe states={next} readiness={nextReadiness} stopGeneration={nextStop} execute={nextExecute} />);
  });
  await render(states);
  return {
    get controller() { return controller!; },
    execute,
    render,
    act: (run: (value: ActivateExecuteController) => void) => act(() => run(controller!)),
    unmount: () => act(() => root.unmount()),
  };
}

describe('Activate and execute: when it is offered', () => {
  it('when components Activate brings up block Execute, besides holds they may cause', () => {
    expect(activationOffered(blocked, down)).toBe(true);
    // A hold or stale input may only be due to the down I/O components: offered.
    const holds: RunBlockedReason[] = [
      { code: 'rule_holds', message: 'a signal is not fresh', rule_id: 'light_curtain', scope: 'all_motion' },
      { code: 'input_not_fresh', message: 'stale', signal_id: 'part_present' },
    ];
    expect(activationOffered({ ...blocked, blocked_reasons: [
      ...blocked.blocked_reasons, ...holds,
    ] }, down)).toBe(true);
    // Nothing to activate: not offered.
    expect(activationOffered({ ...blocked, blocked_reasons: holds }, enabled)).toBe(false);
    // Offline for its own reason (e.g. after an execution host restart): Prepare all prepares it.
    expect(activationOffered(blocked, [down[0]!, state('lite6_robot', 'offline', member), down[2]!])).toBe(true);
    // A fault without a cause isn't something Activate brings up; a latch isn't either.
    expect(activationOffered(blocked, [down[0]!, state('lite6_robot', 'fault', member), down[2]!])).toBe(false);
    // A fault or e-stop Recover and enable may clear is (OD-54); the dialog shows the causes.
    const fault = { code: 'fault_collision', message: 'Collision', clearable: true, operator_action: 'recover' as const, since_ns: 1 };
    expect(activationOffered(blocked, [down[0]!, state('lite6_robot', 'fault', { ...member, fault }), down[2]!])).toBe(true);
    expect(activationOffered({ ...blocked, blocked_reasons: [
      ...blocked.blocked_reasons, { code: 'cell_latched', message: 'lite6_robot: offline' },
    ] }, down)).toBe(false);
    expect(activationOffered(ready, enabled)).toBe(false);
    expect(activationOffered(blocked, null)).toBe(false);
    expect(inactiveComponentIds(down)).toEqual(['lite6', 'lite6_robot', 'lite_gripper']);
    // A prepared owner has no Enable: it is ready.
    expect(inactiveComponentIds(enabled)).toEqual([]);
  });
});

describe('Activate and execute', () => {
  it('prepares (owners first), enables, then executes once when readiness allows', async () => {
    api.groupAction.mockResolvedValue({ action: 'accepted' });
    const mounted = await mount(down, blocked);
    try {
      expect(mounted.controller.available).toBe(true);
      await mounted.act((value) => value.open());
      expect(mounted.controller.flow).toEqual({
        phase: 'offered', componentIds: ['lite6', 'lite6_robot', 'lite_gripper'], failures: [], recovery: {},
      });
      const dialog = renderToStaticMarkup(<ActivateExecuteDialog activation={mounted.controller} />);
      expect(dialog).toContain('Activate components and execute?');
      expect(dialog).toContain('Activate prepares and enables these components, then executes the Program once.');
      expect(dialog).toContain('It never starts the simulator');
      expect(dialog).toContain('lite6_robot (Offline · through lite6: Not prepared)');
      await mounted.act((value) => value.confirm());
      expect(api.groupAction).toHaveBeenLastCalledWith('prepare', ['lite6', 'lite6_robot']);
      expect(mounted.controller.flow?.phase).toBe('prepare');
      await mounted.render(prepared);
      expect(api.groupAction).toHaveBeenLastCalledWith('enable', ['lite6_robot', 'lite_gripper']);
      expect(mounted.controller.flow?.phase).toBe('enable');
      // Every component up; readiness not pushed yet: still waiting.
      await mounted.render(enabled, { ...blocked, blocked_reasons: [blocked.blocked_reasons[2]!] });
      expect(mounted.controller.flow?.phase).toBe('waiting');
      expect(mounted.execute).not.toHaveBeenCalled();
      await mounted.render(enabled, ready);
      expect(mounted.execute).toHaveBeenCalledOnce();
      expect(mounted.controller.flow).toBeNull();
      expect(api.groupAction).toHaveBeenCalledTimes(2);
      expect(api.groupAction.mock.invocationCallOrder[1]!).toBeLessThan(mounted.execute.mock.invocationCallOrder[0]!);
      // Later pushes (readiness still can_execute before the run shows): no second Execute.
      await mounted.render([...enabled], { ...ready });
      await mounted.render(enabled, { ...ready });
      expect(mounted.execute).toHaveBeenCalledOnce();
    } finally { await mounted.unmount(); }
  });

  it('uses the latest Execute callback after a committed rerender', async () => {
    api.groupAction.mockResolvedValue({ action: 'accepted' });
    const mounted = await mount(down, blocked);
    const replacement = vi.fn(async () => undefined);
    try {
      await mounted.act((value) => value.open());
      await mounted.act((value) => value.confirm());
      await mounted.render(prepared, blocked, null, replacement);
      await mounted.render(enabled, ready);
      expect(mounted.execute).not.toHaveBeenCalled();
      expect(replacement).toHaveBeenCalledOnce();
    } finally { await mounted.unmount(); }
  });

  it('omits both paused and offline simulators from activation targets without blocking Execute', async () => {
    api.groupAction.mockResolvedValue({ action: 'accepted' });
    const offlineRobot = state('lite6_robot', 'offline', {
      ...member, reason: { code: 'owner_unavailable', message: 'through lite6' },
    });
    const pausedSimulator = state('isaac_sim', 'prepared', { capabilities: ['sim_reset'] });
    const offlineSimulator = state('offline_sim', 'offline', { capabilities: ['sim_reset'] });
    const initial = [prepared[0]!, offlineRobot, prepared[2]!, pausedSimulator, offlineSimulator];
    const robotBlocked: RunReadiness = {
      ...blocked,
      blocked_reasons: [{ code: 'component_unhealthy', message: 'lite6_robot is offline', component_id: 'lite6_robot' }],
    };
    const mounted = await mount(initial, robotBlocked);
    try {
      await mounted.act((value) => value.open());
      await mounted.act((value) => value.confirm());
      expect(api.groupAction).toHaveBeenCalledExactlyOnceWith('prepare', ['lite6_robot']);

      const robotPrepared = state('lite6_robot', 'prepared', { ...member, last_action: {
        action: 'prepare', outcome: 'succeeded', started_at_ns: 5, finished_at_ns: 6,
      } });
      await mounted.render([prepared[0]!, robotPrepared, prepared[2]!, pausedSimulator, offlineSimulator]);
      expect(api.groupAction).toHaveBeenLastCalledWith('enable', ['lite6_robot', 'lite_gripper']);

      const completed = [
        prepared[0]!,
        state('lite6_robot', 'enabled', { ...member, last_action: {
          action: 'enable', outcome: 'succeeded', started_at_ns: 7, finished_at_ns: 8,
        } }),
        state('lite_gripper', 'enabled', { last_action: {
          action: 'enable', outcome: 'succeeded', started_at_ns: 7, finished_at_ns: 8,
        } }),
        pausedSimulator,
        offlineSimulator,
      ];
      await mounted.render(completed, ready);
      expect(mounted.controller.flow).toBeNull();
      expect(mounted.execute).toHaveBeenCalledOnce();
    } finally { await mounted.unmount(); }
  });

  it('with only a rule hold left after activation, Execute is sent and the server decides', async () => {
    api.groupAction.mockResolvedValue({ action: 'accepted' });
    const hold: RunBlockedReason = {
      code: 'rule_holds', message: 'a signal is not fresh', rule_id: 'light_curtain', scope: 'all_motion',
    };
    const held: RunReadiness = { ...blocked, blocked_reasons: [...blocked.blocked_reasons, hold] };
    const mounted = await mount(down, held);
    try {
      expect(mounted.controller.available).toBe(true);
      await mounted.act((value) => value.open());
      await mounted.act((value) => value.confirm());
      await mounted.render(prepared);
      // A component still not enabled: keep waiting, the hold isn't a failure.
      await mounted.render(enabled, { ...blocked, blocked_reasons: [blocked.blocked_reasons[2]!, hold] });
      expect(mounted.controller.flow?.phase).toBe('waiting');
      expect(mounted.execute).not.toHaveBeenCalled();
      await mounted.render(enabled, { ...blocked, blocked_reasons: [hold] });
      expect(mounted.execute).toHaveBeenCalledOnce();
      expect(mounted.controller.flow).toBeNull();
    } finally { await mounted.unmount(); }
  });

  it('a failing Enable shows the component and its typed error, and never executes', async () => {
    api.groupAction.mockResolvedValue({ action: 'accepted' });
    const mounted = await mount(prepared, blocked);
    try {
      await mounted.act((value) => value.open());
      await mounted.act((value) => value.confirm());
      // Nothing to prepare: straight to Enable all.
      expect(api.groupAction).toHaveBeenCalledExactlyOnceWith('enable', ['lite6_robot', 'lite_gripper']);
      await mounted.render([prepared[0]!, state('lite6_robot', 'enabled', member), state('lite_gripper', 'prepared', {
        last_action: { action: 'enable', outcome: 'failed', started_at_ns: 5, finished_at_ns: 6,
          error: { code: 'fault_estop', message: 'C2' } },
      })]);
      expect(mounted.controller.flow?.phase).toBe('failed');
      expect(mounted.controller.flow?.failures).toEqual([
        { componentId: 'lite_gripper', text: 'Enable failed: Emergency stop active' },
      ]);
      const dialog = renderToStaticMarkup(<ActivateExecuteDialog activation={mounted.controller} />);
      expect(dialog).toContain('Activation stopped; the Program was not executed.');
      expect(dialog).toContain('Enable failed: Emergency stop active');
      await mounted.render(enabled, ready);
      expect(mounted.execute).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
  });

  it('judges a component by its Enable\'s result, not by a fault while that Enable runs', async () => {
    api.groupAction.mockResolvedValue({ action: 'accepted' });
    const mounted = await mount(prepared, blocked);
    try {
      await mounted.act((value) => value.open());
      await mounted.act((value) => value.confirm());
      expect(api.groupAction).toHaveBeenCalledExactlyOnceWith('enable', ['lite6_robot', 'lite_gripper']);
      const enabling = { action: { action: 'enable' as const, started_at_ns: 5 } };
      // The arming briefly stops the joint states: a stale-stream fault inside the Enable.
      await mounted.render([prepared[0]!, state('lite6_robot', 'fault', {
        ...member, ...enabling, fault: { code: 'fault_stream_stale', message: 'The measured_joints stream stopped arriving',
          clearable: true, operator_action: 'recover', since_ns: 6 },
      }), state('lite_gripper', 'enabled')]);
      expect(mounted.controller.flow?.phase).toBe('enable');
      await mounted.render([prepared[0]!, state('lite6_robot', 'prepared', { ...member, ...enabling }), state('lite_gripper', 'enabled')]);
      expect(mounted.controller.flow?.phase).toBe('enable');
      // The Enable succeeded and the component is enabled: on to Execute.
      const succeeded = { last_action: { action: 'enable' as const, outcome: 'succeeded' as const, started_at_ns: 5, finished_at_ns: 9 } };
      await mounted.render([prepared[0]!, state('lite6_robot', 'enabled', { ...member, ...succeeded }), state('lite_gripper', 'enabled')], ready);
      expect(mounted.execute).toHaveBeenCalledOnce();
      expect(mounted.controller.flow).toBeNull();
    } finally { await mounted.unmount(); }
  });

  it('a fault still present after the component\'s own Enable ended stops it', async () => {
    api.groupAction.mockResolvedValue({ action: 'accepted' });
    const mounted = await mount(prepared, blocked);
    try {
      await mounted.act((value) => value.open());
      await mounted.act((value) => value.confirm());
      await mounted.render([prepared[0]!, state('lite6_robot', 'fault', {
        ...member, fault: { code: 'fault_stream_stale', message: 'The measured_joints stream stopped arriving',
          clearable: true, operator_action: 'recover', since_ns: 6 },
        last_action: { action: 'enable', outcome: 'succeeded', started_at_ns: 5, finished_at_ns: 9 },
      }), state('lite_gripper', 'enabled')]);
      expect(mounted.controller.flow?.phase).toBe('failed');
      expect(mounted.controller.flow?.failures).toEqual([{ componentId: 'lite6_robot', text: 'Device stopped reporting' }]);
      expect(mounted.execute).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
  });

  it('a transient fault on a target still waiting its turn doesn\'t stop it', async () => {
    api.groupAction.mockResolvedValue({ action: 'accepted' });
    const mounted = await mount(prepared, blocked);
    try {
      await mounted.act((value) => value.open());
      await mounted.act((value) => value.confirm());
      expect(api.groupAction).toHaveBeenCalledExactlyOnceWith('enable', ['lite6_robot', 'lite_gripper']);
      const stale = { fault: { code: 'fault_stream_stale', message: 'The gripper stream stopped arriving',
        clearable: true, operator_action: 'recover' as const, since_ns: 6 } };
      const armDone = { last_action: { action: 'enable' as const, outcome: 'succeeded' as const, started_at_ns: 5, finished_at_ns: 9 } };
      // The arm's Enable switches the hardware: the waiting gripper shows a stale stream.
      await mounted.render([prepared[0]!, state('lite6_robot', 'prepared', {
        ...member, action: { action: 'enable', started_at_ns: 5 },
      }), state('lite_gripper', 'fault', stale)]);
      expect(mounted.controller.flow?.phase).toBe('enable');
      // The arm's Enable ended; the gripper hasn't had its turn yet, still faulted.
      await mounted.render([prepared[0]!, state('lite6_robot', 'enabled', { ...member, ...armDone }),
        state('lite_gripper', 'fault', stale)]);
      expect(mounted.controller.flow?.phase).toBe('enable');
      // The fault clears before its turn; then its own Enable runs and succeeds.
      await mounted.render([prepared[0]!, state('lite6_robot', 'enabled', { ...member, ...armDone }),
        state('lite_gripper', 'prepared', { action: { action: 'enable', started_at_ns: 10 } })]);
      expect(mounted.controller.flow?.phase).toBe('enable');
      await mounted.render([prepared[0]!, state('lite6_robot', 'enabled', { ...member, ...armDone }),
        state('lite_gripper', 'enabled', { last_action: { action: 'enable', outcome: 'succeeded', started_at_ns: 10, finished_at_ns: 11 } })], ready);
      expect(mounted.execute).toHaveBeenCalledOnce();
      expect(mounted.controller.flow).toBeNull();
    } finally { await mounted.unmount(); }
  });

  it('a waiting target\'s fault that persists after its own action stops it', async () => {
    api.groupAction.mockResolvedValue({ action: 'accepted' });
    const mounted = await mount(prepared, blocked);
    try {
      await mounted.act((value) => value.open());
      await mounted.act((value) => value.confirm());
      const stale = { fault: { code: 'fault_stream_stale', message: 'The gripper stream stopped arriving',
        clearable: true, operator_action: 'recover' as const, since_ns: 6 } };
      const armDone = { last_action: { action: 'enable' as const, outcome: 'succeeded' as const, started_at_ns: 5, finished_at_ns: 9 } };
      await mounted.render([prepared[0]!, state('lite6_robot', 'enabled', { ...member, ...armDone }),
        state('lite_gripper', 'fault', stale)]);
      expect(mounted.controller.flow?.phase).toBe('enable');
      // Its own Enable ran and ended; the fault is still there: a failure.
      await mounted.render([prepared[0]!, state('lite6_robot', 'enabled', { ...member, ...armDone }),
        state('lite_gripper', 'fault', { ...stale,
          last_action: { action: 'enable', outcome: 'succeeded', started_at_ns: 10, finished_at_ns: 11 } })]);
      expect(mounted.controller.flow?.phase).toBe('failed');
      expect(mounted.controller.flow?.failures).toEqual([{ componentId: 'lite_gripper', text: 'Device stopped reporting' }]);
      expect(mounted.execute).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
  });

  it('a Stop that ends the group before a target\'s turn stops it, faulted or not', async () => {
    api.groupAction.mockResolvedValue({ action: 'accepted' });
    const io = state('lite_io', 'prepared', { owner_id: 'lite6', role: 'controller_io' });
    const mounted = await mount([...prepared, io], blocked, 3);
    try {
      await mounted.act((value) => value.open());
      await mounted.act((value) => value.confirm());
      expect(api.groupAction).toHaveBeenCalledExactlyOnceWith('enable', ['lite6_robot', 'lite_gripper', 'lite_io']);
      const stale = { fault: { code: 'fault_stream_stale', message: 'The gripper stream stopped arriving',
        clearable: true, operator_action: 'recover' as const, since_ns: 6 } };
      const armDone = { last_action: { action: 'enable' as const, outcome: 'succeeded' as const, started_at_ns: 5, finished_at_ns: 9 } };
      const after = [prepared[0]!, state('lite6_robot', 'enabled', { ...member, ...armDone }),
        state('lite_gripper', 'fault', stale), io];
      // Between targets, no Stop: waiting for the gripper's turn.
      await mounted.render(after);
      expect(mounted.controller.flow?.phase).toBe('enable');
      // A Stop ends the group: the gripper and the I/O never get their Enable.
      await mounted.render(after, blocked, 4);
      expect(mounted.controller.flow?.phase).toBe('failed');
      expect(mounted.controller.flow?.failures).toEqual([
        { componentId: 'lite_gripper', text: 'Device stopped reporting' },
        { componentId: 'lite_io', text: 'Component action interrupted by Stop' },
      ]);
      await mounted.render(enabled, ready, 4);
      expect(mounted.execute).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
  });

  it('shows a refused group action with its typed text', async () => {
    const { ApiError } = await import('../api/client');
    api.groupAction.mockRejectedValueOnce(new ApiError('latched', 409,
      { detail: { code: 'cell_latched', message: 'latched' } }));
    const mounted = await mount(down, blocked);
    try {
      await mounted.act((value) => value.open());
      await mounted.act((value) => value.confirm());
      expect(mounted.controller.flow?.phase).toBe('failed');
      expect(mounted.controller.flow?.failures[0]?.text).toContain('Cell latched');
    } finally { await mounted.unmount(); }
  });

  it('Cancel stops it: no further action, no Execute', async () => {
    api.groupAction.mockResolvedValue({ action: 'accepted' });
    const mounted = await mount(down, blocked);
    try {
      await mounted.act((value) => value.open());
      await mounted.act((value) => value.confirm());
      await mounted.act((value) => value.cancel());
      expect(mounted.controller.flow).toBeNull();
      await mounted.render(prepared);
      await mounted.render(enabled, ready);
      expect(api.groupAction).toHaveBeenCalledOnce();
      expect(mounted.execute).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
  });
});

const stop = { code: 'estop' as const, message: 'E-stop pressed or not yet cleared', since_ns: 1, vendor_code: 'C2' };
const estopped = [
  state('lite6', 'prepared', { stop }),
  state('lite6_robot', 'prepared', { ...member, reason: { code: 'stopped_by_estop', message: '' } }),
  state('lite_gripper', 'prepared', { owner_id: 'lite6', role: 'tool', reason: { code: 'stopped_by_estop', message: '' } }),
];
const estopBlocked: RunReadiness = { ...blocked, blocked_reasons: [
  { code: 'component_unhealthy', message: 'lite6 e-stop', component_id: 'lite6' },
  { code: 'component_not_enabled', message: 'not enabled', component_id: 'lite6_robot' },
  { code: 'component_not_enabled', message: 'not enabled', component_id: 'lite_gripper' },
] };
const ownerRow = { component_id: 'lite6', owner_id: null, stop, eligibility: 'recover' as const,
  cause: { code: 'estop', message: 'E-stop', vendor_code: 'C2', vendor_text: null, clearable: true,
    operator_action: 'recover' as const, concerns: ['lite6_robot', 'lite_gripper'] } };
const memberRow = (component_id: string, eligibility: 'unknown' | 'ready' = 'unknown') => (
  { component_id, owner_id: 'lite6', cause: null, stop: null, eligibility });
const flush = () => act(async () => { await new Promise((resolve) => { setTimeout(resolve, 0); }); });

describe('Activate and execute: Recover and enable first (OD-54)', () => {
  it('shows what needs recovery, recovers and enables one at a time, enables the rest, then executes', async () => {
    api.recoverEnable.mockResolvedValue({ action: 'enable' });
    api.recoverEnablePreflight.mockImplementation(async (id: string) => ({ snapshot_id: `s-${id}`,
      components: [ownerRow, memberRow(id)] }));
    const mounted = await mount(estopped, estopBlocked);
    try {
      expect(mounted.controller.available).toBe(true);
      await mounted.act((value) => value.open());
      await flush();
      expect(mounted.controller.recoveryReady).toBe(true);
      const dialog = renderToStaticMarkup(<ActivateExecuteDialog activation={mounted.controller} />);
      expect(dialog).toContain('Needs recovery:');
      expect(dialog).toContain('lite6: E-stop pressed or not yet cleared (C2)');
      expect(dialog).toContain('Recover, enable and execute');
      expect(dialog).toContain('neither runs a Program motion');
      await mounted.act((value) => value.confirm());
      await flush();
      expect(api.recoverEnable).toHaveBeenLastCalledWith('lite6_robot', { snapshot_id: 's-lite6_robot', confirmed: [
        { component_id: 'lite6', cause_code: 'estop', vendor_code: 'C2', stop_code: 'estop', stop_vendor_code: 'C2', eligibility: 'recover' },
        { component_id: 'lite6_robot', cause_code: null, vendor_code: null, stop_code: null, stop_vendor_code: null, eligibility: 'unknown' },
      ] });
      expect(mounted.controller.flow?.phase).toBe('recover');
      // The machine recovered with the arm: the gripper now has nothing to recover, so it stays within what was confirmed.
      api.recoverEnablePreflight.mockImplementation(async (id: string) => ({ snapshot_id: `t-${id}`,
        components: [{ ...ownerRow, stop: null, cause: null, eligibility: 'ready' }, memberRow(id, 'ready')] }));
      const armDone = [state('lite6', 'prepared'),
        state('lite6_robot', 'enabled', { ...member, last_action: { action: 'enable', outcome: 'succeeded', started_at_ns: 5, finished_at_ns: 6,
          phases: [{ component_id: 'lite6', action: 'recover', outcome: 'succeeded' }, { component_id: 'lite6_robot', action: 'enable', outcome: 'succeeded' }] } }),
        state('lite_gripper', 'prepared', { owner_id: 'lite6', role: 'tool' })];
      await mounted.render(armDone);
      await flush();
      expect(api.recoverEnable).toHaveBeenLastCalledWith('lite_gripper', expect.objectContaining({ snapshot_id: 't-lite_gripper' }));
      const allDone = [armDone[0]!, armDone[1]!, state('lite_gripper', 'enabled', { owner_id: 'lite6', role: 'tool',
        last_action: { action: 'enable', outcome: 'succeeded', started_at_ns: 7, finished_at_ns: 8 } })];
      // Readiness not pushed yet: only a reason Activate clears remains.
      await mounted.render(allDone, { ...estopBlocked, blocked_reasons: [estopBlocked.blocked_reasons[2]!] });
      await flush();
      expect(api.groupAction).not.toHaveBeenCalled();
      expect(mounted.controller.flow?.failures).toEqual([]);
      expect(mounted.controller.flow?.phase).toBe('waiting');
      await mounted.render(allDone, ready);
      expect(mounted.execute).toHaveBeenCalledOnce();
    } finally { await mounted.unmount(); }
  });

  it('stops before the next component when its causes changed since the confirmation', async () => {
    api.recoverEnable.mockResolvedValue({ action: 'enable' });
    api.recoverEnablePreflight.mockImplementation(async (id: string) => ({ snapshot_id: `s-${id}`,
      components: [ownerRow, memberRow(id)] }));
    const mounted = await mount(estopped, estopBlocked);
    try {
      await mounted.act((value) => value.open());
      await flush();
      // Before the arm is sent, a new fault appears on it.
      api.recoverEnablePreflight.mockResolvedValue({ snapshot_id: 'u', components: [ownerRow,
        { component_id: 'lite6_robot', owner_id: 'lite6', stop: null, eligibility: 'recover',
          cause: { code: 'fault_collision', message: 'Collision', vendor_code: 'C31', vendor_text: null,
            clearable: true, operator_action: 'recover', concerns: [] } }] });
      await mounted.act((value) => value.confirm());
      await flush();
      expect(api.recoverEnable).not.toHaveBeenCalled();
      expect(mounted.controller.flow?.phase).toBe('failed');
      expect(mounted.controller.flow?.failures[0]?.text).toContain('The causes changed since you confirmed them');
      expect(mounted.controller.flow?.failures[0]?.text).toContain('(C31)');
      expect(mounted.execute).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
  });

  it('lists a cause it cannot recover with its required action and keeps the button disabled', async () => {
    api.recoverEnablePreflight.mockResolvedValue({ snapshot_id: 's', components: [
      { ...ownerRow, eligibility: 'blocked', stop: null,
        cause: { ...ownerRow.cause, code: 'fault_drive', message: 'Servo error', vendor_code: 'C21', clearable: false, operator_action: 'none' } },
      memberRow('lite6_robot')] });
    const mounted = await mount(estopped, estopBlocked);
    try {
      await mounted.act((value) => value.open());
      await flush();
      expect(mounted.controller.recoveryReady).toBe(false);
      const dialog = renderToStaticMarkup(<ActivateExecuteDialog activation={mounted.controller} />);
      expect(dialog).toContain("This can&#x27;t be recovered from here:");
      expect(dialog).toMatch(/class="transport-run"[^>]*disabled=""[^>]*>Recover, enable and execute/);
      await mounted.act((value) => value.confirm());
      expect(api.recoverEnable).not.toHaveBeenCalled();
      expect(api.groupAction).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
  });
});
