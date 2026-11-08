import type { ProgramTargetReference, ProgramTreeNode } from '../types/api';

export interface ProgramSourceJointInventory {
  jointNames: string[];
  target: ProgramTargetReference | null;
  unavailableReason: string | null;
}

/** Exact target/joint facts supplied by one validated Draft candidate. */
export interface CandidateSourceJointFact {
  target: ProgramTargetReference;
  source_joint_names: readonly string[];
}

/** Use only facts from the currently selected, generation-checked Draft candidate. */
export function programSourceJointInventory(
  node: ProgramTreeNode,
  candidateTargets: readonly CandidateSourceJointFact[] | null,
  unavailableReason = 'Validate the current Deployment Draft to choose source joints.',
): ProgramSourceJointInventory {
  const raw = node.parameters.target;
  if (raw === null || typeof raw !== 'object' || Array.isArray(raw)
    || typeof raw.component_id !== 'string' || !raw.component_id
    || typeof raw.command_target_id !== 'string' || !raw.command_target_id) {
    return { jointNames: [], target: null, unavailableReason: 'Select a target before choosing source joints.' };
  }
  const target = { component_id: raw.component_id, command_target_id: raw.command_target_id };
  if (candidateTargets === null) return { jointNames: [], target, unavailableReason };
  const matched = candidateTargets.filter((row) => row.target.component_id === target.component_id
    && row.target.command_target_id === target.command_target_id);
  if (matched.length !== 1) {
    return { jointNames: [], target, unavailableReason: 'The selected target is absent or ambiguous in the Deployment Draft.' };
  }
  const jointNames = [...new Set(matched[0].source_joint_names.filter(Boolean))];
  return jointNames.length > 0 ? { jointNames, target, unavailableReason: null }
    : { jointNames: [], target, unavailableReason: 'The selected target has no commandable source joints.' };
}
