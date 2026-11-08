import { describe, expect, it } from 'vitest';
import { ioEmptyOmits, ioSignalChoices, ioTimeoutHint, ioUnsupportedChoices } from './ioAuthoring';

const signal = (signal_name: string, direction: 'input' | 'output', ambiguous = false) => ({
  reference: { component_id: 'robot', signal_name }, value_type: 'bool', direction, ambiguous,
});
describe('io node authoring', () => {
  it('preserves direct identities and shows incompatible or ambiguous choices disabled', () => {
    const rows = [signal('DI0', 'input'), signal('DO1', 'output'), signal('DI2', 'input', true)];
    const choices = ioSignalChoices('io.check_input', rows)!;
    expect(choices[0].value).toEqual({ component_id: 'robot', signal_name: 'DI0' });
    expect(choices[0].disabled).toBeUndefined();
    expect(choices[1].disabled).toBe(true);
    expect(choices[2].disabled).toBe(true);
    expect(ioSignalChoices('io.check_input', null)).toBeNull();
  });
  it('enables clear only when the exact derived signal is qualified', () => {
    const output = signal('DO1', 'output');
    expect(ioSignalChoices('io.clear_outside_change', [output])![0].disabled).toBe(true);
    const choices = ioSignalChoices('io.clear_outside_change', [output, signal('DO1.changed_outside', 'input')])!;
    expect(choices[0].disabled).toBeUndefined();
  });
  it('marks every edge mode except level unsupported, only on wait_for_input', () => {
    const schema = { enum: ['level', 'rising', 'falling'] };
    expect(ioUnsupportedChoices('io.wait_for_input', 'edge', schema)?.values)
      .toEqual(['rising', 'falling']);
    expect(ioUnsupportedChoices('io.wait_for_input', 'edge', { enum: ['level'] })).toBeUndefined();
    expect(ioUnsupportedChoices('io.check_input', 'edge', schema)).toBeUndefined();
  });

  it('lets an empty timeout_s mean wait forever and states the catalog range', () => {
    expect(ioEmptyOmits('io.wait_for_input', 'timeout_s')).toBe(true);
    expect(ioEmptyOmits('logic.wait', 'timeout_s')).toBe(false);
    expect(ioTimeoutHint({ type: 'number', minimum: 0.01, maximum: 3600 }))
      .toContain('0.01–3600 s');
  });
});
