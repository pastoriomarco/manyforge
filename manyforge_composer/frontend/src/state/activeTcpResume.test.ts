import { describe, expect, it } from 'vitest';
import type { ProgramBindingDocument } from '../types/programBindingTransport.generated';
import type { ExecutionOwnerStatus } from '../types/robotControl';
import { ActiveTcpHoldHistory, activeTcpSnapshot } from './activeTcpResume';

const binding = { target_bindings: [
  { target: { component_id: 'robot-a', command_target_id: 'arm' }, robot_tcps: [] },
  { target: { component_id: 'robot-b', command_target_id: 'arm' }, robot_tcps: [] },
  { target: { component_id: 'tool', command_target_id: 'gripper' } },
] } as unknown as ProgramBindingDocument;

function status(a: string | null | undefined, b: string | null | undefined,
  frameComponentA = 'robot-a', frameComponentB = 'robot-b'): ExecutionOwnerStatus {
  return {
    applicationStartId: 'owner', graphRevision: 'graph', retainedBundleId: 'bundle',
    directControlTargets: [['robot-a', a], ['robot-b', b]].map(([robot, role]) => ({
      controllableComponentId: robot, commandTargetId: 'arm',
      operationCapabilityId: 'execution.trajectory.execute',
      activeTcp: role === undefined ? null : { entry: role === null ? null : {
        componentId: robot, targetId: 'arm',
        frameComponentId: robot === 'robot-a' ? frameComponentA : frameComponentB,
        frameRoleId: role,
      }, display: role === null ? null : `${robot === 'robot-a' ? frameComponentA : frameComponentB}.${role}`, applicationStartId: 'owner',
      registrationGeneration: 1, sequence: 1 },
    })),
  } as unknown as ExecutionOwnerStatus;
}

const snap = (a: string | null | undefined, b: string | null | undefined,
  frameComponentA?: string, frameComponentB?: string) => (
  activeTcpSnapshot(binding, status(a, b, frameComponentA, frameComponentB), 'graph', 'bundle')
);

describe('held-run Active TCP history', () => {
  it('compares bound robots for one hold and skips overlapping holds', () => {
    const history = new ActiveTcpHoldHistory();
    history.observe(1, [], snap('one', 'one'));
    history.observe(1, ['group:all', 'group:a'], snap('one', 'one'));
    expect(history.changes('group:a', snap('two', null))).toEqual([
      { robot: 'robot-a', oldDisplay: 'robot-a.one', newDisplay: 'robot-a.two' },
      { robot: 'robot-b', oldDisplay: 'robot-b.one', newDisplay: 'No TCP' },
    ]);
    history.observe(1, ['group:all', 'group:a', 'group:b'], snap('two', 'one'));
    expect(history.changes('group:a', snap('two', null))).toEqual([]);
    history.observe(1, ['group:all', 'group:a'], snap('two', null));
    expect(history.changes('group:a', snap('two', null))).toEqual([]);
  });

  it('skips Halt and ordinary Hold overlapping on the aggregate selection', () => {
    const history = new ActiveTcpHoldHistory();
    history.observe(1, [], snap('one', 'one'));
    history.observe(1, ['group:all'], snap('one', 'one'));
    history.observe(1, ['full'], snap('one', 'one'), ['group:all']);
    expect(history.changes('full', snap('two', 'one'))).toEqual([]);
    expect(history.changes('group:all', snap('two', 'one'))).toEqual([]);
  });

  it('preserves an established baseline across an unavailable row', () => {
    const history = new ActiveTcpHoldHistory();
    history.observe(1, [], snap('one', 'one'));
    history.observe(1, ['group:a'], snap('one', 'one'));
    history.observe(1, [], snap(undefined, undefined), ['group:a']);
    expect(history.changes('group:a', snap('two', 'one'))).toHaveLength(1);
    history.observe(1, [], snap('two', 'one'));
    expect(history.changes('group:a', snap('two', 'one'))).toEqual([]);
  });

  it('does not reuse a baseline after connection loss and an unseen release and re-hold', () => {
    const history = new ActiveTcpHoldHistory();
    history.observe(1, [], snap('one', 'one'));
    history.observe(1, ['group:a'], snap('one', 'one'));
    history.clear();
    history.observe(1, ['group:a'], snap('two', 'one'));
    expect(history.changes('group:a', snap('one', 'one'))).toEqual([]);
  });

  it('records a first-observed ordinary hold only for a locally started run', () => {
    const history = new ActiveTcpHoldHistory();
    history.observe(1, ['group:a'], snap('one', 'one'), [], true);
    expect(history.changes('group:a', snap('two', 'one'))).toHaveLength(1);
  });

  it('skips an already-held run after reload, and clears the previous run', () => {
    const history = new ActiveTcpHoldHistory();
    history.observe(1, ['group:a'], snap('one', 'one'));
    expect(history.changes('group:a', snap('two', 'one'))).toEqual([]);
    history.observe(1, [], snap('two', 'one'));
    history.observe(1, ['group:a'], snap('two', 'one'));
    expect(history.changes('group:a', snap('one', 'one'))).toHaveLength(1);
    history.observe(2, ['group:a'], snap('one', 'one'));
    expect(history.changes('group:a', snap('two', 'one'))).toEqual([]);
  });

  it('skips a replaced execution owner even when the entry differs', () => {
    const history = new ActiveTcpHoldHistory();
    history.observe(1, [], snap('one', 'one'));
    history.observe(1, ['group:a'], snap('one', 'one'));
    const replaced = status('two', 'one');
    replaced.applicationStartId = 'new-owner';
    replaced.directControlTargets.forEach((target) => { target.activeTcp!.applicationStartId = 'new-owner'; });
    const next = activeTcpSnapshot(binding, replaced, 'graph', 'bundle');
    expect(history.changes('group:a', next)).toEqual([]);
    history.observe(1, ['group:a'], next);
    expect(history.changes('group:a', snap('two', 'one'))).toEqual([]);
  });

  it('distinguishes an unavailable row from a versioned no-TCP entry', () => {
    const history = new ActiveTcpHoldHistory();
    history.observe(1, [], snap('one', undefined));
    history.observe(1, ['full'], snap('one', undefined));
    expect(history.changes('full', snap(undefined, null))).toEqual([]);
    expect(history.changes('full', snap(null, null))).toEqual([
      { robot: 'robot-a', oldDisplay: 'robot-a.one', newDisplay: 'No TCP' },
    ]);
    const invalid = activeTcpSnapshot(binding, status('one', 'one'), 'other-graph', 'bundle');
    expect(history.changes('full', invalid)).toEqual([]);
  });

  it('distinguishes equal frame IDs owned by different components', () => {
    const history = new ActiveTcpHoldHistory();
    const baseline = snap('shared', 'shared', 'frame-owner-a', 'frame-owner-b');
    history.observe(1, [], baseline);
    history.observe(1, ['group:a'], baseline);

    expect(history.changes('group:a', baseline)).toEqual([]);
    expect(history.changes('group:a', snap('shared', 'shared', 'frame-owner-b', 'frame-owner-b'))).toEqual([
      { robot: 'robot-a', oldDisplay: 'frame-owner-a.shared', newDisplay: 'frame-owner-b.shared' },
    ]);
  });
});
