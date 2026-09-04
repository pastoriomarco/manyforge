import { describe, expect, it } from 'vitest';

import type { RuntimeScaleControlStatus } from '../types/robotControl';
import {
  acceptedRuntimeScaleStatus,
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

  it('invalidates pending input only when its stable owner or scaling scope changes', () => {
    const initial = runtimeScaleControlScopeFence('owner-1', status);
    expect(runtimeScaleControlScopeFence('owner-2', status)).not.toBe(initial);
    expect(runtimeScaleControlScopeFence('owner-1', {
      ...status,
      selector: { ...status.selector, programBindingId: 'binding-2' },
    })).not.toBe(initial);
    expect(runtimeScaleControlScopeFence('owner-1', {
      ...status,
      scalingDomainId: 'scaling-domain-2',
    })).not.toBe(initial);
  });

  it('keeps queued input across ordinary scale acceptance revisions', () => {
    const initial = runtimeScaleControlScopeFence('owner-1', status);
    expect(runtimeScaleControlScopeFence('owner-1', {
      ...status,
      controlGeneration: 4,
      requestedScale: 0.25,
      activeControlConfigurationId: 'configuration-2',
      activeControlConfigurationGeneration: 8,
      activeOperationScaleGeneration: 4,
      effectiveScale: 0.25,
      applicationState: 'applied',
    })).toBe(initial);
  });

  it('uses an exact owner acceptance to fence the next scale update', () => {
    const accepted = acceptedRuntimeScaleStatus(status, {
      requestId: 'scale-request-1',
      selector: status.selector,
      requestedScale: 0.25,
      controlGeneration: 4,
      activeOperationId: 'operation-1',
      activeOperationRevision: 8,
      activeOperationScaleGeneration: 2,
    }, 'scale-request-1', 0.25);
    expect(accepted).toMatchObject({
      controlGeneration: 4,
      requestedScale: 0.25,
      activeOperationId: 'operation-1',
    });
    expect(() => acceptedRuntimeScaleStatus(status, {
      requestId: 'scale-request-2',
      selector: status.selector,
      requestedScale: 0.25,
      controlGeneration: 4,
      activeOperationId: null,
      activeOperationRevision: null,
      activeOperationScaleGeneration: null,
    }, 'scale-request-1', 0.25)).toThrow(/does not match/);
    expect(() => acceptedRuntimeScaleStatus(status, {
      requestId: 'scale-request-1',
      selector: status.selector,
      requestedScale: 0.4,
      controlGeneration: 4,
      activeOperationId: null,
      activeOperationRevision: null,
      activeOperationScaleGeneration: null,
    }, 'scale-request-1', 0.25)).toThrow(/does not match/);
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
