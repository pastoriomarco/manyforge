import { describe, expect, it } from 'vitest';

import type { ProgramRunStateName } from '../types/api';
import {
  componentActionRunning, documentMotion, manualOperationSettled, newMotionReason,
  type DocumentMotionCycle,
} from './heldRunAuthoring';
import { initialRobotControlState, type RobotControlPhase } from './robotControl';

type Manual = { resident?: unknown; state?: unknown };

// Whether a held run leaves the Draft editable is the backend's published
// rule (Headless `test_held_run_authoring.py`); this display helper only
// explains a closed authoring gate.
describe('a manual operation is settled only affirmatively', () => {
  it.each<Manual>([
    { resident: false, state: 'completed' },
    { resident: false, state: 'stopped' },
    { resident: false, state: 'failed' },
    { resident: true, state: 'cancelled_pre_dispatch' },
    { resident: false, state: 'cancelled_pre_dispatch' },
  ])('a released terminal or never-dispatched operation is settled (%o)', (operation) => {
    expect(manualOperationSettled(operation)).toBe(true);
  });

  it.each<Manual>([
    { resident: true, state: 'intervention_required' },
    { resident: false, state: 'intervention_required' },
    { resident: true, state: 'interrupted' },
    { resident: false, state: 'interrupted' },
    { resident: true, state: 'completed' },
    { resident: true, state: 'executing' },
    { resident: false, state: 'mystery' },
    { resident: false },
    { state: 'completed' },
    { state: 'cancelled_pre_dispatch' },
  ])('an unsettled, unknown or missing state is not settled (%o)', (operation) => {
    expect(manualOperationSettled(operation)).toBe(false);
  });
});

// New waits while motion executes or a Stop is in progress (maintainer,
// 2026-11-13); `moving` is the document commands' existing motion gate.
describe('robots moving or stopping, for the document commands', () => {
  const manual = (phase: RobotControlPhase) => ({ ...initialRobotControlState, phase });
  const cycle = (
    state: DocumentMotionCycle['state'],
    {
      run = null, motionHeld = false, groups = [],
    }: { run?: ProgramRunStateName | null; motionHeld?: boolean; groups?: string[] } = {},
  ): DocumentMotionCycle => ({
    state,
    run: run === null ? null : { state: run },
    runtimeControls: state === 'running'
      ? { executionSelections: { motionHeld, groups: groups.map((group) => ({ state: group })) } }
      : null,
  });

  it.each<[string, DocumentMotionCycle | null, RobotControlPhase, boolean, boolean]>([
    ['an idle cell', cycle('idle_after_reset'), 'idle', false, false],
    ['a finished run awaiting Reset', cycle('terminal_awaiting_reset', { run: 'succeeded' }), 'idle', false, false],
    ['a held run', cycle('running', { run: 'running', motionHeld: true, groups: ['held'] }), 'idle', false, false],
    ['a run whose motion executes', cycle('running', { run: 'running', groups: ['running'] }), 'idle', true, false],
    ['a starting cycle', cycle('starting'), 'idle', true, false],
    ['a group Stop', cycle('running', { run: 'running', groups: ['stopping', 'held'] }), 'idle', true, true],
    ['a Stop all', cycle('running', { run: 'stopping' }), 'idle', true, true],
    ['a Halt', cycle('running', { run: 'halting' }), 'idle', true, true],
    ['a manual move', cycle('idle_after_reset'), 'executing', true, false],
    ['a manual move being stopped', cycle('idle_after_reset'), 'stopping', true, true],
    ['a manual move during a held run', cycle('running', { run: 'running', motionHeld: true }), 'executing', true, false],
    ['a settled manual move', cycle('idle_after_reset'), 'stopped', false, false],
    ['no cycle status', null, 'loading', false, false],
  ])('%s', (_name, observed, phase, moving, stopping) => {
    expect(documentMotion(observed, manual(phase))).toEqual({ moving, stopping });
  });

  // New's own rule: the published held-run rule decides a running cycle, as
  // the server does, and a Reset (which may move the robot) is motion.
  const MOVING = 'Not available while robots are moving. Stop motion first.';
  const HELD = 'Not available until all Program motion is confirmed stopped.';
  it.each<[string, DocumentMotionCycle | null, RobotControlPhase, boolean, string | null]>([
    ['an idle cell', cycle('idle_after_reset'), 'idle', false, null],
    ['a finished run awaiting Reset', cycle('terminal_awaiting_reset', { run: 'succeeded' }), 'idle', false, null],
    ['a held run the rule admits', cycle('running', { run: 'running', motionHeld: true }), 'idle', true, null],
    ['a halted run the rule admits, motion not held', cycle('running', { run: 'halted' }), 'idle', true, null],
    ['a halted run the rule refuses', cycle('running', { run: 'halted' }), 'idle', false, MOVING],
    ['a failure pause with containment unconfirmed', cycle('running', { run: 'intervention_required', motionHeld: true }), 'idle', false, HELD],
    ['a run whose motion executes', cycle('running', { run: 'running', groups: ['running'] }), 'idle', false, MOVING],
    ['a starting cycle', cycle('starting'), 'idle', false, MOVING],
    ['a Reset', cycle('resetting'), 'idle', false, MOVING],
    // The server always refuses New here: Reset when the Runner offers it.
    ['a recovery-required cycle offering Reset',
      { ...cycle('recovery_required'), allowedCommands: [{ commandKind: 'program_reset', allowed: true }] },
      'idle', false, 'Reset the Program first.'],
    ['a recovery-required cycle offering no Reset',
      { ...cycle('recovery_required'), allowedCommands: [{ commandKind: 'program_reset', allowed: false }] },
      'idle', false, 'Not available until the Program cycle is recovered.'],
    ['a group Stop', cycle('running', { run: 'running', groups: ['stopping'] }), 'idle', false, 'A Stop is in progress.'],
    ['a Halt', cycle('running', { run: 'halting' }), 'idle', false, 'A Stop is in progress.'],
    ['a manual move', cycle('idle_after_reset'), 'executing', false, MOVING],
    ['a manual move being stopped', cycle('idle_after_reset'), 'stopping', false, 'A Stop is in progress.'],
    ['a manual move during a held run', cycle('running', { run: 'running', motionHeld: true }), 'executing', false, MOVING],
    ['no cycle status', null, 'loading', false, null],
  ])('New: %s', (_name, observed, phase, heldRunAllowed, reason) => {
    expect(newMotionReason(observed, manual(phase), heldRunAllowed, false)).toBe(reason);
  });

  // OD-194: a running component action, after every other reason.
  const ACTION = 'Not available while a component action is running.';
  it.each<[string, DocumentMotionCycle | null, RobotControlPhase, string]>([
    ['an idle cell', cycle('idle_after_reset'), 'idle', ACTION],
    ['a finished run awaiting Reset', cycle('terminal_awaiting_reset', { run: 'succeeded' }), 'idle', ACTION],
    ['no cycle status', null, 'loading', ACTION],
    ['a manual move', cycle('idle_after_reset'), 'executing', MOVING],
    ['a run whose motion executes', cycle('running', { run: 'running', groups: ['running'] }), 'idle', MOVING],
    ['a failure pause with containment unconfirmed', cycle('running', { run: 'intervention_required', motionHeld: true }), 'idle', HELD],
    ['a recovery-required cycle offering Reset',
      { ...cycle('recovery_required'), allowedCommands: [{ commandKind: 'program_reset', allowed: true }] },
      'idle', 'Reset the Program first.'],
  ])('New during a component action: %s', (_name, observed, phase, reason) => {
    expect(newMotionReason(observed, manual(phase), false, true)).toBe(reason);
  });

  it('New is available again when the component action ends', () => {
    const idle = cycle('idle_after_reset');
    expect(newMotionReason(idle, manual('idle'), false, true)).toBe(ACTION);
    expect(newMotionReason(idle, manual('idle'), false, false)).toBeNull();
  });
});

describe('a component action is running', () => {
  it.each<[string, ReadonlyArray<{ action?: { action: 'reset_position'; started_at_ns: number } }> | null, boolean]>([
    ['unknown components', null, false],
    ['no components', [], false],
    ['no action running', [{}, {}], false],
    ['one component running an action', [{}, { action: { action: 'reset_position', started_at_ns: 1 } }], true],
  ])('%s', (_name, components, running) => {
    expect(componentActionRunning(components)).toBe(running);
  });
});
