import { describe, expect, it } from 'vitest';
import { parseNanoseconds } from './nanoseconds';

describe('exact owner nanoseconds', () => {
  it.each([0, Number.MAX_SAFE_INTEGER, '9007199254740992', '9007199254740993',
    String(31n * 366n * 86400n * 1_000_000_000n), '9223372036854775807'])
  ('preserves %s without rounding', (value) => {
    expect(parseNanoseconds(value, 'time')).toBe(value);
  });

  it.each([Number.MAX_SAFE_INTEGER + 1, true, null, 0.5, -1, '01', '+1', '-1',
    '1e6', '9223372036854775808'])('rejects %s', (value) => {
    expect(() => parseNanoseconds(value, 'time')).toThrow('exact nonnegative int64');
  });
});
