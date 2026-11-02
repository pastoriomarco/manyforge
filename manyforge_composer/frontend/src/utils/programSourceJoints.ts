import type {
  AppliedProgramSelection,
  JsonValue,
  ProgramAuthoringDocument,
  ProgramTreeNode,
  SourceModelProjection,
} from '../types/api';

export interface ProgramSourceJointInventory {
  jointNames: string[];
  targetRole: string | null;
  unavailableReason: string | null;
}

function jsonValuesEqual(left: JsonValue | undefined, right: JsonValue | undefined): boolean {
  if (left === right) return true;
  if (Array.isArray(left) || Array.isArray(right)) {
    return Array.isArray(left)
      && Array.isArray(right)
      && left.length === right.length
      && left.every((value, index) => jsonValuesEqual(value, right[index]));
  }
  if (
    left === null
    || right === null
    || typeof left !== 'object'
    || typeof right !== 'object'
  ) return false;
  const leftKeys = Object.keys(left).sort();
  const rightKeys = Object.keys(right).sort();
  return leftKeys.length === rightKeys.length
    && leftKeys.every((key, index) => (
      key === rightKeys[index]
      && jsonValuesEqual(left[key], right[key])
    ));
}

function unavailable(
  reason: string,
  targetRole: string | null = null,
): ProgramSourceJointInventory {
  return { jointNames: [], targetRole, unavailableReason: reason };
}

/**
 * Join two read-only owner projections for editing assistance only:
 * the current Scenario binding maps the Program target role to one command
 * target, and the source-model projection supplies that target's commandable
 * source joints. Backend Draft validation and Program Binding remain the
 * authority for every submitted value.
 */
export function programSourceJointInventory(
  program: ProgramAuthoringDocument,
  node: ProgramTreeNode,
  applied: AppliedProgramSelection | null,
  sourceModel: SourceModelProjection | null,
  sourceModelUnavailableReason = 'The active source-model projection is unavailable.',
): ProgramSourceJointInventory {
  const targetRole = typeof node.parameters.target_role === 'string'
    ? node.parameters.target_role
    : null;
  if (targetRole === null || targetRole.trim() === '') {
    return unavailable('Select a target role before choosing source joints.');
  }
  if (sourceModel === null) return unavailable(sourceModelUnavailableReason, targetRole);
  if (applied === null || applied.program.program_id !== program.program_id) {
    return unavailable(
      'Joint choices require a current Scenario binding for this Program.',
      targetRole,
    );
  }
  if (
    applied.retainedBundleId !== sourceModel.retained_bundle_id
    || applied.graphRevision !== sourceModel.graph_revision
    || applied.programBinding.retained_bundle_id !== sourceModel.retained_bundle_id
    || applied.programBinding.graph_revision !== sourceModel.graph_revision
  ) {
    return unavailable(
      'The current Scenario binding and source-model projection do not match.',
      targetRole,
    );
  }
  const draftTargetRole = program.target_roles[targetRole];
  const appliedTargetRole = applied.program.target_roles[targetRole];
  if (
    draftTargetRole === undefined
    || appliedTargetRole === undefined
    || !jsonValuesEqual(draftTargetRole, appliedTargetRole)
  ) {
    return unavailable(
      'This target role differs from the current Scenario binding; apply or load a compatible Scenario before changing its joints.',
      targetRole,
    );
  }
  const roleBinding = applied.programBinding.target_role_bindings.find(
    (binding) => binding.target_role_id === targetRole,
  );
  if (roleBinding === undefined) {
    return unavailable(
      `The current Scenario does not bind target role '${targetRole}'.`,
      targetRole,
    );
  }
  const target = sourceModel.targets.find(
    (candidate) => candidate.component_id === roleBinding.controllable_component_id
      && candidate.target_id === roleBinding.command_target_id,
  );
  if (target === undefined) {
    return unavailable(
      `The bound target for '${targetRole}' is absent from the source-model projection.`,
      targetRole,
    );
  }
  const jointNames: string[] = [];
  const seen = new Set<string>();
  for (const group of target.model_groups) {
    for (const jointName of group.commandable_source_joint_names) {
      if (jointName && !seen.has(jointName)) {
        seen.add(jointName);
        jointNames.push(jointName);
      }
    }
  }
  return jointNames.length > 0
    ? { jointNames, targetRole, unavailableReason: null }
    : unavailable(
      `Target role '${targetRole}' has no commandable source joints.`,
      targetRole,
    );
}
