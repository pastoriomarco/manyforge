import { describe, expect, it } from 'vitest';

import {
  getNumericCapError,
  isOmittedOptionalParam,
} from './PrimitiveParameterEditors';

describe('getNumericCapError (per-move TCP hard cap)', () => {
  it('rejects a value above the cap', () => {
    expect(getNumericCapError(0.4, 0.25, 'global cap')).toBe(
      'Exceeds global cap 0.25 m/s',
    );
  });

  it('allows a value equal to the cap', () => {
    expect(getNumericCapError(0.25, 0.25, 'global cap')).toBeNull();
  });

  it('allows a value below the cap', () => {
    expect(getNumericCapError(0.1, 0.25, 'global cap')).toBeNull();
  });

  it('imposes no bound when the cap is undefined', () => {
    expect(getNumericCapError(999, undefined, 'global cap')).toBeNull();
  });
});

describe('isOmittedOptionalParam (R1: empty optional vs required vs typo)', () => {
  it('treats an omitted OPTIONAL param as valid (skip validation)', () => {
    expect(isOmittedOptionalParam(false, undefined)).toBe(true);
    expect(isOmittedOptionalParam(false, null)).toBe(true);
  });

  it('does NOT skip a REQUIRED param left empty (still errors)', () => {
    expect(isOmittedOptionalParam(true, undefined)).toBe(false);
    expect(isOmittedOptionalParam(true, null)).toBe(false);
  });

  it('does NOT skip an optional param holding an empty string (still type-checked)', () => {
    expect(isOmittedOptionalParam(false, '')).toBe(false);
  });

  it('does NOT skip an optional param with a present value', () => {
    expect(isOmittedOptionalParam(false, 0.2)).toBe(false);
  });
});
