import { describe, expect, it } from 'vitest';
import {
  programSignalReferenceChoices,
  programTargetReferenceChoices,
  type SignalReferenceChoiceFact,
  type TargetReferenceChoiceFact,
} from './programTargetSignalChoices';

const targetFact = (
  component_id: string,
  command_target_id: string,
  capabilities: TargetReferenceChoiceFact['capabilities'],
): TargetReferenceChoiceFact => ({ target: { component_id, command_target_id }, capabilities });

const signalFact = (
  component_id: string,
  signal_name: string,
  options: Partial<Omit<SignalReferenceChoiceFact, 'reference'>> = {},
): SignalReferenceChoiceFact => ({
  reference: { component_id, signal_name },
  ambiguous: false,
  target: { component_id, command_target_id: 'arm' },
  value_type: 'bool',
  direction: 'input',
  provider_profile: 'robot',
  ...options,
});

describe('Program target and signal reference choice data', () => {
  it('keeps target identity component-qualified and reports each target capability result', () => {
    const choices = programTargetReferenceChoices([
      targetFact('arm-a', 'arm', [{ capability_id: 'motion', available: true, reason: null }]),
      targetFact('arm-b', 'arm', [{ capability_id: 'motion', available: false, reason: 'Unavailable on arm-b' }]),
      targetFact('tool', 'tool', []),
    ], ['motion']);

    expect(choices.map((choice) => choice.value)).toEqual([
      { component_id: 'arm-a', command_target_id: 'arm' },
      { component_id: 'arm-b', command_target_id: 'arm' },
      { component_id: 'tool', command_target_id: 'tool' },
    ]);
    expect(choices.map((choice) => choice.reason)).toEqual([
      undefined, 'Unavailable on arm-b', 'Missing motion',
    ]);
  });

  it('checks the exact signal target and preserves I/O and ambiguity reasons', () => {
    const signals = [
      signalFact('arm-b', 'DI0'),
      signalFact('arm-a', 'DO0', { direction: 'output' }),
      signalFact('arm-a', 'DI1', { ambiguous: true, target: null }),
    ];
    const targets = [
      targetFact('arm-a', 'arm', [{ capability_id: 'observe', available: true, reason: null }]),
      targetFact('arm-b', 'arm', [{ capability_id: 'observe', available: false, reason: 'Observation unavailable on arm-b' }]),
    ];

    const choices = programSignalReferenceChoices('io.check_input', signals, targets, ['observe']);
    expect(choices.map((choice) => choice.value)).toEqual([
      { component_id: 'arm-b', signal_name: 'DI0' },
      { component_id: 'arm-a', signal_name: 'DO0' },
      { component_id: 'arm-a', signal_name: 'DI1' },
    ]);
    expect(choices.map((choice) => choice.reason)).toEqual([
      'Observation unavailable on arm-b',
      'This node requires an input signal.',
      'Ambiguous signal identity',
    ]);
  });

  it('keeps the system-carried input capability exception for input nodes', () => {
    const signal = signalFact('system', 'object_present', {
      target: null,
      provider_profile: 'system_carried_object',
    });

    expect(programSignalReferenceChoices('io.get_input', [signal], null, ['observe'])[0].reason)
      .toBeUndefined();
    expect(programSignalReferenceChoices('io.set_output', [{
      ...signal,
      reference: { ...signal.reference, signal_name: 'object_reset' },
      direction: 'output',
    }], null, ['observe'])[0].reason).toBe('Missing observe');
  });
});
