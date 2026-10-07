import { describe, expect, it } from 'vitest';

import { manualOperationSettled } from './heldRunAuthoring';

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
