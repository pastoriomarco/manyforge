import { describe, expect, it, vi } from 'vitest';

import { ExecutionControlClient } from './executionControlClient';

describe('ExecutionControlClient', () => {
  it.each(['stop', 'ordinary'] as const)(
    'dispatches %s transport work before publishing local state',
    async (priority) => {
      const order: string[] = [];
      const client = new ExecutionControlClient();
      const response = client[priority](
        () => {
          order.push('network');
          return Promise.resolve('accepted');
        },
        () => order.push('local'),
      );

      expect(order).toEqual(['network', 'local']);
      await expect(response).resolves.toBe('accepted');
    },
  );

  it('does not publish pending state when request creation throws', () => {
    const client = new ExecutionControlClient();
    const afterDispatch = vi.fn();

    expect(() => client.stop(
      () => { throw new Error('request was not dispatched'); },
      afterDispatch,
    )).toThrow('request was not dispatched');
    expect(afterDispatch).not.toHaveBeenCalled();
  });
});
