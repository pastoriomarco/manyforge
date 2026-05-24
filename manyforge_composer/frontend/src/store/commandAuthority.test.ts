import { describe, expect, it } from 'vitest';

import {
  ROBOT_COMMAND_AUTHORITY_REASONS,
  evaluateRobotCommandAuthority,
  type DraftAuthorityStatus,
  type RobotCommandAuthorityInputs,
} from './commandAuthority';

const CLEAN_DRAFT: DraftAuthorityStatus = {
  hasUnappliedChanges: false,
  domains: [],
  reason: null,
};

function evaluate(
  overrides: Partial<RobotCommandAuthorityInputs> = {},
) {
  return evaluateRobotCommandAuthority({
    cycleState: 'idle',
    followTargetPhase: 'idle',
    draftAuthority: CLEAN_DRAFT,
    sceneError: null,
    lastUpdatedAtMs: 1_000,
    nowMs: 1_100,
    ...overrides,
  });
}

describe('evaluateRobotCommandAuthority', () => {
  it('blocks robot commands when editor drafts have not been applied', () => {
    const result = evaluate({
      draftAuthority: {
        hasUnappliedChanges: true,
        domains: ['program graph', 'deployment'],
        reason: ROBOT_COMMAND_AUTHORITY_REASONS.applyDraftChangesFirst([
          'program graph',
          'deployment',
        ]),
      },
    });

    expect(result.canCommand).toBe(false);
    expect(result.reason).toBe('apply draft changes first: program graph, deployment');
    expect(result.unappliedDraftDomains).toEqual(['program graph', 'deployment']);
  });

  it('blocks robot commands when scene state is stale', () => {
    const result = evaluate({
      lastUpdatedAtMs: 1_000,
      nowMs: 3_001,
      sceneStaleAfterMs: 2_000,
    });

    expect(result.canCommand).toBe(false);
    expect(result.reason).toBe(ROBOT_COMMAND_AUTHORITY_REASONS.sceneStateStale);
  });

  it('blocks robot commands while the cycle owns the robot', () => {
    expect(evaluate({ cycleState: 'running' }).reason).toBe(
      ROBOT_COMMAND_AUTHORITY_REASONS.cycleOwnsRobot,
    );
    expect(evaluate({ cycleState: 'direct_control' }).reason).toBe(
      ROBOT_COMMAND_AUTHORITY_REASONS.cycleOwnsRobot,
    );
  });

  it('blocks new robot commands while follow-target streaming owns the robot', () => {
    const result = evaluate({
      followTargetPhase: 'streaming',
    });

    expect(result.canCommand).toBe(false);
    expect(result.reason).toBe(ROBOT_COMMAND_AUTHORITY_REASONS.streamingOwnsRobot);
  });

  it('can keep the active follow-target surface enabled during its own stream', () => {
    const result = evaluate({
      cycleState: 'direct_control',
      followTargetPhase: 'streaming',
      options: { allowFollowTargetActive: true },
    });

    expect(result.canCommand).toBe(true);
    expect(result.reason).toBeNull();
  });

  it('uses user-facing reason labels for scene readiness failures', () => {
    expect(evaluate({ lastUpdatedAtMs: null }).reason).toBe('waiting for scene state');
    expect(evaluate({ sceneError: 'planner offline' }).reason).toBe(
      'scene unavailable: planner offline',
    );
  });
});
