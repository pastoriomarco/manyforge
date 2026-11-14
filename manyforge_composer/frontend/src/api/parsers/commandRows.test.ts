import { describe, expect, it } from 'vitest';

import {
  applyResultOfRow,
  parseActivationPreparationStatus,
  parseApplyCommandReply,
  parseApplyCommands,
  parseProgramCommandReply,
  parseRunnerCommands,
} from './commandRows';

const KEY = 'a'.repeat(32);
const DIGEST = `sha256:${'d'.repeat(64)}`;

function runnerRow(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    kind: 'start', request_key: KEY, step: 'claiming',
    accepted_at_ns: '1763112345678901234', end: null, ...overrides,
  };
}

function end(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return { outcome: 'succeeded', code: null, details: null, finished_at_ns: '1763112345700000000', ...overrides };
}

describe('runner command rows (OD-214)', () => {
  it('parses the command in progress and the ended ones, newest first', () => {
    const rows = parseRunnerCommands({
      in_progress: runnerRow(),
      ended: [
        runnerRow({ kind: 'reset', request_key: 'b'.repeat(32), step: 'releasing', end: end() }),
        runnerRow({
          kind: 'start_motion', request_key: 'c'.repeat(32), step: 'admitting',
          end: end({ outcome: 'refused', code: 'rule_holds', details: { rule_id: 'door' } }),
        }),
        // Refused before acceptance: no step, no acceptance time.
        runnerRow({
          kind: 'resume_halt', request_key: 'e'.repeat(32), step: null, accepted_at_ns: null,
          end: end({ outcome: 'refused', code: 'program_run_revision_stale' }),
        }),
      ],
    });
    expect(rows.inProgress).toEqual({
      kind: 'start', requestKey: KEY, step: 'claiming',
      acceptedAtNs: '1763112345678901234', end: null,
    });
    expect(rows.ended.map((row) => [row.kind, row.end?.outcome, row.end?.code])).toEqual([
      ['reset', 'succeeded', null],
      ['start_motion', 'refused', 'rule_holds'],
      ['resume_halt', 'refused', 'program_run_revision_stale'],
    ]);
    expect(rows.ended[1].end?.details).toEqual({ rule_id: 'door' });
  });

  it('accepts every published step of a kind and refuses any other', () => {
    for (const step of ['admitting', 'recovering', 'resuming', 'stopping', 'restoring_position']) {
      expect(() => parseRunnerCommands({
        in_progress: runnerRow({ kind: 'recovery_action', step }), ended: [],
      })).not.toThrow();
    }
    for (const [kind, step] of [
      ['start', 'applying'], ['runtime_blackboard_write', 'admitting'],
      ['reset', 'claiming'], ['start', 'unknown_step'],
    ]) {
      expect(() => parseRunnerCommands({ in_progress: runnerRow({ kind, step }), ended: [] }))
        .toThrow(/step/);
    }
  });

  it('refuses rows that break the contract instead of guessing', () => {
    const invalid: Array<[unknown, RegExp]> = [
      [{ in_progress: null }, /exactly/],
      [{ in_progress: null, ended: [], extra: 1 }, /exactly/],
      [{ in_progress: runnerRow({ end: end() }), ended: [] }, /in_progress\.end/],
      [{ in_progress: null, ended: [runnerRow()] }, /end of an ended command/],
      [{ in_progress: null, ended: Array.from({ length: 9 }, () => runnerRow({ end: end() })) }, /at most 8/],
      [{ in_progress: runnerRow({ kind: 'deployment_apply' }), ended: [] }, /kind/],
      [{ in_progress: runnerRow({ request_key: 'request.1' }), ended: [] }, /request_key/],
      [{ in_progress: runnerRow({ accepted_at_ns: -1 }), ended: [] }, /accepted_at_ns/],
      [{ in_progress: runnerRow({ accepted_at_ns: '01' }), ended: [] }, /accepted_at_ns/],
      // Wall-clock ns are exact: decimal text, never a rounded number.
      [{ in_progress: runnerRow({ accepted_at_ns: Number('1763112345678901234') }), ended: [] }, /accepted_at_ns/],
      // No step before acceptance, and no end there other than refused.
      [{ in_progress: runnerRow({ accepted_at_ns: null }), ended: [] }, /before acceptance/],
      [{ in_progress: null, ended: [runnerRow({ step: null, accepted_at_ns: null, end: end() })] },
        /before acceptance/],
      [{ in_progress: null, ended: [runnerRow({ step: null, accepted_at_ns: null,
        end: end({ outcome: 'failed', code: 'program_run_started_then_failed' }) })] }, /before acceptance/],
      // A code exactly when the command did not succeed; no details on success.
      [{ in_progress: null, ended: [runnerRow({ end: end({ code: 'x' }) })] }, /code/],
      [{ in_progress: null, ended: [runnerRow({ end: end({ outcome: 'failed' }) })] }, /code/],
      [{ in_progress: null, ended: [runnerRow({ end: end({ details: { a: 1 } }) })] }, /details/],
      // A Stop never cuts a runner command off.
      [{ in_progress: null, ended: [runnerRow({ end: end({ outcome: 'interrupted', code: 'x' }) })] },
        /outcome/],
      [{ in_progress: null, ended: [runnerRow({ end: { ...end(), message: 'text' } })] }, /exactly/],
    ];
    for (const [value, error] of invalid) {
      expect(() => parseRunnerCommands(value), JSON.stringify(value)).toThrow(error);
    }
  });

  it('parses a runner reply at acceptance, ended or not', () => {
    const reply = (command: unknown) => ({ schema_version: 'manyforge.program_command_reply.v1', command });
    expect(parseProgramCommandReply(reply(runnerRow())).end).toBeNull();
    // An idle Reset without effects ends in the same change as its acceptance.
    expect(parseProgramCommandReply(reply(runnerRow({ kind: 'reset', step: 'admitting', end: end() })))
      .end?.outcome).toBe('succeeded');
    // A replayed key may return a row that ended unknown before acceptance
    // (an untyped failure), with the start and the key's display added.
    expect(parseProgramCommandReply({
      ...reply(runnerRow({ step: null, accepted_at_ns: null,
        end: end({ outcome: 'unknown', code: 'program_runtime_control_outcome_unknown',
          details: { command_kind: 'start', cause_code: 'RuntimeError' } }) })),
      application_start_id: '3f9a0000000000a1', display: '3f9a/s4.118',
    })).toMatchObject({ acceptedAtNs: null, step: null, end: { outcome: 'unknown' } });
    expect(() => parseProgramCommandReply({ ...reply(runnerRow()), application_start_id: 'A' }))
      .toThrow(/application_start_id/);
    expect(() => parseProgramCommandReply({ ...reply(runnerRow()), extra: 1 })).toThrow(/optional/);
    expect(() => parseProgramCommandReply({ ...reply(runnerRow()), schema_version: 'manyforge.program_runtime_control_result.v2' }))
      .toThrow(/schema_version/);
  });
});

function applyRow(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    kind: 'deployment_apply', request_key: KEY, step: 'applying',
    step_source: { status: 'manager_apply', apply_id: 4, apply_generation: 2 },
    accepted_at_ns: 10, end: null, ...overrides,
  };
}

describe('Apply-level command rows (OD-214)', () => {
  it('parses each step with the status that carries its detail', () => {
    const sources: Array<[string, string, unknown]> = [
      ['scenario_load', 'emptying', null],
      ['scenario_new', 'resetting_cycle', { status: 'runner_command', kind: 'reset', request_key: 'b'.repeat(32) }],
      ['deployment_revert', 'previewing', { status: 'activation_preparation', captured_input_digest: DIGEST }],
      ['deployment_apply', 'disabling', { status: 'component_action', component_id: 'arm' }],
      ['deployment_apply', 'applying', null],
      ['scenario_new', 'clearing_scene', { status: 'scene_clear', apply_id: 4, apply_generation: 2 }],
      ['program_apply', 'preparing_components', { status: 'component_action', component_id: 'gripper' }],
    ];
    for (const [kind, step, source] of sources) {
      const rows = parseApplyCommands({ in_progress: applyRow({ kind, step, step_source: source }), ended: [] });
      expect(rows.inProgress).toMatchObject({ kind, step });
    }
    expect(parseApplyCommands({ in_progress: applyRow(), ended: [] }).inProgress?.stepSource)
      .toEqual({ status: 'manager_apply', applyId: 4, applyGeneration: 2 });
  });

  it('refuses a step of another kind or a status that does not carry the step', () => {
    for (const row of [
      applyRow({ kind: 'deployment_apply', step: 'emptying', step_source: null }),
      applyRow({ kind: 'deployment_apply', step: 'clearing_scene', step_source: null }),
      applyRow({ step: 'disabling', step_source: { status: 'manager_apply', apply_id: 4, apply_generation: 2 } }),
      applyRow({ step_source: { status: 'manager_apply', apply_id: 4 } }),
      applyRow({ kind: 'scenario_open' }),
    ]) {
      expect(() => parseApplyCommands({ in_progress: row, ended: [] })).toThrow();
    }
  });

  it('keeps the component rows of an ended Apply, also when it succeeded', () => {
    const succeeded = parseApplyCommandReply({
      schema_version: 'manyforge.apply_command_reply.v1',
      command: applyRow({ end: end({ details: { components: [
        { component_id: 'old_arm', disposition: 'remove' },
        { component_id: 'arm', disposition: 'add', lifecycle_after: 'prepared' },
      ] } }) }),
    });
    expect(applyResultOfRow(succeeded)).toEqual({ outcome: 'succeeded', components: [
      { component_id: 'old_arm', disposition: 'remove' },
      { component_id: 'arm', disposition: 'add', lifecycle_after: 'prepared' },
    ] });
    // Interrupted, unknown and refused state a failure with their typed code.
    for (const outcome of ['failed', 'interrupted', 'unknown', 'refused']) {
      const row = parseApplyCommands({ in_progress: null, ended: [applyRow({
        end: end({ outcome, code: 'component_action_interrupted_by_stop', details: null }),
      })] }).ended[0];
      expect(applyResultOfRow(row)).toEqual({ outcome: 'failed', components: [],
        error: { code: 'component_action_interrupted_by_stop', message: '' } });
    }
    expect(() => applyResultOfRow(parseApplyCommands({ in_progress: null, ended: [applyRow({
      end: end({ details: { components: [{ component_id: 'arm', disposition: 'moved' }] } }),
    })] }).ended[0])).toThrow(/disposition/);
  });
});

describe('activation preparation status (OD-214)', () => {
  it('parses its rows with the request digest and refuses an Apply step', () => {
    const row = {
      kind: 'activation_preparation', request_key: KEY, step: 'preparing',
      accepted_at_ns: 3, end: null, captured_input_digest: DIGEST,
    };
    expect(parseActivationPreparationStatus({ in_progress: row, ended: [] }).inProgress)
      .toEqual({
        kind: 'activation_preparation', requestKey: KEY, step: 'preparing',
        acceptedAtNs: 3, end: null, capturedInputDigest: DIGEST,
      });
    expect(() => parseActivationPreparationStatus({ in_progress: { ...row, step: 'applying' }, ended: [] }))
      .toThrow(/step/);
    expect(() => parseActivationPreparationStatus({ in_progress: { ...row, captured_input_digest: 'x' }, ended: [] }))
      .toThrow(/captured_input_digest/);
    expect(() => parseActivationPreparationStatus({ in_progress: null, ended: [{
      ...row, end: end({ outcome: 'unknown', code: 'activation_preparation_failed' }),
    }] })).toThrow(/outcome/);
  });
});
