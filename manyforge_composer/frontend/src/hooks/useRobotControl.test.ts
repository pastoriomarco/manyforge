import { describe, expect, it } from 'vitest';

import type { RuntimeScaleControlStatus } from '../types/robotControl';
import {
  mergeRuntimeScaleStatus,
  runtimeScaleControlScopeFence,
  runtimeScaleFlushDelay,
} from './useRobotControl';

const status = {
  selector: {
    programBindingId: 'binding-1',
    componentRoleId: 'robot',
    targetRoleId: 'arm',
  },
  controlGeneration: 3,
  requestedScale: 0.4,
  supported: true,
  frozen: false,
  controlDomainId: 'domain-1',
  controlDomainRevision: 'domain-revision-1',
  controlBindingId: 'control-binding-1',
  controlBindingRevision: 'control-binding-revision-1',
  scalingDomainId: 'scaling-domain-1',
  activeControlConfigurationId: 'configuration-1',
  activeControlConfigurationGeneration: 7,
  activeOperationId: null,
  activeOperationRevision: null,
  activeOperationScaleGeneration: null,
  effectiveScale: null,
  applicationState: null,
  reason: null,
} satisfies RuntimeScaleControlStatus;

describe('persistent runtime-scale transport cadence', () => {
  it('sends the first value immediately and coalesces later values to 250 ms', () => {
    expect(runtimeScaleFlushDelay(10, Number.NEGATIVE_INFINITY, true, false))
      .toBe(0);
    expect(runtimeScaleFlushDelay(100, 0, true, false)).toBe(150);
    expect(runtimeScaleFlushDelay(300, 0, true, false)).toBe(0);
  });

  it('does not schedule without a queued value or while one request is in flight', () => {
    expect(runtimeScaleFlushDelay(10, 0, false, false)).toBeNull();
    expect(runtimeScaleFlushDelay(10, 0, true, true)).toBeNull();
  });

  it('invalidates pending input on owner, binding, or control-configuration change', () => {
    const initial = runtimeScaleControlScopeFence('owner-1', status);
    expect(runtimeScaleControlScopeFence('owner-2', status)).not.toBe(initial);
    expect(runtimeScaleControlScopeFence('owner-1', {
      ...status,
      selector: { ...status.selector, programBindingId: 'binding-2' },
    })).not.toBe(initial);
    expect(runtimeScaleControlScopeFence('owner-1', {
      ...status,
      activeControlConfigurationGeneration: 8,
    })).not.toBe(initial);
  });

  it('rejects a delayed projection older than the accepted owner generation', () => {
    const accepted = {
      ...status,
      controlGeneration: 5,
      requestedScale: 0.25,
    };
    expect(mergeRuntimeScaleStatus(accepted, status)).toBe(accepted);
    expect(mergeRuntimeScaleStatus(accepted, {
      ...status,
      controlGeneration: 5,
      requestedScale: 0.25,
      effectiveScale: 0.25,
      applicationState: 'applied',
    })).toMatchObject({
      controlGeneration: 5,
      requestedScale: 0.25,
      effectiveScale: 0.25,
      applicationState: 'applied',
    });
  });
});
