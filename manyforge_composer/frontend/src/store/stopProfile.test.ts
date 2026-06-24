import { describe, expect, it } from 'vitest';

import type { RuntimeStatus } from '../types/api';
import {
  buildStopExecutionWarning,
  buildStopProfileView,
  evaluateStopRunGate,
  FAULT_STOP_SLOT,
  KNOWN_STOP_MODES,
  OPERATOR_STOP_SLOT,
  hardStopLevelLabel,
  shouldClearStopInProgress,
  shouldFreezeVelocitySlider,
} from './stopProfile';

function makeRuntime(overrides: Partial<RuntimeStatus> = {}): RuntimeStatus {
  return {
    schemaVersion: '1',
    profileId: 'p',
    profileKind: 'real_robot',
    profileLabel: 'Real robot',
    authority: 'controller_observed',
    readiness: 'ready',
    summaryLabel: 'Ready',
    canRender: true,
    canPlan: true,
    canExecute: true,
    userMessage: '',
    components: [],
    externalProviders: [],
    supportedStopModes: ['soft_on_path', 'spring_back_hold'],
    configuredStopModes: { [OPERATOR_STOP_SLOT]: 'soft_on_path', [FAULT_STOP_SLOT]: 'spring_back_hold' },
    configuredStopDecelerationTimes: { [OPERATOR_STOP_SLOT]: 2.0, [FAULT_STOP_SLOT]: 1.25 },
    stopCapabilityProfileId: 'ros2_control_v1',
    hardStopLevel: 'jtc_cancel_unprofiled_hold',
    derivativeProofLevel: 'none',
    scalingSourceVisibility: 'observed',
    scalingSourceProbeStatus: 'ok',
    stopProfileUnsupportedReasons: [],
    backendUnsupportedStopReasons: {},
    stopProfileUnsupported: false,
    stopCapabilityQueryFailed: false,
    timestampNs: 0,
    ...overrides,
  };
}

describe('buildStopProfileView', () => {
  it('returns an unavailable, empty view when there is no runtime status', () => {
    const view = buildStopProfileView(null);
    expect(view.available).toBe(false);
    expect(view.options).toEqual([]);
    expect(view.operatorMode).toBeNull();
    expect(view.faultMode).toBeNull();
  });

  it('extracts configured operator/fault modes and marks supported modes selectable', () => {
    const view = buildStopProfileView(makeRuntime());
    expect(view.available).toBe(true);
    expect(view.operatorMode).toBe('soft_on_path');
    expect(view.faultMode).toBe('spring_back_hold');
    expect(view.operatorDecelerationTimeS).toBe(2.0);
    expect(view.faultDecelerationTimeS).toBe(1.25);
    expect(view.unsupported).toBe(false);

    const soft = view.options.find((o) => o.mode === 'soft_on_path');
    expect(soft).toBeDefined();
    expect(soft?.supported).toBe(true);
    expect(soft?.disabled).toBe(false);
    expect(soft?.reason).toBeNull();
    expect(soft?.configuredSlots).toEqual([OPERATOR_STOP_SLOT]);
  });

  it('disables modes the backend does not advertise and attaches the backend reason', () => {
    const view = buildStopProfileView(
      makeRuntime({
        supportedStopModes: ['soft_on_path'],
        backendUnsupportedStopReasons: {
          hard_deceleration: 'decelerate_on_cancel_evidence_incomplete',
        },
      }),
    );
    const hard = view.options.find((o) => o.mode === 'hard_deceleration');
    expect(hard?.supported).toBe(false);
    expect(hard?.disabled).toBe(true);
    expect(hard?.reason).toBe('decelerate_on_cancel_evidence_incomplete');
  });

  it('makes hard_deceleration selectable when advertised by the backend', () => {
    const view = buildStopProfileView(
      makeRuntime({
        supportedStopModes: ['soft_on_path', 'spring_back_hold', 'hard_deceleration'],
        hardStopLevel: 'hard_deceleration',
      }),
    );
    const hard = view.options.find((o) => o.mode === 'hard_deceleration');
    expect(hard?.supported).toBe(true);
    expect(hard?.disabled).toBe(false);
    expect(hard?.reason).toBeNull();
  });

  it('falls back to the structured stopProfileUnsupportedReasons entry when no per-mode reason exists', () => {
    const view = buildStopProfileView(
      makeRuntime({
        supportedStopModes: ['soft_on_path'],
        backendUnsupportedStopReasons: {},
        stopProfileUnsupportedReasons: [
          { profile: FAULT_STOP_SLOT, stop_mode: 'hard_deceleration', reason: 'stop_mode_not_supported' },
        ],
      }),
    );
    const hard = view.options.find((o) => o.mode === 'hard_deceleration');
    expect(hard?.reason).toBe('stop_mode_not_supported');
  });

  it('fail-closes every mode to disabled when the capability query failed', () => {
    const view = buildStopProfileView(
      makeRuntime({
        supportedStopModes: null,
        stopCapabilityQueryFailed: true,
      }),
    );
    expect(view.available).toBe(true);
    expect(view.queryFailed).toBe(true);
    for (const option of view.options) {
      expect(option.supported).toBe(false);
      expect(option.disabled).toBe(true);
      expect(option.reason).toBe('stop capability could not be verified');
    }
  });

  it('never hides a configured or advertised mode outside the known set', () => {
    const view = buildStopProfileView(
      makeRuntime({
        supportedStopModes: ['soft_on_path', 'experimental_future_mode'],
        configuredStopModes: { [OPERATOR_STOP_SLOT]: 'experimental_future_mode' },
      }),
    );
    const modes = view.options.map((o) => o.mode);
    for (const known of KNOWN_STOP_MODES) {
      expect(modes).toContain(known);
    }
    expect(modes).toContain('experimental_future_mode');
    const exp = view.options.find((o) => o.mode === 'experimental_future_mode');
    expect(exp?.supported).toBe(true);
    expect(exp?.configuredSlots).toEqual([OPERATOR_STOP_SLOT]);
  });

  it('reports unsupported when the backend flags the configured profile unsupported', () => {
    const view = buildStopProfileView(
      makeRuntime({
        supportedStopModes: ['spring_back_hold'],
        configuredStopModes: { [OPERATOR_STOP_SLOT]: 'soft_on_path' },
        stopProfileUnsupported: true,
      }),
    );
    expect(view.unsupported).toBe(true);
    const operator = view.options.find((o) => o.mode === 'soft_on_path');
    expect(operator?.disabled).toBe(true);
  });

  it('is unavailable with no options when nothing is configured or advertised', () => {
    const view = buildStopProfileView(
      makeRuntime({
        supportedStopModes: null,
        stopCapabilityQueryFailed: false,
        configuredStopModes: {},
      }),
    );
    expect(view.available).toBe(false);
    expect(view.options).toEqual([]);
  });

  it('fail-closes the configured mode to null on malformed (empty / non-string) slot values', () => {
    const view = buildStopProfileView(
      makeRuntime({
        configuredStopModes: {
          [OPERATOR_STOP_SLOT]: '',
          // backend contract is Record<string,string>; guard the malformed case anyway.
          [FAULT_STOP_SLOT]: 123 as unknown as string,
        },
      }),
    );
    expect(view.operatorMode).toBeNull();
    expect(view.faultMode).toBeNull();
  });

  it('ignores malformed configured stop deceleration values', () => {
    const view = buildStopProfileView(
      makeRuntime({
        configuredStopDecelerationTimes: {
          [OPERATOR_STOP_SLOT]: Number.NaN,
          [FAULT_STOP_SLOT]: -1,
        },
      }),
    );
    expect(view.operatorDecelerationTimeS).toBeNull();
    expect(view.faultDecelerationTimeS).toBeNull();
  });

  it('always keeps the disabled === !supported invariant', () => {
    const view = buildStopProfileView(
      makeRuntime({ supportedStopModes: ['soft_on_path'] }),
    );
    expect(view.options.length).toBeGreaterThan(0);
    for (const option of view.options) {
      expect(option.disabled).toBe(!option.supported);
    }
  });
});

describe('evaluateStopRunGate', () => {
  it('does not block when runtime status is not yet known (backend stays authoritative)', () => {
    expect(evaluateStopRunGate(null)).toEqual({ blocked: false, severity: null, reason: null });
  });

  it('does not block a healthy, supported stop profile', () => {
    const gate = evaluateStopRunGate(makeRuntime());
    expect(gate.blocked).toBe(false);
    expect(gate.severity).toBeNull();
  });

  it('blocks with fault severity when the capability query failed', () => {
    const gate = evaluateStopRunGate(makeRuntime({ stopCapabilityQueryFailed: true, supportedStopModes: null }));
    expect(gate.blocked).toBe(true);
    expect(gate.severity).toBe('fault');
    expect(gate.reason).toMatch(/could not report its supported stop modes/);
  });

  it('blocks with degraded severity and the offending mode + reason when the profile is unsupported', () => {
    const gate = evaluateStopRunGate(
      makeRuntime({
        supportedStopModes: ['spring_back_hold'],
        configuredStopModes: { [OPERATOR_STOP_SLOT]: 'soft_on_path' },
        stopProfileUnsupported: true,
        backendUnsupportedStopReasons: { soft_on_path: 'planner_config_invalid' },
      }),
    );
    expect(gate.blocked).toBe(true);
    expect(gate.severity).toBe('degraded');
    expect(gate.reason).toMatch(/Soft on-path/);
    expect(gate.reason).toMatch(/planner_config_invalid/);
  });

  it('prioritises the query-failed fault over the unsupported-profile degraded state', () => {
    const gate = evaluateStopRunGate(
      makeRuntime({ stopCapabilityQueryFailed: true, stopProfileUnsupported: true, supportedStopModes: null }),
    );
    expect(gate.severity).toBe('fault');
  });

  it('blocks without naming a phantom mode when unsupported is flagged but no slot is configured', () => {
    const gate = evaluateStopRunGate(
      makeRuntime({
        supportedStopModes: ['spring_back_hold'],
        configuredStopModes: {},
        stopProfileUnsupported: true,
      }),
    );
    expect(gate.blocked).toBe(true);
    expect(gate.severity).toBe('degraded');
    expect(gate.reason).toBe('Run blocked: a configured stop profile is not supported by the backend.');
    expect(gate.reason).not.toMatch(/\(configured\)/);
  });

  it('explains when a required operator or fault stop profile is missing', () => {
    const gate = evaluateStopRunGate(
      makeRuntime({
        configuredStopModes: { [OPERATOR_STOP_SLOT]: 'soft_on_path' },
        stopProfileUnsupported: true,
        stopProfileUnsupportedReasons: [
          { profile: FAULT_STOP_SLOT, stop_mode: '', reason: 'stop_profile_required' },
        ],
      }),
    );
    expect(gate.blocked).toBe(true);
    expect(gate.severity).toBe('degraded');
    expect(gate.reason).toBe(
      'Run blocked: executable deployments must configure both operator and fault stop profiles.',
    );
  });

  it('explains when the mandatory hard-stop floor is unavailable', () => {
    const gate = evaluateStopRunGate(
      makeRuntime({
        hardStopLevel: 'unavailable',
        stopProfileUnsupported: true,
        stopProfileUnsupportedReasons: [
          { profile: 'hard_stop_floor', stop_mode: 'unavailable', reason: 'hard_stop_floor_unavailable' },
        ],
      }),
    );
    expect(gate.blocked).toBe(true);
    expect(gate.severity).toBe('degraded');
    expect(gate.reason).toBe(
      'Run blocked: the execution backend did not report a valid hard-stop floor.',
    );
  });
});

describe('buildStopExecutionWarning', () => {
  it('returns null when the stop executed as requested', () => {
    expect(buildStopExecutionWarning({
      requested_stop_mode: 'soft_on_path',
      executed_stop_mode: 'soft_on_path',
    })).toBeNull();
  });

  it('warns when the backend executed a fallback stop mode', () => {
    expect(buildStopExecutionWarning({
      requested_stop_mode: 'soft_on_path',
      executed_stop_mode: 'spring_back_hold',
      fallback_reason: 'soft_on_path_planner_rejected',
    })).toBe(
      'Controlled stop used Spring-back hold instead of Soft on-path — soft_on_path_planner_rejected.',
    );
  });

  it('warns when a controlled stop transitioned to hard stop', () => {
    expect(buildStopExecutionWarning({
      hard_stop_transitioned: 'true',
      reason: 'stop_path_collision',
    })).toBe('Controlled stop escalated to hard stop — stop_path_collision.');
  });

  it('includes the hard-stop floor when reported', () => {
    expect(buildStopExecutionWarning({
      hard_stop_transitioned: 'true',
      hard_stop_level: 'jtc_cancel_unprofiled_hold',
      reason: 'backend_stop_not_confirmed',
    })).toBe(
      'Controlled stop escalated to hard stop ' +
      '(Immediate cancel/hold (unprofiled JTC floor)) — backend_stop_not_confirmed.',
    );
  });
});

describe('hardStopLevelLabel', () => {
  it('labels the profiled and immediate hard-stop levels', () => {
    expect(hardStopLevelLabel('hard_deceleration')).toContain('Profiled');
    expect(hardStopLevelLabel('jtc_cancel_unprofiled_hold')).toContain('Immediate cancel/hold');
    expect(hardStopLevelLabel('native_immediate_hold')).toBe('Immediate native hold');
    expect(hardStopLevelLabel(null)).toBe('Not reported');
  });
});

describe('shouldClearStopInProgress', () => {
  it('keeps the latch while running, stopping, or jogging', () => {
    expect(shouldClearStopInProgress('running')).toBe(false);
    expect(shouldClearStopInProgress('stopping')).toBe(false);
    expect(shouldClearStopInProgress('direct_control')).toBe(false);
  });

  it('clears the latch once the cycle has settled to stopped or idle', () => {
    expect(shouldClearStopInProgress('stopped')).toBe(true);
    expect(shouldClearStopInProgress('idle')).toBe(true);
    expect(shouldClearStopInProgress('running', 'held')).toBe(true);
  });
});

describe('shouldFreezeVelocitySlider', () => {
  it('freezes while a stop is in progress regardless of cycle state', () => {
    expect(shouldFreezeVelocitySlider('running', true)).toBe(true);
    expect(shouldFreezeVelocitySlider('direct_control', true)).toBe(true);
  });

  it('freezes while stopping but releases after the cycle is stopped', () => {
    expect(shouldFreezeVelocitySlider('stopping', false)).toBe(true);
    expect(shouldFreezeVelocitySlider('stopped', false)).toBe(false);
  });

  it('keeps the slider live while running, jogging, or idle', () => {
    expect(shouldFreezeVelocitySlider('running', false)).toBe(false);
    expect(shouldFreezeVelocitySlider('direct_control', false)).toBe(false);
    expect(shouldFreezeVelocitySlider('idle', false)).toBe(false);
  });
});
