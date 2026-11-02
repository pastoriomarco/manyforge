import { describe, expect, it } from 'vitest';

import {
  graspConfirmationForm,
  graspConfirmationProblem,
  graspConfirmationValue,
  workpieceGraspedSignal,
} from './graspConfirmation';

describe('grasp confirmation authoring', () => {
  it('reads an absent, an Off and a Required row', () => {
    expect(graspConfirmationForm(undefined)).toEqual({ policy: 'not_required', minPosition: '', maxPosition: '' });
    expect(graspConfirmationForm({ policy: 'not_required' }).policy).toBe('not_required');
    expect(graspConfirmationForm({
      policy: 'required', stall_window: { min_position: 0.005, max_position: 0.03 },
    })).toEqual({ policy: 'required', minPosition: '0.005', maxPosition: '0.03' });
  });

  it('round-trips the authored value', () => {
    for (const value of [
      { policy: 'required', stall_window: { min_position: 0.005, max_position: 0.03 } },
      { policy: 'not_required', stall_window: { min_position: 0, max_position: 0.01 } },
    ]) {
      expect(graspConfirmationValue(graspConfirmationForm(value), true)).toEqual(value);
    }
    expect(graspConfirmationValue(graspConfirmationForm({ policy: 'required' }), false)).toEqual({ policy: 'required' });
  });

  it('toggles Required and Off; Off without a window is the absent default', () => {
    const off = graspConfirmationForm(undefined);
    expect(graspConfirmationValue(off, true)).toBeUndefined();
    expect(graspConfirmationValue(off, false)).toBeUndefined();
    expect(graspConfirmationValue({ ...off, policy: 'required' }, false)).toEqual({ policy: 'required' });
    const limits = { minPosition: '0.005', maxPosition: '0.03' };
    expect(graspConfirmationValue({ policy: 'required', ...limits }, true)).toEqual({
      policy: 'required', stall_window: { min_position: 0.005, max_position: 0.03 },
    });
    expect(graspConfirmationValue({ policy: 'not_required', ...limits }, true)).toEqual({
      policy: 'not_required', stall_window: { min_position: 0.005, max_position: 0.03 },
    });
    // A pneumatic gripper never carries a window.
    expect(graspConfirmationValue({ policy: 'required', ...limits }, false)).toEqual({ policy: 'required' });
  });

  it('validates the stall window of a position gripper only', () => {
    const form = (policy: 'required' | 'not_required', minPosition: string, maxPosition: string) => (
      { policy, minPosition, maxPosition });
    expect(graspConfirmationProblem(form('required', '', ''), true)).toMatch(/needs a stall window/);
    expect(graspConfirmationProblem(form('not_required', '', ''), true)).toBeNull();
    expect(graspConfirmationProblem(form('required', '0.01', ''), true)).toMatch(/both stall window limits/);
    expect(graspConfirmationProblem(form('required', 'abc', '0.02'), true)).toMatch(/both stall window limits/);
    expect(graspConfirmationProblem(form('not_required', '0.03', '0.01'), true)).toMatch(/must not exceed/);
    expect(graspConfirmationProblem(form('required', '0.01', '0.01'), true)).toBeNull();
    expect(graspConfirmationProblem(form('required', '-0.01', '0.02'), true)).toBeNull();
    // A pneumatic gripper's evidence is its input; the owner names a missing one.
    expect(graspConfirmationProblem(form('required', '', ''), false)).toBeNull();
  });

  it('finds the mapped workpiece_grasped input', () => {
    const io = (signal: Record<string, string>) => ({
      outputs: [], inputs: { workpiece_grasped: { signal } },
    });
    expect(workpieceGraspedSignal(io({ controllable_component_id: 'sensors', signal_name: 'part' })))
      .toEqual({ componentId: 'sensors', signalName: 'part' });
    expect(workpieceGraspedSignal(io({ controllable_component_id: '', signal_name: '' }))).toBeNull();
    expect(workpieceGraspedSignal({ outputs: [], inputs: { open: { signal: {
      controllable_component_id: 'sensors', signal_name: 'open' } } } })).toBeNull();
    expect(workpieceGraspedSignal(undefined)).toBeNull();
  });
});
