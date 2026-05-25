import { describe, expect, it, vi } from 'vitest';
import { coerceCycleStateFailClosed, parseCycleState } from './cycleState';

describe('cycle state boundary parsing', () => {
  it('accepts every backend cycle state currently exposed to Composer', () => {
    expect(parseCycleState('idle')).toBe('idle');
    expect(parseCycleState('running')).toBe('running');
    expect(parseCycleState('stopped')).toBe('stopped');
    expect(parseCycleState('direct_control')).toBe('direct_control');
  });

  it('rejects unknown states instead of silently casting them', () => {
    expect(() => parseCycleState('interrupted')).toThrow('Unknown cycle state');
  });

  it('coerces unknown states to running so editing fails closed', () => {
    const consoleSpy = vi.spyOn(console, 'error').mockImplementation(() => {});

    const result = coerceCycleStateFailClosed('interrupted');

    expect(result.state).toBe('running');
    expect(result.error).toContain('interrupted');
    expect(consoleSpy).toHaveBeenCalledOnce();
    consoleSpy.mockRestore();
  });
});
