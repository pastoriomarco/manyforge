import { describe, expect, it } from 'vitest';
import {
  StructuralContractError,
  parseCatalogResponse,
  parseExecutionOwnerStatus,
  parseHealthResponse,
  parseHeadlessRuntimeSnapshot,
  parseHistorianPage,
  parseOperatorAlarmSnapshot,
  parseMotionPreviewResponse,
  parseSceneUrdfManifest,
} from './parsers';

function sourceSectionFixture(): Record<string, unknown> {
  return {
    available: true,
    freshness: 'fresh',
    known_gaps: [],
    payload: { schema_version: 'fixture.v1' },
    source_generation: 1,
    source_owner_epoch: 'source-epoch-1',
    source_owner_id: 'source-owner',
    receiver_age_ns: 100,
  };
}

function runtimeSnapshotFixture(): Record<string, unknown> {
  return {
    schema_version: 'manyforge.headless_runtime_snapshot.v1',
    complete: true,
    projection_generation: 4,
    projection_owner_epoch: 'projection-epoch-1',
    projection_owner_id: 'headless-api',
    reasons: [],
    unavailable_sections: [],
    sections: {
      manager: sourceSectionFixture(),
      logical_scene: sourceSectionFixture(),
      scene_runtime: sourceSectionFixture(),
    },
  };
}

function alarmSnapshotFixture(): Record<string, unknown> {
  const alarm = {
    condition_id: 'condition-1',
    activation_id: 'activation-1',
    record_revision: 3,
    activated_at_ns: 100,
    definition_id: 'manyforge.test.condition',
    lifecycle_state: 'active_unacknowledged',
    active: true,
    acknowledged: false,
    priority: 'high',
    alarm_class: 'runtime_health',
    title: 'Test alarm',
    message: 'A condition requires attention.',
    consequence: 'Runtime state may be incomplete.',
    operator_response: 'Inspect the source.',
    occurrence_count: 2,
    requires_motion_attention: false,
    subject: {
      subject_kind: 'cell',
      subject_id: 'cell-1',
      resource_ids: [],
    },
    details: { source: 'fixture' },
  };
  return {
    schema_version: 'manyforge.operator_alarm_snapshot.v2',
    owner_id: 'alarm-owner',
    owner_epoch: 'alarm-epoch-1',
    generation: 2,
    state_revision: 3,
    reconciled: true,
    health: { state: 'available', details: {} },
    live_count: 1,
    unacknowledged_count: 1,
    active_count: 1,
    active_acknowledged_count: 0,
    motion_attention_count: 0,
    unacknowledged_membership: [{
      condition_id: 'condition-1',
      activation_id: 'activation-1',
      record_revision: 3,
    }],
    alarms: [alarm],
    truncated: false,
  };
}

function structuralManifestFixture(): Record<string, unknown> {
  return {
    robot_id: 'fixture-arm',
    model_key: 'model-key-123',
    root_link: 'base_link',
    projection: {
      kind: 'authored_neutral',
      source: 'authored_urdf',
      root_frame: 'base_link',
      position_unit: 'm',
      angle_unit: 'rad',
      quaternion_order: 'xyzw',
      joint_coordinate_policy: 'zero_independent_with_authored_mimic',
      operational_validity: 'not_asserted',
      runtime_state_used: false,
    },
    links: [
      {
        name: 'base_link',
        neutral_transform: {
          relative_to: 'base_link',
          position_m: [0, 0, 0],
          orientation_xyzw: [0, 0, 0, 1],
        },
        visuals: [],
        collisions: [],
      },
    ],
    joints: [],
  };
}

describe('parseSceneUrdfManifest', () => {
  it('accepts a complete authored-neutral projection', () => {
    const parsed = parseSceneUrdfManifest(structuralManifestFixture());

    expect(parsed.projection.runtime_state_used).toBe(false);
    expect(parsed.projection.operational_validity).toBe('not_asserted');
    expect(parsed.links[0]?.neutral_transform.relative_to).toBe('base_link');
  });

  it('rejects a projection that claims runtime state was used', () => {
    const fixture = structuralManifestFixture();
    const projection = fixture.projection as Record<string, unknown>;
    projection.runtime_state_used = true;

    expect(() => parseSceneUrdfManifest(fixture)).toThrowError(
      StructuralContractError,
    );
  });

  it('rejects links without explicit neutral transforms', () => {
    const fixture = structuralManifestFixture();
    const [link] = fixture.links as Record<string, unknown>[];
    delete link.neutral_transform;

    expect(() => parseSceneUrdfManifest(fixture)).toThrow(
      'manifest.links[0].neutral_transform must be an object',
    );
  });

  it('rejects historical runtime-authority fields', () => {
    const fixture = structuralManifestFixture();
    fixture.authority = { commandable_joint_names: ['joint1'] };

    expect(() => parseSceneUrdfManifest(fixture)).toThrow(
      'manifest must be free of runtime authority and live-state fields',
    );
  });
});

describe('parseCatalogResponse', () => {
  it('accepts the v2 compiler-product identity and entry shape', () => {
    const parsed = parseCatalogResponse({
      apiVersion: 'manyforge.node_catalog.v2',
      contentVersion: '1.0.0',
      catalogId: 'manyforge_builtin',
      catalogDigest: 'sha256:catalog',
      environmentDigest: 'sha256:environment',
      entries: [
        {
          id: 'sequence',
          name: 'Sequence',
          category: 'Control Flow',
          description: 'Runs children in order.',
          stability: 'stable',
          kind: 'composite',
          childrenMin: 1,
          childrenMax: 64,
          parameters: [],
          requiredCapabilities: [],
          execution: {
            blocking: false,
            cancellable: false,
            ownerApi: 'orchestration.control_flow',
          },
          runners: { py_trees: 'sequence_v1' },
          tags: ['control-flow'],
          minProgramProfile: null,
          blackboard: { reads: [], writes: [] },
          ownerContext: null,
          outcomes: { success: 'all children succeeded' },
          eventFamily: null,
        },
      ],
    });

    expect(parsed.apiVersion).toBe('manyforge.node_catalog.v2');
    expect(parsed.entries[0]?.execution.ownerApi).toBe(
      'orchestration.control_flow',
    );
    expect(parsed.entries[0]?.runners).toEqual({ py_trees: 'sequence_v1' });
  });

  it('does not accept a historical catalog version', () => {
    expect(() => parseCatalogResponse({ apiVersion: 'v1' })).toThrow(
      'catalog.apiVersion must be the literal "manyforge.node_catalog.v2"',
    );
  });
});

describe('headless projection parsers', () => {
  it('accepts structural and headless Composer health without inventing state', () => {
    const structural = parseHealthResponse({
      status: 'degraded',
      mode: 'structural_only',
      components: { runtime: 'unavailable' },
      runtimeAvailability: {
        code: 'composable_cell_runtime_unavailable',
        message: 'Runtime is unavailable.',
        restorationGates: ['N', 'L0', 'P5', 'I', 'R', 'T'],
      },
      timestamp: 1,
    });
    const headless = parseHealthResponse({
      status: 'degraded',
      mode: 'headless_client',
      components: { runtime: 'degraded' },
      runtimeAvailability: null,
      headless: { schema_version: 'manyforge.headless_health.v1' },
      timestamp: 2,
    });

    expect(structural.runtimeAvailability?.restorationGates).toEqual([
      'N', 'L0', 'P5', 'I', 'R', 'T',
    ]);
    expect(structural.headless).toBeNull();
    expect(headless.mode).toBe('headless_client');
    expect(headless.runtimeAvailability).toBeNull();
  });

  it('accepts exact runtime and four-state alarm snapshots', () => {
    const runtime = parseHeadlessRuntimeSnapshot(runtimeSnapshotFixture());
    const alarms = parseOperatorAlarmSnapshot(alarmSnapshotFixture());

    expect(runtime.sections.logicalScene.sourceOwnerEpoch).toBe('source-epoch-1');
    expect(alarms.alarms[0]?.lifecycleState).toBe('active_unacknowledged');
    expect(alarms.unacknowledgedMembership[0]?.recordRevision).toBe(3);
  });

  it('rejects unsafe fence numbers and unknown alarm lifecycle values', () => {
    const runtime = runtimeSnapshotFixture();
    runtime.projection_generation = Number.MAX_SAFE_INTEGER + 1;
    expect(() => parseHeadlessRuntimeSnapshot(runtime)).toThrow(
      'runtime.projection_generation must be a non-negative safe integer',
    );

    const alarms = alarmSnapshotFixture();
    const [record] = alarms.alarms as Record<string, unknown>[];
    record.lifecycle_state = 'cleared';
    expect(() => parseOperatorAlarmSnapshot(alarms)).toThrow(
      'a supported four-state alarm lifecycle value',
    );
  });

  it('accepts the bounded historian page and validates event schema identity', () => {
    const page = parseHistorianPage({
      schema_version: 'manyforge.historian_page.v1',
      events: [{
        sequence: 7,
        event: {
          schema_version: 'manyforge.historian_event.v1',
          event_id: 'event-1',
          component: 'manyforge.alarm',
          event_type: 'operator_alarm_activated',
          severity: 'INFO',
          occurred_at_ns: 123,
          occurred_at_wall: '2026-08-05T12:00:00Z',
          payload: { condition_id: 'condition-1' },
        },
      }],
      next_before_sequence: null,
      truncated: false,
    });

    expect(page.events[0]?.event.eventType).toBe('operator_alarm_activated');
    expect(page.events[0]?.sequence).toBe(7);
  });
});

describe('addressed Robot Control parsers', () => {
  const sceneRevision = {
    scene_id: 'scene-1',
    revision_id: 3,
    content_hash: 'sha256:scene',
  };

  it('accepts source-local limits and exactly one matching observation', () => {
    const response = {
      ok: true,
      command: 'status',
      status: {
        owner_epoch: 'owner-1',
        graph_revision: 'sha256:graph',
        retained_bundle_id: 'sha256:bundle',
        scene_revision: sceneRevision,
        direct_control: {
          controllable_component_id: 'robot-a',
          command_target_id: 'arm',
          component_role_id: 'controlled_component',
          target_role_id: 'command_target',
          base_frame_role_id: 'base',
          tcp_frame_role_id: 'tcp',
          operation_capability_id: 'execution.trajectory.execute',
          planning_capability_ids: ['planning.motion.joint_goal'],
          frame_role_ids: ['base', 'tcp'],
          program_id: 'manyforge.direct_control.motion',
          program_revision: 'sha256:program',
          program_binding_id: 'sha256:binding',
          program_run_id: 'direct-control-owner-1',
          observed_state: {
            complete: true,
            positions_rad: [0.2],
            sequence: 7,
            source_epoch: 'source-1',
            source_joint_names: ['joint1'],
          },
          joints: [{
            source_joint_name: 'joint1',
            min_position_rad: -1,
            max_position_rad: 1,
            max_velocity_rad_s: 2,
            max_acceleration_rad_s2: null,
          }],
        },
        observed_targets: [],
      },
    };
    const status = parseExecutionOwnerStatus(response);

    expect(status.directControl.baseFrameRoleId).toBe('base');
    expect(status.observedMotion.sourceJointNames).toEqual(['joint1']);
    expect(status.observedMotion.positionsRad).toEqual([0.2]);

    const leaked = structuredClone(response);
    Object.assign(leaked.status.direct_control, {
      compiled_joint_names: ['robot-a__joint1'],
    });
    expect(() => parseExecutionOwnerStatus(leaked)).toThrow(/exactly/);
  });

  it('accepts only a no-motion receipt with equal source arrays', () => {
    const response = {
      ok: true,
      command: 'preview_motion',
      preview_receipt: {
        schema_version: 'manyforge.direct_control_motion_preview.v1',
        receipt_digest: 'sha256:receipt',
        intent_digest: 'sha256:intent',
        request_id: 'request-1',
        request_owner_epoch: 'owner-1',
        program_id: 'manyforge.direct_control.motion',
        program_revision: 'sha256:program',
        program_binding_id: 'sha256:binding',
        program_run_id: 'direct-control-owner-1',
        graph_revision: 'sha256:graph',
        retained_bundle_id: 'sha256:bundle',
        scene_revision: sceneRevision,
        component_role_id: 'controlled_component',
        target_role_id: 'command_target',
        required_capability_id: 'execution.trajectory.execute',
        source_joint_names: ['joint1'],
        start_positions_rad: [0],
        final_positions_rad: [0.4],
        final_tcp_pose: {
          position_m: { x_m: 0, y_m: 0, z_m: 0.4 },
          orientation: { x: 0, y: 0, z: 0, w: 1 },
        },
        collision_world_revision: {
          revision_id: 2,
          content_hash: 'collision-2',
        },
        commit_eligible: true,
      },
    };
    const receipt = parseMotionPreviewResponse(response);

    expect(receipt.finalPositionsRad).toEqual([0.4]);
    expect(receipt.commitEligible).toBe(true);

    const undeclared = structuredClone(response);
    Object.assign(undeclared.preview_receipt, { trajectory: [] });
    expect(() => parseMotionPreviewResponse(undeclared)).toThrow(
      /exactly/,
    );
  });
});
