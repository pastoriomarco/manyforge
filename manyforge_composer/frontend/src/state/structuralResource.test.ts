import { describe, expect, it } from 'vitest';
import {
  errorMessage,
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

describe('errorMessage', () => {
  it('names what a refusal waits for from its structured details', async () => {
    const { ApiError } = await import('../api/client');
    const refusal = new ApiError('lifecycle mutation requires settled physical and Program work', 400, {
      detail: {
        code: 'execution_rejected',
        message: 'lifecycle mutation requires settled physical and Program work',
        details: {
          blocking_conditions: 'operations_active,resources_not_free',
          active_operation_ids: 'operation.7',
          busy_resource_ids: 'resource.arm',
        },
      },
    });
    expect(errorMessage(refusal, 'fallback')).toBe(
      'lifecycle mutation requires settled physical and Program work '
      + '(waiting for: operations_active, resources_not_free; operations: operation.7; resources: resource.arm)',
    );
    expect(errorMessage(new Error('plain'), 'fallback')).toBe('plain');
    expect(errorMessage(null, 'fallback')).toBe('fallback');
  });
});
