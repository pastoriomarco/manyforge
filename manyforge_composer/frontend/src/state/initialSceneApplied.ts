/**
 * Reload initial scene after an Apply or Revert.
 *
 * Program and Deployment Apply and Revert keep the live Scene (`preserve`):
 * a new initial scene reaches the cell only through a Reset that reloads it
 * (or Load, New). With **Reload initial scene** chosen in the confirmation,
 * one idle Reset with that option is sent once the Apply or Revert is Applied
 * (never before) and the cycle and authoring admission allow it. A Program
 * Apply or Revert is Applied when its target revision is the observed Applied
 * Program (and is not reloaded when that left the initial scene unchanged); a
 * Deployment Apply or Revert is Applied when its own Apply command ends
 * without error. An Apply or Revert that ends refused or failed drops its
 * expectation. While the Reset waits a notice says so; a run or another
 * command that comes first leaves it unsent, and a notice says that until
 * acknowledged. Pure: the hook (`useInitialSceneReload`) sends the Reset.
 */
import type { AppliedProgramSelection, ProgramCycleStatus, ProgramDocument } from '../types/api';
import { canonical } from '../utils/referenceValidation';

/** The confirmation's text for the Reload initial scene option; the Reset
 * confirmation uses the same. */
export const RELOAD_INITIAL_SCENE_NOTE = 'Scene objects, including carried ones, are replaced by the Program\'s initial Scene.';

/** The initial scene of a Program document, comparable by value. */
export function initialSceneKey(program: ProgramDocument | null | undefined): string {
  return canonical(program?.scene ?? []);
}

/**
 * Reload initial scene is offered with a Program Apply that changes the
 * Applied initial scene, with a Program Revert that changes the Applied
 * Program (the Saved Program's scene is not projected to the browser), and
 * with every Deployment Apply or Revert (a moved component can change where
 * objects placed relative to its frames start).
 */
export function initialSceneReloadOffered({
  action,
  target,
  draftProgram,
  appliedProgram,
  savedDiffersFromApplied,
}: {
  action: 'apply' | 'revert' | 'discard' | null;
  target: 'program' | 'deployment' | 'scenario' | null;
  draftProgram: ProgramDocument | null;
  appliedProgram: ProgramDocument | null;
  savedDiffersFromApplied: boolean;
}): boolean {
  if (target === 'deployment') return action === 'apply' || action === 'revert';
  if (target !== 'program' || appliedProgram === null) return false;
  if (action === 'apply') {
    return draftProgram !== null && initialSceneKey(draftProgram) !== initialSceneKey(appliedProgram);
  }
  return action === 'revert' && savedDiffersFromApplied;
}

/**
 * What the operator confirmed: a Program Apply or Revert to this revision, or
 * a Deployment Apply or Revert (settled by its Apply command's end).
 */
export type InitialSceneExpectation =
  | {
    target: 'program';
    targetProgramRevision: string;
    /** Reload initial scene was checked. */
    reload: boolean;
  }
  | { target: 'deployment'; reload: boolean };

interface AppliedScene {
  recordDigest: string;
  programRevision: string;
  scene: string;
}

export interface InitialSceneTracking {
  applied: AppliedScene | null;
  expectation: InitialSceneExpectation | null;
  /**
   * Waiting for Idle, admission and Reset to be allowed (cleared when sent),
   * or not sent because a run or another command came first (cleared when
   * acknowledged).
   */
  reload: { kind: 'waiting' } | { kind: 'not_sent' } | null;
}

export const INITIAL_SCENE_TRACKING: InitialSceneTracking = {
  applied: null, expectation: null, reload: null,
};

export function expectInitialScene(
  tracking: InitialSceneTracking,
  expectation: InitialSceneExpectation,
): InitialSceneTracking {
  return { ...tracking, expectation };
}

/** The confirmed Apply or Revert ended refused or failed: nothing to reload. */
export function dropInitialSceneExpectation(
  tracking: InitialSceneTracking,
  expectation: InitialSceneExpectation,
): InitialSceneTracking {
  return tracking.expectation === expectation ? { ...tracking, expectation: null } : tracking;
}

/** A Reset-then-Apply chain of this document stopped before its Apply. */
export function dropInitialSceneExpectationOf(
  tracking: InitialSceneTracking,
  target: InitialSceneExpectation['target'],
): InitialSceneTracking {
  return tracking.expectation?.target === target ? { ...tracking, expectation: null } : tracking;
}

/** The operator acknowledged the reload that was not sent. */
export function acknowledgeReloadNotSent(tracking: InitialSceneTracking): InitialSceneTracking {
  return tracking.reload?.kind === 'not_sent' ? { ...tracking, reload: null } : tracking;
}

/**
 * One Applied Program observation. `null` is ignored unless `noneObserved`
 * (the backend read succeeded and names no Applied Program): a read that
 * briefly fails right after an Apply proves nothing.
 */
export function observeAppliedScene(
  tracking: InitialSceneTracking,
  selection: AppliedProgramSelection | null,
  noneObserved: boolean,
): InitialSceneTracking {
  if (selection === null) {
    return noneObserved
      ? { ...tracking, applied: null, expectation: null, reload: null }
      : tracking;
  }
  const applied = {
    recordDigest: selection.recordDigest,
    programRevision: selection.programRevision,
    scene: initialSceneKey(selection.program),
  };
  const previous = tracking.applied;
  if (previous !== null && previous.recordDigest === applied.recordDigest) return tracking;
  const expectation = tracking.expectation;
  if (expectation?.target === 'deployment') {
    // Its Apply command's end settles it (`observeDeploymentApplyEnd`).
    return { ...tracking, applied };
  }
  if (expectation !== null && expectation.targetProgramRevision === applied.programRevision) {
    if (previous !== null && previous.scene === applied.scene) {
      return { ...tracking, applied, expectation: null };
    }
    return {
      ...tracking, applied, expectation: null,
      reload: expectation.reload ? { kind: 'waiting' } : null,
    };
  }
  // Another Applied Program (Load, New, another browser's Apply).
  return { ...tracking, applied, expectation: null };
}

/**
 * The end of a Deployment Apply or Revert command: once it ended without
 * error it is Applied, and the reload it was confirmed with waits for Idle.
 */
export function observeDeploymentApplyEnd(
  tracking: InitialSceneTracking,
  succeeded: boolean,
): InitialSceneTracking {
  if (tracking.expectation?.target !== 'deployment') return tracking;
  return {
    ...tracking, expectation: null,
    reload: succeeded && tracking.expectation.reload ? { kind: 'waiting' } : tracking.reload,
  };
}

/** One cycle observation: a run or another command came first. */
export function observeInitialSceneCycle(
  tracking: InitialSceneTracking,
  cycle: Pick<ProgramCycleStatus, 'state'> | null,
): InitialSceneTracking {
  if (cycle === null || tracking.reload?.kind !== 'waiting' || cycle.state === 'idle_after_reset') return tracking;
  return { ...tracking, reload: { kind: 'not_sent' } };
}

/** The waiting reload may be sent now. */
export function initialSceneReloadReady(
  tracking: InitialSceneTracking,
  facts: {
    cycle: Pick<ProgramCycleStatus, 'state' | 'programRunId'> | null;
    resetAllowed: boolean;
    runnerBusy: boolean;
    commandPending: boolean;
    authoringAllowed: boolean | null;
  },
): boolean {
  return tracking.reload?.kind === 'waiting'
    && facts.cycle?.state === 'idle_after_reset'
    && facts.cycle.programRunId === null
    && facts.resetAllowed && !facts.runnerBusy && !facts.commandPending
    && facts.authoringAllowed === true;
}
