import { describe, expect, it } from 'vitest';
import {
  isOmittedOptionalParameter,
  numericUpperBoundError,
} from '../../utils/parameterAuthoring';

describe('numericUpperBoundError', () => {
  it('rejects only values above a supplied cap', () => {
    expect(
      numericUpperBoundError(0.4, 0.25, { label: 'global cap', unit: 'm/s' }),
    ).toBe('Exceeds global cap 0.25 m/s');
    expect(numericUpperBoundError(0.25, 0.25)).toBeNull();
    expect(numericUpperBoundError(999, undefined)).toBeNull();
  });
});

describe('isOmittedOptionalParameter', () => {
  it('distinguishes omission from required or wrong-typed values', () => {
    expect(isOmittedOptionalParameter(false, undefined)).toBe(true);
    expect(isOmittedOptionalParameter(false, null)).toBe(true);
    expect(isOmittedOptionalParameter(true, undefined)).toBe(false);
    expect(isOmittedOptionalParameter(false, '')).toBe(false);
  });
});
