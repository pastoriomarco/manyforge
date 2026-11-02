import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { ActivateExecuteDialog } from '../components/ActivateExecuteDialog';
import { activationOffered, inactiveComponentIds } from '../state/activateExecute';
import { reactPropsHost } from '../test/reactPropsHost';
import type { ComponentState, RunBlockedReason, RunReadiness } from '../types/components';
import { useActivateExecute, type ActivateExecuteController } from './useActivateExecute';

const api = vi.hoisted(() => ({ groupAction: vi.fn() }));

vi.mock('../api/client', async (original) => ({
  ...(await original<typeof import('../api/client')>()),
  api: { components: { groupAction: api.groupAction } },
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
  function Probe(props: { states: ComponentState[]; readiness: RunReadiness; stopGeneration: number | null }) {
    controller = useActivateExecute(props.states, props.readiness, execute, props.stopGeneration);
    return null;
  }
  let stop = stopGeneration;
  const render = (next: ComponentState[], nextReadiness: RunReadiness = readiness, nextStop: number | null = stop) => act(() => {
    stop = nextStop;
    root.render(<Probe states={next} readiness={nextReadiness} stopGeneration={nextStop} />);
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
    // A faulted component isn't something Activate brings up; a latch isn't either.
    expect(activationOffered(blocked, [down[0]!, state('lite6_robot', 'fault', member), down[2]!])).toBe(false);
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
        phase: 'offered', componentIds: ['lite6', 'lite6_robot', 'lite_gripper'], failures: [],
      });
      const dialog = renderToStaticMarkup(<ActivateExecuteDialog activation={mounted.controller} />);
      expect(dialog).toContain('Activate components and execute?');
      expect(dialog).toContain('Activate prepares and enables every active component of the Deployment, then executes the Program once.');
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
