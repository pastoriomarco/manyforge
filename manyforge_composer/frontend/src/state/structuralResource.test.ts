import { describe, expect, it } from 'vitest';
import {
  structuralResourceReducer,
  type StructuralResourceState,
} from './structuralResource';

describe('structuralResourceReducer', () => {
  const loading: StructuralResourceState<string> = { status: 'loading' };

  it('represents a successful structural read without availability inference', () => {
    expect(structuralResourceReducer(loading, {
      type: 'resolve',
      data: 'authored-only',
    })).toEqual({ status: 'ready', data: 'authored-only' });
  });

  it('keeps absent authored data distinct from request failure', () => {
    expect(structuralResourceReducer(loading, {
      type: 'absent',
      message: 'No authored deployment model.',
    })).toEqual({
      status: 'absent',
      message: 'No authored deployment model.',
    });
    expect(structuralResourceReducer(loading, {
      type: 'reject',
      message: 'Authorization failed.',
    })).toEqual({
      status: 'error',
      message: 'Authorization failed.',
    });
  });

  it('retains one coherent structural value while its replacement is read', () => {
    const ready: StructuralResourceState<string> = {
      status: 'ready',
      data: 'coherent-owner-projection',
    };
    expect(structuralResourceReducer(ready, {
      type: 'load',
    })).toBe(ready);
    expect(structuralResourceReducer(loading, { type: 'load' })).toBe(loading);
  });
});
