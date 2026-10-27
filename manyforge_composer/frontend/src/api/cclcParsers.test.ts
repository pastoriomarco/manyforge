import { describe, expect, it } from 'vitest';
import type { CCLCIncidentDiagnostic } from '../types/cclc';

import {
  parseCCLCAdmission,
  parseCCLCComponentSnapshot,
  parseCCLCOwnerIdentity,
  parseCCLCRouteTemplateSet,
  parseCCLCStatusEnvelope,
} from './cclcParsers';
import { lifecycleRequestFromSnapshot } from '../hooks/useCCLC';

function componentSnapshot(): Record<string, unknown> {
  const snapshot = {
    active_public_pointer_digest: `sha256:${'1'.repeat(64)}`,
    component_lifecycle_generation: 4,
    component_lifecycle_state: 'prepared',
    controllable_component_id: 'component.arm',
    domains: [{
      active_control_configuration: null,
      control_domain_id: 'domain.arm',
      control_domain_revision: 'domain.revision.1',
      controller_topology_id: 'topology.arm',
      current_control_bindings: [{
        control_binding_id: 'binding.arm',
        control_binding_revision: 'binding.revision.1',
        control_route_id: 'route.normal',
      }],
      kind: 'controller',
      resources: ['joint.1'],
      topology_revision: 'topology.revision.1',
    }],
    graph_revision: 'graph.1',
    lifecycle_progress: {
      blocking_cause_class: 'availability',
      blocking_reason: 'controller_manager not ready',
      delayed: false,
      incident: null,
      next_intent: 'enable',
      phase: 'waiting_for_prerequisites',
      waiting_since_ns: 1_000,
    },
    lifecycle_target: {
      origin: 'startup',
      requested_state: 'active',
      target_generation: 1,
    },
    provider_occurrences: [{
      control_domain_id: 'domain.arm',
      lifecycle_plan_catalog_digest: `sha256:${'2'.repeat(64)}`,
      provider_instance_id: 'provider.arm',
      provider_manifest_id: 'manifest.arm',
      provider_manifest_revision: `sha256:${'3'.repeat(64)}`,
      provider_runtime_epoch: 'runtime.epoch.1',
      provider_runtime_unit_id: 'runtime.arm',
    }],
    publication_generation: 7,
    retained_bundle_id: 'bundle.1',
    snapshot_digest: `sha256:${'4'.repeat(64)}`,
  };
  return { ...snapshot, readiness: {
    request_owner_epoch: 'cclc-owner.1', evaluated_at_ns: 100,
    next_intent: 'enable', requested_state: 'active', ready: false,
    error: { code: 'component_lifecycle_transition_missing', message: 'Controller not ready', details: { reason: 'no_transition_satisfied' } },
    domains: [{ provider_occurrence: { ...snapshot.provider_occurrences[0], control_domain_revision: 'domain.revision.1' },
      health: { observed_state: 'prepared', facts: [{ lifecycle_fact_id: 'fact.loaded', value: { kind: 'boolean', boolean_value: false } }], diagnostic: null },
      decision_kind: null, error: { code: 'component_lifecycle_transition_missing', message: 'Controller not ready', details: { reason: 'no_transition_satisfied' } } }],
    measured_state: { required: true, available: true, error: null },
  } };
}

function lifecycleAcceptance(): Record<string, unknown> {
  return {
    accepted_at_ns: 100,
    component_lifecycle_generation: 5,
    lifecycle_transaction_generation: 2,
    lifecycle_transaction_id: 'lifecycle.2',
    request_id: 'request.2',
    timeout_duration_ns: 1_000_000,
  };
}

function incidentDiagnostic(): CCLCIncidentDiagnostic & {
  snapshot: NonNullable<CCLCIncidentDiagnostic['snapshot']>;
} {
  return {
    error: {
      code: 'controller_launch_exited', message: 'm'.repeat(1024),
      details: { component_id: 'component.arm' },
    },
    snapshot: {
      exit: { status: null, signal: 11, core_dumped: false, observed_utc: '2026-10-11T18:00:00Z' },
      context: {
        controllable_component_id: 'component.arm', request_context: 'Apply request.1', runtime_unit_id: 'runtime.arm',
        runtime_epoch: 'epoch.1', retained_bundle_id: 'bundle.1', graph_revision: 'graph.1',
        generator_identity: null, launch_identity: 'launch.1',
      },
      launch_readiness: {
        known: true, controller_manager_present: true, controller_observed: true,
        controller_loaded: true, controller_bound: false, arm_controller_active: null,
        hardware_ready: null, joint_sample_available: false, binding_epoch: '18446744073709551615',
        sample_age_ns: '9223372036854775807', captured_utc: null,
      },
      settings: {
        argv: ['ros2', 'launch'], controller_profile: 'lite6', runtime_namespace: '',
        arm_controller_name: 'arm_controller', gripper_controller_name: null, frame_prefix: '',
        controllers_yaml: 'controllers: {}\n', retained_configuration_json: '{"effective":true}',
      },
      generated: { robot_description_sha256: `sha256:${'a'.repeat(64)}`, controllers_sha256: null },
      output: {
        tail: 'first "line"\nCafé\n', line_count: 2, truncated: true,
        dropped_forward_lines: '18446744073709551615', dropped_forward_bytes: 0, invalid_text_bytes: 0,
      },
      core_dump_link: null,
    },
  };
}

function componentWithDiagnostic(diagnostic: unknown): Record<string, unknown> {
  const value = componentSnapshot();
  (value.lifecycle_progress as Record<string, unknown>).incident = {
    acknowledged: false, cause_class: 'provider_lost', detected_at_ns: 1000,
    incident_id: 'incident.arm.1', summary: 'Controller launch exited', vendor_codes: [], diagnostic,
  };
  return value;
}

describe('CCLC closed response parsers', () => {
  it('preserves the actual full 257-byte UR5e diagnostic launch identity', () => {
    const identity = "mock\nur5e_robot\nur5e_robotiq_2f_85\n/manyforge/ur5e_robot\njoint_trajectory_controller\nrobotiq_gripper_controller\n\nsha256:02e2b9e7733647dc75eea1a518dda0289aa07603d668d07e16dc87c79ab970fc\nsha256:d5e2658bd13c5f16c8f23c699c70fc200a4e4b904e995571c3eda1718faffec6\n";
    expect(new TextEncoder().encode(identity).length).toBe(257);
    const diagnostic = incidentDiagnostic();
    diagnostic.snapshot.context.launch_identity = identity;
    const parsed = parseCCLCComponentSnapshot(componentWithDiagnostic(diagnostic));
    expect(parsed.lifecycleProgress?.incident?.diagnostic?.snapshot?.context.launch_identity).toBe(identity);
  });

  it('uses the Close packet ceiling only for diagnostic launch identity', () => {
    const diagnostic = incidentDiagnostic();
    diagnostic.snapshot.context.launch_identity = 'x'.repeat(256 * 1024);
    expect(parseCCLCComponentSnapshot(componentWithDiagnostic(diagnostic))
      .lifecycleProgress?.incident?.diagnostic).toEqual(diagnostic);
    diagnostic.snapshot.context.launch_identity += 'x';
    expect(() => parseCCLCComponentSnapshot(componentWithDiagnostic(diagnostic))).toThrow();
    diagnostic.snapshot.context.launch_identity = 'launch.1';
    diagnostic.snapshot.context.controllable_component_id = 'x'.repeat(129);
    expect(() => parseCCLCComponentSnapshot(componentWithDiagnostic(diagnostic))).toThrow();
  });

  it('preserves closed rich incident evidence and exact full-width scalars', () => {
    const diagnostic = incidentDiagnostic();
    const incident = parseCCLCComponentSnapshot(componentWithDiagnostic(diagnostic)).lifecycleProgress?.incident;
    expect(incident?.summary).toBe('Controller launch exited');
    expect(incident?.diagnostic).toEqual(diagnostic);
    expect(incident?.diagnostic?.snapshot?.launch_readiness.sample_age_ns).toBe('9223372036854775807');
    expect(incident?.diagnostic?.snapshot?.output.dropped_forward_lines).toBe('18446744073709551615');
    const bom = incidentDiagnostic();
    bom.snapshot.settings.controllers_yaml = '\ufeffcontrollers: {}\n';
    bom.error.details = Object.fromEntries([['__proto__', 'observed']]);
    const preserved = parseCCLCComponentSnapshot(componentWithDiagnostic(bom)).lifecycleProgress?.incident?.diagnostic;
    expect(preserved).toEqual(bom);
    expect(Object.keys(preserved!.error.details)).toEqual(['__proto__']);
    expect(parseCCLCComponentSnapshot(componentWithDiagnostic(null)).lifecycleProgress?.incident?.diagnostic).toBeNull();
    const unknownFacts = incidentDiagnostic();
    for (const key of Object.keys(unknownFacts.snapshot.launch_readiness)) {
      (unknownFacts.snapshot.launch_readiness as unknown as Record<string, unknown>)[key] = null;
    }
    expect(parseCCLCComponentSnapshot(componentWithDiagnostic(unknownFacts)).lifecycleProgress?.incident?.diagnostic).toEqual(unknownFacts);
  });

  it('preserves a short-only diagnostic before a real snapshot is available', () => {
    const rich = incidentDiagnostic();
    const short = { error: rich.error, snapshot: null };
    const parsedShort = parseCCLCComponentSnapshot(componentWithDiagnostic(short)).lifecycleProgress?.incident;
    expect(parsedShort?.summary).toBe('Controller launch exited');
    expect(parsedShort?.diagnostic).toEqual(short);
    expect(parseCCLCComponentSnapshot(componentWithDiagnostic(rich)).lifecycleProgress?.incident?.diagnostic?.error).toEqual(parsedShort?.diagnostic?.error);
    for (const malformed of [
      { error: null, snapshot: null },
      { error: rich.error },
      { ...short, extra: true },
    ]) {
      expect(() => parseCCLCComponentSnapshot(componentWithDiagnostic(malformed))).toThrow();
    }
  });

  it('preserves paired informational output drop counters with exact uint64 values', () => {
    const absent = parseCCLCComponentSnapshot(componentSnapshot());
    expect(absent).not.toHaveProperty('droppedOutputLines');
    expect(absent).not.toHaveProperty('droppedOutputBytes');
    for (const [lines, bytes] of [
      [0, 0], [Number.MAX_SAFE_INTEGER, '9007199254740992'],
      ['18446744073709551615', '18446744073709551615'],
    ]) {
      const parsed = parseCCLCComponentSnapshot({
        ...componentSnapshot(), dropped_output_lines: lines, dropped_output_bytes: bytes,
      });
      expect(parsed.droppedOutputLines).toBe(lines);
      expect(parsed.droppedOutputBytes).toBe(bytes);
    }
    for (const field of ['dropped_output_lines', 'dropped_output_bytes']) {
      for (const invalid of [null, true, -1, 1.5, 2 ** 53, '01', '18446744073709551616']) {
        expect(() => parseCCLCComponentSnapshot({
          ...componentSnapshot(), dropped_output_lines: 0, dropped_output_bytes: 0, [field]: invalid,
        })).toThrow('uint64');
      }
      expect(() => parseCCLCComponentSnapshot({ ...componentSnapshot(), [field]: 0 })).toThrow('closed fields');
    }
  });

  it('parses CP9 stream health rows and refuses malformed ones', () => {
    expect(parseCCLCComponentSnapshot(componentSnapshot())).not.toHaveProperty('streamHealth');
    const row = {
      acknowledgement_required: false, age_ns: 1000, budget_ns: 250_000_000, error: null,
      recovery: 'ack', severity: 'fault', state: 'live', stream_id: 'measured_joints',
      timestamp_source: 'message_stamp',
    };
    const stale = {
      ...row, stream_id: 'sim_clock', state: 'stale', severity: 'blocking', recovery: 'auto',
      error: { code: 'component_stream_stale', message: 'stale', details: { stream_id: 'sim_clock' } },
    };
    const parsed = parseCCLCComponentSnapshot({ ...componentSnapshot(), stream_health: [row, stale] });
    expect(parsed.streamHealth).toEqual([
      {
        streamId: 'measured_joints', state: 'live', ageNs: 1000, budgetNs: 250_000_000,
        severity: 'fault', recovery: 'ack', timestampSource: 'message_stamp',
        acknowledgementRequired: false, error: null,
      },
      {
        streamId: 'sim_clock', state: 'stale', ageNs: 1000, budgetNs: 250_000_000,
        severity: 'blocking', recovery: 'auto', timestampSource: 'message_stamp',
        acknowledgementRequired: false,
        error: { code: 'component_stream_stale', message: 'stale', details: { stream_id: 'sim_clock' } },
      },
    ]);
    for (const invalid of [
      [], [{ ...row, state: 'degraded' }], [{ ...row, budget_ns: 0 }], [{ ...row, age_ns: -1 }],
      [{ ...row, extra: true }], [stale, row],
    ]) {
      expect(() => parseCCLCComponentSnapshot({ ...componentSnapshot(), stream_health: invalid })).toThrow();
    }
  });

  it('rejects unknown fields and malformed typed nullable evidence', () => {
    const invalidFacts: Array<[string[], unknown]> = [
      [['extra'], true],
      [['error', 'message'], 'm'.repeat(1025)],
      [['error', 'details'], Object.fromEntries(Array.from({ length: 9 }, (_, i) => [String(i), 'value']))],
      [['error', 'details'], { '': 'value' }],
      [['snapshot', 'extra'], {}],
      [['snapshot', 'exit', 'signal'], true],
      [['snapshot', 'exit', 'status'], 256],
      [['snapshot', 'context', 'controllable_component_id'], 'x'.repeat(129)],
      [['snapshot', 'launch_readiness', 'controller_bound'], 'unknown'],
      [['snapshot', 'launch_readiness', 'sample_age_ns'], 2 ** 53],
      [['snapshot', 'launch_readiness', 'sample_age_ns'], '9223372036854775808'],
      [['snapshot', 'launch_readiness', 'binding_epoch'], '18446744073709551616'],
      [['snapshot', 'settings', 'retained_configuration_json'], { escape: true }],
      [['snapshot', 'settings', 'frame_prefix'], 'Cafe\u0301'],
      [['snapshot', 'settings', 'argv'], ['\ud800']],
      [['snapshot', 'generated', 'controllers_sha256'], `sha256:${'A'.repeat(64)}`],
      [['snapshot', 'output', 'tail'], 'Cafe\u0301\n'],
      [['snapshot', 'output', 'line_count'], 1],
      [['snapshot', 'output', 'truncated'], 1],
      [['snapshot', 'output', 'dropped_forward_bytes'], '01'],
    ];
    for (const [path, invalid] of invalidFacts) {
      const diagnostic = incidentDiagnostic();
      let cursor = diagnostic as unknown as Record<string, unknown>;
      for (const key of path.slice(0, -1)) cursor = cursor[key] as Record<string, unknown>;
      cursor[path.at(-1)!] = invalid;
      expect(() => parseCCLCComponentSnapshot(componentWithDiagnostic(diagnostic))).toThrow();
    }
    const missing = incidentDiagnostic();
    delete (missing.snapshot.launch_readiness as unknown as Record<string, unknown>).known;
    expect(() => parseCCLCComponentSnapshot(componentWithDiagnostic(missing))).toThrow('closed fields');
    // Hard switch: superseded diagnostic names are refused.
    for (const [group, next, old] of [
      ['context', 'controllable_component_id', 'component_id'],
      ['context', 'retained_bundle_id', 'bundle_id'],
      [null, 'launch_readiness', 'readiness'],
    ] as const) {
      const superseded = incidentDiagnostic();
      const facts = (group === null ? superseded.snapshot : superseded.snapshot[group]) as unknown as Record<string, unknown>;
      facts[old] = facts[next];
      delete facts[next];
      expect(() => parseCCLCComponentSnapshot(componentWithDiagnostic(superseded))).toThrow('closed fields');
    }
  });

  it('enforces both tail caps including escaping and the diagnostic JSON budget', () => {
    const diagnostic = incidentDiagnostic();
    for (const [tail, lines] of [['', 0], ['a\nb', 2], ['a\n'.repeat(2000), 2000], ['"'.repeat(65535), 1]] as const) {
      Object.assign(diagnostic.snapshot.output, { tail, line_count: lines });
      expect(parseCCLCComponentSnapshot(componentWithDiagnostic(diagnostic)).lifecycleProgress?.incident?.diagnostic?.snapshot?.output.tail).toBe(tail);
    }
    for (const [tail, lines] of [['"'.repeat(65536), 1], ['a\n'.repeat(2001), 2001], ['😀'.repeat(32768), 1]] as const) {
      Object.assign(diagnostic.snapshot.output, { tail, line_count: lines });
      expect(() => parseCCLCComponentSnapshot(componentWithDiagnostic(diagnostic))).toThrow();
    }
    Object.assign(diagnostic.snapshot.output, { tail: '', line_count: 0 });
    diagnostic.snapshot.settings.controllers_yaml = 'x'.repeat(300 * 1024);
    expect(parseCCLCComponentSnapshot(componentWithDiagnostic(diagnostic)).lifecycleProgress?.incident?.diagnostic).toEqual(diagnostic);
    diagnostic.snapshot.settings.controllers_yaml = 'x'.repeat(512 * 1024);
    expect(() => parseCCLCComponentSnapshot(componentWithDiagnostic(diagnostic))).toThrow('512 KiB');
  });

  it('parses only the sole CCLC owner identity field', () => {
    expect(parseCCLCOwnerIdentity({
      request_owner_epoch: 'cclc-owner.1',
    })).toEqual({ requestOwnerEpoch: 'cclc-owner.1' });
    expect(() => parseCCLCOwnerIdentity({
      request_owner_epoch: 'cclc-owner.1', extra: true,
    })).toThrow('closed fields');
  });

  it('projects one complete owner snapshot and rejects noncanonical scope', () => {
    const value = componentSnapshot();
    const parsed = parseCCLCComponentSnapshot(value);
    expect(parsed).toMatchObject({
      controllableComponentId: 'component.arm',
      lifecycleGeneration: 4,
      lifecycleState: 'prepared',
      graphRevision: 'graph.1',
      providers: [{
        controlDomainId: 'domain.arm',
        providerInstanceId: 'provider.arm',
        providerRuntimeUnitId: 'runtime.arm',
      }],
    });

    const torn = structuredClone(value);
    const domain = (torn.domains as Array<Record<string, unknown>>)[0]!;
    domain.resources = ['joint.2', 'joint.1'];
    expect(() => parseCCLCComponentSnapshot(torn)).toThrow('canonical');
  });

  it('projects the requested target and owner progress separately from the achieved state', () => {
    const parsed = parseCCLCComponentSnapshot(componentSnapshot());
    expect(parsed.lifecycleState).toBe('prepared');
    expect(parsed.lifecycleTarget).toEqual({
      requestedState: 'active', origin: 'startup', targetGeneration: 1,
    });
    expect(parsed.lifecycleProgress).toEqual({
      phase: 'waiting_for_prerequisites',
      nextIntent: 'enable',
      blockingCauseClass: 'availability',
      blockingReason: 'controller_manager not ready',
      waitingSinceNs: 1_000,
      delayed: false,
      incident: null,
    });

    const nulls = componentSnapshot();
    nulls.lifecycle_target = null;
    nulls.lifecycle_progress = null;
    const parsedNulls = parseCCLCComponentSnapshot(nulls);
    expect(parsedNulls.lifecycleTarget).toBeNull();
    expect(parsedNulls.lifecycleProgress).toBeNull();

    const nullableProgress = componentSnapshot();
    Object.assign(nullableProgress.lifecycle_progress as Record<string, unknown>, {
      blocking_cause_class: 'none',
      blocking_reason: null,
      next_intent: null,
      phase: 'satisfied',
      waiting_since_ns: null,
    });
    expect(parseCCLCComponentSnapshot(nullableProgress).lifecycleProgress).toMatchObject({
      phase: 'satisfied', nextIntent: null, blockingReason: null, waitingSinceNs: null,
    });
  });

  it('rejects missing, widened or unknown lifecycle target and progress facts', () => {
    for (const key of ['lifecycle_target', 'lifecycle_progress']) {
      const missing = componentSnapshot();
      delete missing[key];
      expect(() => parseCCLCComponentSnapshot(missing)).toThrow('closed fields');
    }

    const rejected: Array<[string, string, unknown, string]> = [
      ['lifecycle_target', 'requested_state', 'degraded', 'active, prepared or disabled'],
      ['lifecycle_target', 'origin', 'browser', 'startup, operator or program'],
      ['lifecycle_target', 'target_generation', 0, 'positive safe integer'],
      ['lifecycle_progress', 'phase', 'retrying', 'known lifecycle progress phase'],
      ['lifecycle_progress', 'next_intent', 'restart', 'known lifecycle intent'],
      ['lifecycle_progress', 'blocking_cause_class', 'estop', 'known blocking cause class'],
      ['lifecycle_progress', 'blocking_reason', '', 'bounded non-empty NFC text'],
      ['lifecycle_progress', 'delayed', 'yes', 'boolean'],
      ['lifecycle_progress', 'waiting_since_ns', -1, 'nanoseconds'],
    ];
    for (const [field, key, invalid, message] of rejected) {
      const value = componentSnapshot();
      (value[field] as Record<string, unknown>)[key] = invalid;
      expect(() => parseCCLCComponentSnapshot(value)).toThrow(message);
    }

    const widened = componentSnapshot();
    (widened.lifecycle_progress as Record<string, unknown>).acknowledged = true;
    expect(() => parseCCLCComponentSnapshot(widened)).toThrow('closed fields');
    const narrowed = componentSnapshot();
    delete (narrowed.lifecycle_target as Record<string, unknown>).origin;
    expect(() => parseCCLCComponentSnapshot(narrowed)).toThrow('closed fields');
  });

  it('projects an open incident with the new cause classes and rejects a malformed one', () => {
    const incident = (): Record<string, unknown> => ({
      acknowledged: false,
      cause_class: 'collision',
      detected_at_ns: 1_759_900_000_000,
      incident_id: 'incident.arm.1',
      summary: 'Collision detected while executing',
      vendor_codes: ['C31', 'state=4', 'mode=0'],
    });
    for (const cause of ['collision', 'external_control', 'provider_lost'] as const) {
      const value = componentSnapshot();
      Object.assign(value.lifecycle_progress as Record<string, unknown>, {
        blocking_cause_class: cause,
        phase: 'blocked',
        incident: { ...incident(), cause_class: cause },
      });
      const parsed = parseCCLCComponentSnapshot(value).lifecycleProgress;
      expect(parsed?.blockingCauseClass).toBe(cause);
      expect(parsed?.incident).toEqual({
        incidentId: 'incident.arm.1',
        causeClass: cause,
        vendorCodes: ['C31', 'state=4', 'mode=0'],
        detectedAtNs: 1_759_900_000_000,
        summary: 'Collision detected while executing',
        acknowledged: false,
      });
    }

    const rejected: Array<[string, unknown, string]> = [
      ['cause_class', 'availability', 'intervention cause class'],
      ['cause_class', 'none', 'intervention cause class'],
      ['incident_id', '', 'bounded non-empty NFC text'],
      ['incident_id', 'i'.repeat(129), 'bounded non-empty NFC text'],
      ['summary', 'Cafe\u0301', 'bounded non-empty NFC text'],
      ['summary', 's'.repeat(1025), 'bounded non-empty NFC text'],
      ['vendor_codes', 'C31', 'bounded vendor codes'],
      ['vendor_codes', [''], 'bounded vendor codes'],
      ['vendor_codes', Array(17).fill('C'), 'bounded vendor codes'],
      ['detected_at_ns', -1, 'nanoseconds'],
      ['acknowledged', true, 'false'],
    ];
    for (const [key, invalid, message] of rejected) {
      const value = componentSnapshot();
      (value.lifecycle_progress as Record<string, unknown>).incident = {
        ...incident(), [key]: invalid,
      };
      expect(() => parseCCLCComponentSnapshot(value)).toThrow(message);
    }
    const widened = componentSnapshot();
    (widened.lifecycle_progress as Record<string, unknown>).incident = {
      ...incident(), occurred: true,
    };
    expect(() => parseCCLCComponentSnapshot(widened)).toThrow('closed fields');
    const missing = componentSnapshot();
    delete (missing.lifecycle_progress as Record<string, unknown>).incident;
    expect(() => parseCCLCComponentSnapshot(missing)).toThrow('closed fields');
  });

  it('accepts only exact retained route templates', () => {
    const value = {
      controllable_component_id: 'component.arm',
      graph_revision: 'graph.1',
      retained_bundle_id: 'bundle.1',
      templates: [{
        component_expectations: [{
          controllable_component_id: 'component.arm',
          expected_component_lifecycle_generation: 4,
          expected_component_lifecycle_state: 'prepared',
        }],
        desired_configuration: {
          activation_set_id: 'activation.arm',
          binding_routes: [{
            control_binding_id: 'binding.arm',
            control_binding_revision: 'binding.revision.1',
            control_route_id: 'route.fast',
          }],
          control_domain_id: 'domain.arm',
          control_domain_revision: 'domain.revision.1',
          controller_topology_id: 'topology.arm',
          topology_revision: 'topology.revision.1',
        },
        expected_active_control_configuration: { presence: 'absent' },
      }],
    };
    expect(parseCCLCRouteTemplateSet(value).templates).toHaveLength(1);

    const disabled = structuredClone(value);
    disabled.templates[0].component_expectations[0].expected_component_lifecycle_state = 'disabled';
    expect(parseCCLCRouteTemplateSet(disabled).templates[0].component_expectations[0]
      .expected_component_lifecycle_state).toBe('disabled');
    disabled.templates[0].component_expectations[0].expected_component_lifecycle_state = 'faulted';
    expect(() => parseCCLCRouteTemplateSet(disabled)).toThrow('prepared, active or disabled');

    const widened = structuredClone(value);
    (widened.templates[0] as Record<string, unknown>).caller_override = true;
    expect(() => parseCCLCRouteTemplateSet(widened)).toThrow('closed fields');
  });

  it('joins admission and status to the exact accepted transaction', () => {
    const acceptance = lifecycleAcceptance();
    const status = {
      acceptance,
      accepted_at_ns: 100,
      controllable_component_id: 'component.arm',
      current_state: 'prepared',
      domains: [{
        action_state: 'not_started',
        control_domain_id: 'domain.arm',
        current_state: 'prepared',
        effect: 'none',
        error: null,
      }],
      error: null,
      latest_updated_at_ns: 110,
      phase: 'accepted',
      previous_state: 'declared',
      reconciliation_required: false,
      remaining_duration_ns: 999_990,
      status_owner_epoch: 'cclc.epoch.1',
      status_sequence: 1,
      terminal_result: null,
    };
    const result = parseCCLCAdmission({
      acceptance,
      replay_status: 'accepted_new',
      status,
    }, 'lifecycle');
    expect(result).toMatchObject({
      requestId: 'request.2',
      transactionId: 'lifecycle.2',
      status: { phase: 'accepted', reconciliationRequired: false },
    });
    expect(parseCCLCStatusEnvelope({ status }, 'lifecycle')?.transactionId)
      .toBe('lifecycle.2');

    const failed = {
      ...structuredClone(status),
      phase: 'failed',
      terminal_result: 'failed_known_no_effect',
      error: {
        code: 'native_lifecycle_route_unavailable',
        message: 'The native route did not become ready',
        details: { reason: 'route_readback_missing' },
      },
    };
    expect(parseCCLCStatusEnvelope({ status: failed }, 'lifecycle')?.error)
      .toEqual(failed.error);

    const mismatched = structuredClone(status);
    (mismatched.acceptance as Record<string, unknown>).lifecycle_transaction_id =
      'lifecycle.foreign';
    expect(() => parseCCLCAdmission({
      acceptance,
      replay_status: 'accepted_new',
      status: mismatched,
    }, 'lifecycle')).toThrow('accepted transaction status');
  });

  it('accepts a Recover that only acknowledged a latched stream (CP9)', () => {
    const acknowledged = { acknowledged_at_ns: 9_000, controllable_component_id: 'component.arm' };
    expect(parseCCLCAdmission({
      acceptance: null,
      replay_status: 'accepted_new',
      status: null,
      stream_acknowledgement: acknowledged,
    }, 'lifecycle')).toEqual({
      replayStatus: 'accepted_new',
      requestId: null,
      transactionId: null,
      status: null,
      incidentRecovery: null,
      streamAcknowledgement: { controllableComponentId: 'component.arm', acknowledgedAtNs: 9_000 },
    });
    expect(() => parseCCLCAdmission({
      acceptance: null, replay_status: 'replayed_exact', status: null,
      stream_acknowledgement: acknowledged,
    }, 'lifecycle')).toThrow('without a transaction');
    expect(() => parseCCLCAdmission({
      acceptance: null, replay_status: 'accepted_new', status: null,
      stream_acknowledgement: { ...acknowledged, stream_id: 'measured_joints' },
    }, 'lifecycle')).toThrow('closed fields');
    expect(() => parseCCLCAdmission({
      acceptance: null, replay_status: 'accepted_new', status: null,
      stream_acknowledgement: acknowledged,
    }, 'route')).toThrow('closed fields');
  });

  it('accepts the authorization-only Recover of an open incident and nothing looser', () => {
    const recovery = {
      acknowledged_at_ns: 9_000,
      acknowledged_by: 'manyforge_composer',
      cause_class: 'emergency_stop',
      controllable_component_id: 'component.arm',
      incident_id: 'incident.component.arm.5000',
    };
    for (const replayStatus of ['accepted_new', 'replayed_exact'] as const) {
      expect(parseCCLCAdmission({
        acceptance: null,
        incident_recovery: recovery,
        replay_status: replayStatus,
        status: null,
      }, 'lifecycle')).toEqual({
        replayStatus,
        requestId: null,
        transactionId: null,
        status: null,
        incidentRecovery: {
          incidentId: 'incident.component.arm.5000',
          controllableComponentId: 'component.arm',
          causeClass: 'emergency_stop',
          acknowledgedBy: 'manyforge_composer',
          acknowledgedAtNs: 9_000,
        },
      });
    }
    // An ordinary admission carries no recovery.
    expect(parseCCLCAdmission({
      acceptance: null, replay_status: 'stale_request_owner_epoch', status: null,
    }, 'lifecycle').incidentRecovery).toBeNull();
    // Only a lifecycle result can carry it, never with a transaction or refusal.
    expect(() => parseCCLCAdmission({
      acceptance: null, incident_recovery: recovery, replay_status: 'accepted_new', status: null,
    }, 'route')).toThrow('closed fields');
    expect(() => parseCCLCAdmission({
      acceptance: null,
      incident_recovery: recovery,
      replay_status: 'request_id_conflict',
      status: null,
    }, 'lifecycle')).toThrow('authorization-only');
    expect(() => parseCCLCAdmission({
      acceptance: null,
      incident_recovery: { ...recovery, cause_class: 'availability' },
      replay_status: 'accepted_new',
      status: null,
    }, 'lifecycle')).toThrow('cause_class');
    const extra = { ...recovery, summary: 'not in the contract' };
    expect(() => parseCCLCAdmission({
      acceptance: null, incident_recovery: extra, replay_status: 'accepted_new', status: null,
    }, 'lifecycle')).toThrow('closed fields');
  });

  it('retains only the typed Close incident acknowledgement reason', () => {
    const recovery = {
      acknowledged_at_ns: 9000, acknowledged_by: 'manyforge_composer',
      cause_class: 'emergency_stop', controllable_component_id: 'component.arm',
      incident_id: 'incident.arm', closure_reason: 'cell_closed_by_operator',
    };
    const envelope = { acceptance: null, incident_recovery: recovery,
      replay_status: 'accepted_new', status: null };
    expect(parseCCLCAdmission(envelope, 'lifecycle').incidentRecovery?.closureReason)
      .toBe('cell_closed_by_operator');
    for (const invalid of [null, '', 'recovered', true]) {
      expect(() => parseCCLCAdmission({ ...envelope,
        incident_recovery: { ...recovery, closure_reason: invalid } }, 'lifecycle'))
        .toThrow('closure_reason');
    }
  });

  it('keeps full-width owner nanoseconds exact and refuses noncanonical spellings', () => {
    // Owner clocks exceed 2^53 ns after ~104 days: larger values arrive as
    // canonical decimal text and must stay text, never a rounded number.
    const aboveSafe = '9007199254740993';
    const maxInt64 = '9223372036854775807';
    const incident = {
      acknowledged: false,
      cause_class: 'fault',
      detected_at_ns: aboveSafe,
      incident_id: 'incident.arm.2',
      summary: 'Controller fault',
      vendor_codes: [],
    };
    const snapshot = componentSnapshot();
    Object.assign(snapshot.lifecycle_progress as Record<string, unknown>, {
      blocking_cause_class: 'fault',
      phase: 'blocked',
      incident,
      waiting_since_ns: maxInt64,
    });
    const progress = parseCCLCComponentSnapshot(snapshot).lifecycleProgress;
    expect(progress?.waitingSinceNs).toBe(maxInt64);
    expect(progress?.incident?.detectedAtNs).toBe(aboveSafe);

    for (const [waiting, detected] of [
      [Number.MAX_SAFE_INTEGER, 0], ['1000', '0'],
    ] as const) {
      const safe = componentSnapshot();
      Object.assign(safe.lifecycle_progress as Record<string, unknown>, {
        blocking_cause_class: 'fault',
        phase: 'blocked',
        incident: { ...incident, detected_at_ns: detected },
        waiting_since_ns: waiting,
      });
      const parsed = parseCCLCComponentSnapshot(safe).lifecycleProgress;
      expect(parsed?.waitingSinceNs).toBe(waiting);
      expect(parsed?.incident?.detectedAtNs).toBe(detected);
    }

    const recovery = {
      acknowledged_at_ns: aboveSafe,
      acknowledged_by: 'manyforge_composer',
      cause_class: 'fault',
      controllable_component_id: 'component.arm',
      incident_id: 'incident.arm.2',
    };
    expect(parseCCLCAdmission({
      acceptance: null, incident_recovery: recovery, replay_status: 'accepted_new', status: null,
    }, 'lifecycle').incidentRecovery?.acknowledgedAtNs).toBe(aboveSafe);

    const acceptance = { ...lifecycleAcceptance(), accepted_at_ns: maxInt64 };
    expect(parseCCLCAdmission({
      acceptance, replay_status: 'accepted_new', status: null,
    }, 'lifecycle').transactionId).toBe('lifecycle.2');

    for (const invalid of [
      -1, Number.MAX_SAFE_INTEGER + 1, 1.5, true, '', '01', '-1', '+1', '1e3', ' 1',
      '9223372036854775808',
    ]) {
      const waiting = componentSnapshot();
      (waiting.lifecycle_progress as Record<string, unknown>).waiting_since_ns = invalid;
      expect(() => parseCCLCComponentSnapshot(waiting)).toThrow('nanoseconds');
      const detected = componentSnapshot();
      Object.assign(detected.lifecycle_progress as Record<string, unknown>, {
        blocking_cause_class: 'fault',
        phase: 'blocked',
        incident: { ...incident, detected_at_ns: invalid },
      });
      expect(() => parseCCLCComponentSnapshot(detected)).toThrow('nanoseconds');
      expect(() => parseCCLCAdmission({
        acceptance: null,
        incident_recovery: { ...recovery, acknowledged_at_ns: invalid },
        replay_status: 'accepted_new',
        status: null,
      }, 'lifecycle')).toThrow('nanoseconds');
    }
  });
});

function changeReadiness(path: string[], value: unknown) {
  const snapshot = componentSnapshot();
  let row = snapshot as Record<string, unknown>;
  for (const key of path.slice(0, -1)) row = row[key] as Record<string, unknown>;
  row[path.at(-1)!] = value;
  return snapshot;
}

describe('Q114 closed readiness', () => {
  it.each([true, false])('preserves optional activation fact %s without changing readiness', (value) => {
    const wire = changeReadiness(['readiness', 'domains', '0', 'has_activation_step'], value);
    const parsed = parseCCLCComponentSnapshot(wire).readiness;
    expect(parsed.providerOccurrences[0]?.hasActivationStep).toBe(value);
    expect(parsed.ready).toBe(false);
    expect(parsed.nextIntent).toBe('enable');
    expect(parsed.providerOccurrences[0]?.health).toEqual(parseCCLCComponentSnapshot(componentSnapshot()).readiness.providerOccurrences[0]?.health);
    expect(parsed.error?.details.reason).toBe('no_transition_satisfied');
  });
  it('keeps an absent activation fact unstated', () => {
    const domain = parseCCLCComponentSnapshot(componentSnapshot()).readiness.providerOccurrences[0]!;
    expect(domain.hasActivationStep).toBeUndefined();
    expect(Object.hasOwn(domain, 'hasActivationStep')).toBe(false);
  });
  it.each([null, 'false', 0, undefined])('rejects a nonboolean present activation fact %j', (value) => {
    expect(() => parseCCLCComponentSnapshot(changeReadiness(
      ['readiness', 'domains', '0', 'has_activation_step'], value))).toThrow();
  });
  it('keeps the extended domain wire closed', () => {
    expect(() => parseCCLCComponentSnapshot(changeReadiness(
      ['readiness', 'domains', '0', 'hasActivationStep'], false))).toThrow('closed fields');
  });
  it('preserves exact current health facts, owner clock and blockers', () => {
    const parsed = parseCCLCComponentSnapshot(componentSnapshot()).readiness;
    expect(parsed.requestOwnerEpoch).toBe('cclc-owner.1');
    expect(parsed.ready).toBe(false);
    expect(parsed.providerOccurrences[0]?.health?.facts[0]?.value).toEqual({ kind: 'boolean', booleanValue: false });
    expect(parsed.error?.details.reason).toBe('no_transition_satisfied');
    expect(parseCCLCComponentSnapshot(changeReadiness(['readiness', 'evaluated_at_ns'], '9223372036854775807')).readiness.evaluatedAtNs)
      .toBe('9223372036854775807');
  });
  it.each([
    [['readiness'], undefined], [['readiness', 'extra'], true],
    [['readiness', 'next_intent'], null], [['readiness', 'requested_state'], 'prepared'],
    [['readiness', 'ready'], 1], [['readiness', 'evaluated_at_ns'], true],
    [['readiness', 'evaluated_at_ns'], '9223372036854775808'],
    [['readiness', 'domains'], []],
    [['readiness', 'domains', '0', 'provider_occurrence', 'control_domain_revision'], 'foreign'],
    [['readiness', 'domains', '0', 'provider_occurrence', 'lifecycle_plan_catalog_digest'], 'foreign'],
    [['readiness', 'domains', '0', 'health', 'facts', '0', 'value'], { kind: 'boolean', boolean_value: 'false' }],
    [['readiness', 'domains', '0', 'health', 'facts', '0', 'value'], { kind: 'integer', integer_value: true }],
    [['readiness', 'domains', '0', 'health', 'facts', '0', 'value'], { kind: 'integer', integer_value: '1' }],
    [['readiness', 'domains', '0', 'health', 'facts', '0', 'value'], { kind: 'unknown', boolean_value: false }],
    [['readiness', 'measured_state'], { required: false, available: false, error: null }],
    [['readiness', 'measured_state'], { required: null, available: null, error: null }],
    [['readiness', 'ready'], true],
  ] as Array<[string[], unknown]>)('rejects torn or invalid readiness at %j', (path, value) => {
    expect(() => parseCCLCComponentSnapshot(changeReadiness(path, value))).toThrow();
  });
  it('keeps durable Active next-step fields null and genuine unavailable health visible', () => {
    const snapshot = componentSnapshot();
    snapshot.component_lifecycle_state = 'active';
    const readiness = snapshot.readiness as Record<string, unknown>;
    readiness.next_intent = null; readiness.requested_state = null;
    expect(parseCCLCComponentSnapshot(snapshot).readiness.ready).toBe(false);
    readiness.ready = true;
    expect(() => parseCCLCComponentSnapshot(snapshot)).toThrow();
  });
  it('keeps lifecycle readiness separate from exact measured-state availability', () => {
    const snapshot = componentSnapshot();
    const readiness = snapshot.readiness as { ready: boolean; error: unknown;
      domains: Array<{error: unknown; decision_kind: string | null}>; measured_state: unknown };
    readiness.ready = true;
    readiness.error = null;
    readiness.domains[0].error = null;
    readiness.domains[0].decision_kind = 'dispatch_primitive';
    readiness.measured_state = { required: true, available: false,
      error: { code: 'observation_stale', message: 'Required measured state is stale', details: {} } };
    const parsed = parseCCLCComponentSnapshot(snapshot);
    expect(parsed.readiness.ready).toBe(true);
    expect(parsed.readiness.measuredState.available).toBe(false);
    expect(parsed.readiness.measuredState.error?.code).toBe('observation_stale');
  });
  it('rejects duplicate health occurrences and fact IDs', () => {
    const snapshot = componentSnapshot();
    const readiness = snapshot.readiness as { domains: Array<{health: {facts: unknown[]}}> };
    readiness.domains.push(readiness.domains[0]!);
    expect(() => parseCCLCComponentSnapshot(snapshot)).toThrow();
    const facts = readiness.domains[0]!.health.facts;
    readiness.domains.pop(); facts.push(facts[0]);
    expect(() => parseCCLCComponentSnapshot(snapshot)).toThrow();
  });
});

describe('Q114 original diagnostic bounds', () => {
  it('preserves general measured/provider errors beyond eight details and long lifecycle messages', () => {
    const error = { code: 'measured_source_unavailable', message: 'm'.repeat(1024),
      details: Object.fromEntries(Array.from({ length: 64 }, (_, index) => [`fact.${index}`, `actual.${index}`])) };
    const snapshot = componentSnapshot();
    const readiness = snapshot.readiness as Record<string, unknown>;
    readiness.error = error;
    readiness.measured_state = { required: true, available: false, error };
    (readiness.domains as Array<Record<string, unknown>>)[0]!.error = error;
    const parsed = parseCCLCComponentSnapshot(snapshot).readiness;
    expect(parsed.error).toEqual(error);
    expect(parsed.measuredState.error).toEqual(error);
    expect(parsed.providerOccurrences[0]?.error).toEqual(error);
    error.details.extra = 'actual';
    expect(() => parseCCLCComponentSnapshot(snapshot)).toThrow();
  });
  it('retains the normalized health diagnostic eight-detail limit', () => {
    const diagnostic = { code: 'provider_diagnostic', message: 'm'.repeat(1024),
      details: Object.fromEntries(Array.from({ length: 8 }, (_, index) => [`fact.${index}`, `actual.${index}`])) };
    const snapshot = changeReadiness(['readiness', 'domains', '0', 'health', 'diagnostic'], diagnostic);
    expect(parseCCLCComponentSnapshot(snapshot).readiness.providerOccurrences[0]?.health?.diagnostic).toEqual(diagnostic);
    diagnostic.details.extra = 'actual';
    expect(() => parseCCLCComponentSnapshot(snapshot)).toThrow();
  });
});

// Source-derived provider-domain sample in the agreed contract 125/128 response shape
// (kind "io_provider", common `resources`). It is NOT a capture from the normal producer;
// it is re-pinned from Track 2's real capture later.
const CORE_PROVIDER_DOMAIN_SAMPLE = `{
  "control_domain_id": "domain.arm",
  "control_domain_revision": "domain.rev.1",
  "kind": "io_provider",
  "provider": {
    "controllable_component_id": "component.arm",
    "provider_runtime_unit_id": "runtime.arm",
    "provider_instance_id": "provider.arm",
    "provider_manifest_id": "manifest.arm",
    "provider_manifest_revision": "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
    "lifecycle_plan_catalog_digest": "sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
    "signal_configuration_digest": "sha256:1111111111111111111111111111111111111111111111111111111111111111",
    "signal_names": [
      "ready"
    ],
    "lifecycle_command_timeout_ns": 30000000000
  },
  "provider_runtime_epoch": "epoch.1",
  "resources": [],
  "active_control_configuration": {
    "active_control_configuration_id": "provider.configuration",
    "active_control_configuration_generation": 9007199254740991,
    "source_runtime_unit_id": "runtime.arm",
    "source_runtime_epoch": "epoch.1",
    "source_sequence": 9007199254740991,
    "signal_configuration_digest": "sha256:1111111111111111111111111111111111111111111111111111111111111111"
  }
}`;

type Wire = Record<string, any>; // eslint-disable-line @typescript-eslint/no-explicit-any

function providerDomainSnapshot(): Wire {
  const snapshot = componentSnapshot() as Wire;
  const domain = JSON.parse(CORE_PROVIDER_DOMAIN_SAMPLE) as Wire;
  const provider = domain.provider as Wire;
  const occurrence = {
    control_domain_id: domain.control_domain_id,
    lifecycle_plan_catalog_digest: provider.lifecycle_plan_catalog_digest,
    provider_instance_id: provider.provider_instance_id,
    provider_manifest_id: provider.provider_manifest_id,
    provider_manifest_revision: provider.provider_manifest_revision,
    provider_runtime_epoch: domain.provider_runtime_epoch,
    provider_runtime_unit_id: provider.provider_runtime_unit_id,
  };
  snapshot.domains = [domain];
  snapshot.provider_occurrences = [occurrence];
  snapshot.readiness.domains[0].provider_occurrence = {
    ...occurrence, control_domain_revision: domain.control_domain_revision,
  };
  snapshot.readiness.measured_state = { required: false, available: null, error: null };
  return snapshot;
}

describe('provider-domain component snapshot', () => {
  it('parses the pinned Core provider domain and echoes it as the provider expected scope', () => {
    const parsed = parseCCLCComponentSnapshot(providerDomainSnapshot());
    const domain = JSON.parse(CORE_PROVIDER_DOMAIN_SAMPLE) as Wire;
    expect(parsed.domains).toEqual([{
      kind: 'io_provider',
      controlDomainId: 'domain.arm',
      controlDomainRevision: 'domain.rev.1',
      provider: domain.provider,
      providerRuntimeEpoch: 'epoch.1',
      resources: [],
      activeConfiguration: { id: 'provider.configuration', generation: 9007199254740991 },
    }]);
    const context = {
      request_id: 'request.1', request_owner_epoch: 'owner.1', source_label: 'composer', correlation_id: null,
    };
    expect(lifecycleRequestFromSnapshot(parsed, 'enable', context, 7).expected_control_domains).toEqual([{
      kind: 'io_provider',
      control_domain_id: 'domain.arm',
      control_domain_revision: 'domain.rev.1',
      provider: domain.provider,
      expected_active_control_configuration: {
        presence: 'present',
        active_control_configuration_id: 'provider.configuration',
        active_control_configuration_generation: 9007199254740991,
      },
    }]);
    const unconfigured = providerDomainSnapshot();
    unconfigured.domains[0].active_control_configuration = null;
    unconfigured.domains[0].resources = ['io.do.1', 'io.do.2'];
    expect(parseCCLCComponentSnapshot(unconfigured).domains[0]?.activeConfiguration).toBeNull();
  });

  it.each<[string, (snapshot: Wire) => void]>([
    ['physical bindings', (s) => { s.domains[0].current_control_bindings = []; }],
    ['old controller resources', (s) => { s.domains[0].current_resources = ['joint.1']; }],
    ['old output resources', (s) => { s.domains[0].output_resources = s.domains[0].resources; }],
    ['renamed output resources', (s) => {
      s.domains[0].output_resources = s.domains[0].resources;
      delete s.domains[0].resources;
    }],
    ['absent resources', (s) => { delete s.domains[0].resources; }],
    ['controller topology', (s) => { s.domains[0].controller_topology_id = 'topology.arm'; }],
    ['controller kind', (s) => { s.domains[0].kind = 'controller'; }],
    ['absent kind', (s) => { delete s.domains[0].kind; }],
    ['superseded provider kind', (s) => { s.domains[0].kind = 'provider'; }],
    ['absent configuration member', (s) => { delete s.domains[0].active_control_configuration; }],
    ['unsorted outputs', (s) => { s.domains[0].resources = ['io.2', 'io.1']; }],
    ['empty signals', (s) => { s.domains[0].provider.signal_names = []; }],
    ['string timeout', (s) => { s.domains[0].provider.lifecycle_command_timeout_ns = '30000000000'; }],
    ['non-digest revision', (s) => { s.domains[0].provider.provider_manifest_revision = 'rev.1'; }],
    ['extra provider field', (s) => { s.domains[0].provider.extra = true; }],
    ['controller configuration fields', (s) => { s.domains[0].active_control_configuration.state = 'reconciled'; }],
    ['foreign configuration epoch', (s) => { s.domains[0].active_control_configuration.source_runtime_epoch = 'epoch.2'; }],
    ['foreign signal digest', (s) => {
      s.domains[0].active_control_configuration.signal_configuration_digest = `sha256:${'9'.repeat(64)}`;
    }],
    ['foreign component', (s) => { s.domains[0].provider.controllable_component_id = 'component.other'; }],
    ['foreign occurrence', (s) => { s.provider_occurrences[0].provider_instance_id = 'provider.other'; }],
    ['missing occurrence', (s) => { s.provider_occurrences = []; }],
  ])('refuses a provider domain with %s', (_label, mutate) => {
    const snapshot = providerDomainSnapshot();
    mutate(snapshot);
    expect(() => parseCCLCComponentSnapshot(snapshot)).toThrow();
  });

  it.each(['current_control_bindings', 'resources'])(
    'still refuses a controller domain with empty %s',
    (field) => {
      const snapshot = componentSnapshot() as Wire;
      snapshot.domains[0][field] = [];
      expect(() => parseCCLCComponentSnapshot(snapshot)).toThrow();
    },
  );

  it.each<[string, (domain: Wire) => void]>([
    ['absent kind', (d) => { delete d.kind; }],
    ['unknown kind', (d) => { d.kind = 'io'; }],
    ['superseded provider kind', (d) => { d.kind = 'provider'; }],
    ['io_provider kind', (d) => { d.kind = 'io_provider'; }],
    ['absent resources', (d) => { delete d.resources; }],
    ['renamed current_resources', (d) => { d.current_resources = d.resources; delete d.resources; }],
    ['extra current_resources', (d) => { d.current_resources = ['joint.1']; }],
    ['extra output_resources', (d) => { d.output_resources = ['joint.1']; }],
  ])('refuses a controller domain with %s', (_label, mutate) => {
    const snapshot = componentSnapshot() as Wire;
    mutate(snapshot.domains[0]);
    expect(() => parseCCLCComponentSnapshot(snapshot)).toThrow();
  });
});
