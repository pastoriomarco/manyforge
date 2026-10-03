import { describe, expect, it } from 'vitest';

import { heldOffer, heldRun } from '../testFixtures/heldRun';
import type { ComposerHeadlessState } from '../types/headless';
import { heldRunMotionStopped, manualOperationSettled } from './heldRunAuthoring';

type Manual = { resident?: unknown; state?: unknown };

function snapshot(
  manualOperations: Manual[] | undefined,
  { runState = 'intervention_required', offer = true }: { runState?: string; offer?: boolean } = {},
): ComposerHeadlessState {
  return {
    programCycle: {
      state: 'running',
      programRunId: 'run.1',
      run: { ...heldRun(), state: runState },
      recoveryOffer: offer ? heldOffer({ actions: [] }) : null,
      runtimeControls: {
        executionSelections: {
          groups: [{ groupId: 'all', state: 'held', nodeCount: 2, heldNodeCount: 2 }],
        },
      },
    },
    executionControlStatus: manualOperations === undefined ? null : { manualOperations },
  } as unknown as ComposerHeadlessState;
}

describe('all motion confirmed stopped requires affirmative settlement', () => {
  it.each<Manual>([
    { resident: false, state: 'completed' },
    { resident: false, state: 'stopped' },
    { resident: false, state: 'failed' },
    { resident: true, state: 'cancelled_pre_dispatch' },
    { resident: false, state: 'cancelled_pre_dispatch' },
  ])('a settled manual operation leaves the Draft editable (%o)', (operation) => {
    expect(manualOperationSettled(operation)).toBe(true);
    expect(heldRunMotionStopped(snapshot([operation]))).toBe(true);
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
  ])('an unsettled, unknown or missing state is not stopped (%o)', (operation) => {
    expect(manualOperationSettled(operation)).toBe(false);
    expect(heldRunMotionStopped(snapshot([operation]))).toBe(false);
  });

  it('missing control state or a failure pause without its recovery offer is not stopped', () => {
    expect(heldRunMotionStopped(snapshot([]))).toBe(true);
    expect(heldRunMotionStopped(snapshot(undefined))).toBe(false);
    expect(heldRunMotionStopped(snapshot([], { offer: false }))).toBe(false);
    // An operator Stop (run still `running`) publishes no recovery offer.
    expect(heldRunMotionStopped(snapshot([], { runState: 'running', offer: false }))).toBe(true);
  });
});

describe('a run Executed but never Run is stopped', () => {
  function executed(
    groupStates: string[],
    { runState = 'running', offer = false, manual = [] as Manual[] } = {},
  ): ComposerHeadlessState {
    const base = snapshot(manual, { runState, offer });
    return {
      ...base,
      programCycle: {
        ...base.programCycle,
        runtimeControls: {
          executionSelections: {
            groups: groupStates.map((state, index) => ({
              groupId: index === 0 ? 'all' : `group.${index}`,
              state,
              nodeCount: 2,
              heldNodeCount: state === 'ready' ? 2 : 0,
            })),
          },
        },
      },
    } as unknown as ComposerHeadlessState;
  }

  it('leaves the Draft editable while every group is still ready', () => {
    expect(heldRunMotionStopped(executed(['ready']))).toBe(true);
    expect(heldRunMotionStopped(executed(['ready', 'ready', 'ready']))).toBe(true);
  });

  it('locks the Draft once any group starts or the facts are not proven', () => {
    expect(heldRunMotionStopped(executed(['ready', 'running', 'ready']))).toBe(false);
    expect(heldRunMotionStopped(executed(['running']))).toBe(false);
    expect(heldRunMotionStopped(executed(['ready', 'mixed']))).toBe(false);
    expect(heldRunMotionStopped(executed(['ready', 'unavailable']))).toBe(false);
    expect(heldRunMotionStopped(executed([]))).toBe(false);
    // A recovery offer means a node already failed.
    expect(heldRunMotionStopped(executed(['ready'], { offer: true }))).toBe(false);
    expect(heldRunMotionStopped(executed(['ready'], { runState: 'stopping' }))).toBe(false);
    expect(heldRunMotionStopped(
      executed(['ready'], { manual: [{ resident: true, state: 'executing' }] }),
    )).toBe(false);
  });
});
