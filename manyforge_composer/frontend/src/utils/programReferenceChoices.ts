import type {
  ProgramAuthoringDocument,
  ProgramCollisionBodyReference,
  ProgramDocument,
  ProgramFrameReference,
} from '../types/api';
import type { FrameScope } from './programFrames';
import type { CandidateTarget, ReferenceSelectorFacts } from './referenceValidation';

/** Direct component-feature facts projected for selector consumers. */
export interface ProgramFeatureReferenceChoice<T> {
  key: string;
  reference: T;
  ambiguous: boolean;
  /** Null means no target-specific qualification was requested. */
  movingTargetMatches: boolean | null;
  selectable: boolean;
}

function sameTarget(left: CandidateTarget, right: CandidateTarget): boolean {
  return left.component_id === right.component_id
    && left.command_target_id === right.command_target_id;
}

function movingTargetMatch(
  movingTargets: readonly CandidateTarget[],
  movingTarget: CandidateTarget | null,
): boolean | null {
  return movingTarget === null
    ? null
    : movingTargets.some((target) => sameTarget(target, movingTarget));
}

export function programFrameReferenceChoices(
  selectorFacts: Pick<ReferenceSelectorFacts, 'frames'> | null,
  movingTarget: CandidateTarget | null = null,
): ProgramFeatureReferenceChoice<ProgramFrameReference>[] {
  return (selectorFacts?.frames ?? []).map((row) => {
    const reference = { ...row.reference };
    const matches = movingTargetMatch(row.moving_targets, movingTarget);
    return {
      key: JSON.stringify([reference.component_id, reference.frame_id]),
      reference,
      ambiguous: row.ambiguous,
      movingTargetMatches: matches,
      selectable: !row.ambiguous && (matches === null || matches),
    };
  });
}

export function programCollisionBodyReferenceChoices(
  selectorFacts: Pick<ReferenceSelectorFacts, 'bodies'> | null,
  movingTarget: CandidateTarget | null = null,
): ProgramFeatureReferenceChoice<ProgramCollisionBodyReference>[] {
  return (selectorFacts?.bodies ?? []).map((row) => {
    const reference = { ...row.reference };
    const matches = movingTargetMatch(row.moving_targets, movingTarget);
    return {
      key: JSON.stringify([reference.component_id, reference.collision_body_id]),
      reference,
      ambiguous: row.ambiguous,
      movingTargetMatches: matches,
      selectable: !row.ambiguous && (matches === null || matches),
    };
  });
}

/** Shared FrameSelector scope built only from the captured selector facts. */
export function programFrameScopeFromSelectorFacts(
  program: Pick<ProgramDocument, 'scene'>,
  selectorFacts: Pick<ReferenceSelectorFacts, 'frames' | 'bodies'> | null,
  movingTarget: CandidateTarget | null = null,
  requireMovingTarget = false,
): FrameScope {
  const frames = programFrameReferenceChoices(selectorFacts, movingTarget);
  const availableFrames = frames.filter((choice) => (
    !choice.ambiguous && (!requireMovingTarget || choice.movingTargetMatches === true)
  ));
  return {
    program,
    frames: availableFrames.map((choice) => choice.reference),
    movingFrames: frames.filter((choice) => (
      !choice.ambiguous && choice.movingTargetMatches === true
    )).map((choice) => choice.reference),
    ...(movingTarget === null ? {} : { movingComponentId: movingTarget.component_id }),
    unavailableFrames: frames.filter((choice) => !availableFrames.includes(choice)).map((choice) => ({
      reference: choice.reference,
      reason: choice.ambiguous
        ? 'Ambiguous frame identity'
        : 'Frame is not qualified for the selected target',
    })),
    bodies: programCollisionBodyReferenceChoices(selectorFacts).filter((choice) => !choice.ambiguous)
      .map((choice) => choice.reference),
  };
}

/** Scene IDs remain direct resource identities; callers retain their own ordering. */
export function programSceneResourceIds(
  program: Pick<ProgramAuthoringDocument, 'scene'>,
): string[] {
  return (program.scene ?? []).map((resource) => resource.scene_resource_id);
}

/** Contact-policy runtime objects are only Scene-resource blackboard keys. */
export function programSceneResourceBlackboardKeys(
  program: Pick<ProgramAuthoringDocument, 'blackboard'>,
): string[] {
  return (program.blackboard?.keys ?? [])
    .filter((row) => row.type === 'scene_resource')
    .map((row) => row.key)
    .sort();
}
