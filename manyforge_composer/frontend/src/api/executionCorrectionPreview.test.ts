import { describe, expect, it } from 'vitest';
import { parseProgramNodeCorrectionPreview } from './parsers/execution';
import type { ProgramNodeCorrectionPreviewRequest } from '../types/api';

const digest = `sha256:${'a'.repeat(64)}`;
const request: ProgramNodeCorrectionPreviewRequest = {
  schema_version: 'manyforge.program_node_correction_preview_request.v1',
  request_id: 'request.1', program_run_id: 'run.1', expected_owner_epoch: 'runner.1',
  program_id: 'program', program_revision: digest, program_binding_id: digest,
  node_instance_id: 'node', node_attempt_id: 'attempt',
  proposed_inputs: [{ path: 'goal', value: [] }],
};
const target = { component_id: 'robot', command_target_id: 'arm' };
const response = () => ({
  schema_version: 'manyforge.program_node_correction_preview.v2',
  request_id: request.request_id, program_run_id: request.program_run_id,
  program_id: request.program_id, program_revision: digest, program_binding_id: digest,
  node_instance_id: request.node_instance_id, node_attempt_id: request.node_attempt_id,
  program_runner_owner_epoch: request.expected_owner_epoch, run_revision: 1,
  retained_bundle_id: digest, graph_revision: digest, node_id: 'motion.move',
  proposed_paths: ['goal'], parameters_digest: digest, target,
  unavailable_reason: null, preview_check: 'trajectory_planned_and_collision_validated',
  preview: { ok: true, command: 'preview_motion', preview_receipt: {
    schema_version: 'manyforge.direct_control_motion_preview.v4', target,
    resolved_tcp: {
      entry: { target, frame: { component_id: 'robot', frame_id: 'tcp' } },
      source: 'active_tcp', owner_epoch: 'kernel.1', registration_generation: 1, sequence: 0,
    },
    receipt_digest: digest, intent_digest: digest, request_id: request.request_id,
    request_owner_epoch: 'kernel.1', program_id: request.program_id,
    program_revision: digest, program_binding_id: digest, program_run_id: request.program_run_id,
    graph_revision: digest, retained_bundle_id: digest,
    scene_revision: { scene_id: digest, revision_id: 1, content_hash: digest },
    required_capability_id: 'execution.trajectory.execute', source_joint_names: ['joint'],
    start_positions_rad: [0], final_positions_rad: [0],
    final_tcp_pose: { position_m: { x_m: 0, y_m: 0, z_m: 0 }, orientation: { x: 0, y: 0, z: 0, w: 1 } },
    collision_world_revision: { revision_id: 1, content_hash: digest }, commit_eligible: true,
    path_preview: { source_joint_names: ['joint'], pairs: [], samples: [
      { time_from_start_s: 0, positions_rad: [0], contacts: [], status: 'checked' },
    ] },
  } },
});

describe('correction preview direct identity', () => {
  it('accepts v2 and correlates the v4 native target and resolved TCP', () => {
    expect(parseProgramNodeCorrectionPreview(response(), request).target).toEqual({
      componentId: 'robot', commandTargetId: 'arm',
    });
    const crossed = response();
    crossed.preview.preview_receipt.target = { component_id: 'tool', command_target_id: 'arm' };
    expect(() => parseProgramNodeCorrectionPreview(crossed, request)).toThrow();
    const retired = response();
    retired.schema_version = 'manyforge.program_node_correction_preview.v1';
    expect(() => parseProgramNodeCorrectionPreview(retired, request)).toThrow();
    const stale = response();
    stale.preview.preview_receipt.graph_revision = `sha256:${'b'.repeat(64)}`;
    expect(() => parseProgramNodeCorrectionPreview(stale, request)).toThrow();
    const oldReceipt = response();
    oldReceipt.preview.preview_receipt.schema_version = 'manyforge.direct_control_motion_preview.v3';
    expect(() => parseProgramNodeCorrectionPreview(oldReceipt, request)).toThrow();
  });
});
