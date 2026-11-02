/** Shared held-run fixtures: a run paused on a failed motion node. */
import type {
  CatalogEntry,
  CycleRecoveryOffer,
  ProgramAuthoringDocument,
  ProgramDocument,
  ProgramRunStatus,
} from '../types/api';

const digest = (fill: string) => `sha256:${fill.repeat(64)}`;

export function heldProgram(): ProgramDocument {
  return {
    api_version: 'manyforge.program.v2',
    program_id: 'pick',
    program_revision: digest('a'),
    program_profile: 'composable-cell-v1',
    name: 'Pick',
    catalog: { api_version: 'manyforge.node_catalog.v2', content_version: 'fixture' },
    component_roles: {},
    target_roles: {},
    parameters: [{
      parameter_id: 'approach_height', type: 'float64', default: 0.1,
    }],
    blackboard: {
      namespaces: [],
      keys: [
        { key: 'pick_pose', type: 'stored_pose', access: 'read_write', persistence: 'run' },
        { key: 'layer', type: 'int64', access: 'read_write', persistence: 'persistent', initial: 0 },
        { key: 'offset', type: 'float64', access: 'read', persistence: 'run', expression: 'param.approach_height * 2' },
      ],
    },
    tree: {
      node_id: 'control.sequence',
      instance_id: 'root',
      parameters: {},
      children: [
        {
          node_id: 'motion.move_to_pose',
          instance_id: 'move',
          parameters: { target_role: 'arm', goal: { position_m: [0.3, 0, 0.9] }, speed: 0.2 },
          value_sources: {
            approach: { kind: 'program_parameter', reference: 'approach_height' },
            layer: { kind: 'blackboard_key', reference: 'layer' },
            clearance: { kind: 'expression', expression: 'key.offset + param.approach_height' },
          },
          children: [],
        },
        {
          node_id: 'timer.wait',
          instance_id: 'settle',
          parameters: {},
          value_sources: {
            duration_s: { kind: 'program_parameter', reference: 'approach_height' },
          },
          children: [],
        },
      ],
    },
  };
}

export function heldAuthoring(program = heldProgram()): ProgramAuthoringDocument {
  const { program_revision: _revision, tree, ...rest } = program;
  void _revision;
  return {
    ...rest,
    schema_version: 'manyforge.composer_authoring_forest.v1',
    forest: [structuredClone(tree)],
    positions: {},
  };
}

export const heldCatalog: CatalogEntry[] = [{
  id: 'motion.move_to_pose',
  parameters: ['target_role', 'goal', 'speed', 'approach', 'layer', 'clearance'].map((id) => ({
    id, required: false, schema: { type: id === 'goal' ? 'object' : 'number' },
    schemaHash: 'x', hasDefault: false,
  })),
} as unknown as CatalogEntry];

export function heldOffer(patch: Partial<CycleRecoveryOffer> = {}): CycleRecoveryOffer {
  return {
    schemaVersion: 'manyforge.cycle_recovery_offer.v5',
    offerId: digest('e'),
    offerRevision: 1,
    programRunId: 'run.1',
    programBindingId: digest('c'),
    nodeInstanceId: 'move',
    nodeAttemptId: 'move.attempt.1',
    recoveryClass: 'node_failure_pause',
    actions: [],
    reason: {
      code: 'ik_failed',
      message: 'No IK solution for goal',
      details: { reason: 'native_ik_no_solution', path: 'goal.position_m[2]' },
      origin: null,
    },
    policy: { allowIntervention: true, maxRetries: 0 },
    retriesUsed: 0,
    ...patch,
  } as unknown as CycleRecoveryOffer;
}

export function heldRun(): ProgramRunStatus {
  return {
    ownerEpoch: 'runner.epoch.1',
    runRevision: 4,
    state: 'intervention_required',
    capture: {
      programRunId: 'run.1',
      programId: 'pick',
      programRevision: digest('a'),
      programBindingId: digest('c'),
    },
  } as unknown as ProgramRunStatus;
}

