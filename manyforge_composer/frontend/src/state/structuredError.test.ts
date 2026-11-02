import { describe, expect, it } from 'vitest';

import {
  failureContextText,
  formatStructuredError,
  humanizeReasonCode,
  structuredErrorRows,
  structuredErrorText,
} from './structuredError';

describe('structured error display', () => {
  it('labels an owner refusal reference and its origin', () => {
    expect(formatStructuredError({
      code: 'owner_request_failed',
      message: 'frame role tool0 is not available',
      details: {
        reason: 'frame_role_unresolved',
        reference_type: 'frame_role',
        reference_id: 'tool0',
        target_role_id: 'arm',
      },
      origin: { kind: 'owner', nodeInstanceId: 'move_to_pick', ownerApi: 'scene.frames' },
    })).toEqual({
      title: 'owner request failed',
      message: 'frame role tool0 is not available',
      rows: [
        { label: 'Reason', value: 'frame_role_unresolved' },
        { label: 'Reference', value: "frame_role 'tool0' of target role 'arm'" },
        { label: 'Origin', value: "scene.frames for node 'move_to_pick'" },
      ],
    });
  });

  it('does not repeat a reason or reference the owner message already names', () => {
    // describe_owner_failure: the Runner message names details.reason and the reference.
    expect(structuredErrorText({
      code: 'request_ledger_full',
      message: "cell-admission request rejected (claim_capacity_exhausted; reference frame_role 'tool0' of target role 'arm')",
      details: {
        reason: 'claim_capacity_exhausted',
        reference_type: 'frame_role',
        reference_id: 'tool0',
        target_role_id: 'arm',
      },
      origin: { kind: 'owner', nodeInstanceId: 'move', ownerApi: 'kernel.cell_admission' },
    })).toBe(
      "cell-admission request rejected (claim_capacity_exhausted; reference frame_role 'tool0' of target role 'arm')"
        + " (Origin: kernel.cell_admission for node 'move')",
    );
    // Only a whole-token mention counts as named.
    expect(structuredErrorRows({
      details: { reason: 'capacity' }, origin: null,
    }, 'claim_capacity_exhausted')).toEqual([{ label: 'Reason', value: 'capacity' }]);
  });

  it('labels a node input failure', () => {
    expect(structuredErrorRows({
      details: {
        input_problem: 'missing',
        path: 'duration_s',
        source_kind: 'blackboard_key',
        source_reference: 'delay',
      },
      origin: { kind: 'program_node', nodeInstanceId: 'wait', ownerApi: null },
    })).toEqual([
      { label: 'Input', value: "duration_s from blackboard key 'delay' (missing)" },
      { label: 'Origin', value: "node 'wait'" },
    ]);
    expect(structuredErrorRows({
      details: { input_problem: 'invalid', path: null, source_kind: 'parameter', source_reference: null },
      origin: null,
    })).toEqual([{ label: 'Input', value: 'node inputs from parameter (invalid)' }]);
  });

  it('labels the required ErrorDTO details and any other API detail', () => {
    expect(structuredErrorRows({
      details: {
        component_id: 'arm.left',
        retryable: false,
        required_action: 'reset_cycle',
        maximum_admitted: 4,
        lane: 'ordinary',
      },
      origin: null,
    })).toEqual([
      { label: 'Required action', value: 'reset cycle' },
      { label: 'Component id', value: 'arm.left' },
      { label: 'Lane', value: 'ordinary' },
      { label: 'Maximum admitted', value: '4' },
      { label: 'Retryable', value: 'no' },
    ]);
    // A registered key outside its group is still shown, never dropped.
    expect(structuredErrorRows({
      details: { path: '/projects/cell', reference_id: 'tool0', component_id: null },
      origin: null,
    })).toEqual([
      { label: 'Component id', value: 'none' },
      { label: 'Path', value: '/projects/cell' },
      { label: 'Reference id', value: 'tool0' },
    ]);
  });

  it('names a runner origin with or without one exact node', () => {
    expect(failureContextText({
      details: null,
      origin: { kind: 'program_runner', nodeInstanceId: null, ownerApi: null },
    })).toBe('Origin: Program runner');
    expect(failureContextText({
      details: null,
      origin: { kind: 'program_runner', nodeInstanceId: 'seq', ownerApi: null },
    })).toBe("Origin: Program runner at node 'seq'");
    expect(failureContextText({
      details: null,
      origin: { kind: 'owner', nodeInstanceId: 'grip', ownerApi: null },
    })).toBe("Origin: owner for node 'grip'");
    expect(failureContextText({ details: null, origin: null })).toBeNull();
  });

  it('keeps the message as produced and falls back to the code', () => {
    expect(structuredErrorText({
      code: 'operator_stop', message: 'Stopped by the operator.', details: null, origin: null,
    })).toBe('Stopped by the operator.');
    expect(structuredErrorText({
      code: 'node_input_unavailable',
      message: '',
      details: null,
      origin: { kind: 'program_node', nodeInstanceId: 'wait', ownerApi: null },
    })).toBe("node input unavailable (Origin: node 'wait')");
    expect(humanizeReasonCode(' stop_fault ')).toBe('stop fault');
  });
});
