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
    draftAuthority: CLEAN_DRAFT,
    sceneError: null,
    sceneConnectionState: 'connected',
    frameAvailable: true,
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

  it('blocks robot commands only on explicit stale or disconnected authority', () => {
    const result = evaluate({
      sceneConnectionState: 'stale',
    });

    expect(result.canCommand).toBe(false);
    expect(result.reason).toBe(ROBOT_COMMAND_AUTHORITY_REASONS.sceneStateStale);
    expect(evaluate({ sceneConnectionState: 'disconnected' }).reason).toBe(
      ROBOT_COMMAND_AUTHORITY_REASONS.sceneStateStale,
    );
  });

  it('blocks robot commands while the cycle owns the robot', () => {
    expect(evaluate({ cycleState: 'running' }).reason).toBe(
      ROBOT_COMMAND_AUTHORITY_REASONS.cycleOwnsRobot,
    );
    expect(evaluate({ cycleState: 'stopping' }).reason).toBe(
      ROBOT_COMMAND_AUTHORITY_REASONS.cycleOwnsRobot,
    );
    expect(evaluate({ cycleState: 'direct_control' }).reason).toBe(
      ROBOT_COMMAND_AUTHORITY_REASONS.cycleOwnsRobot,
    );
  });

  it('uses user-facing reason labels for scene readiness failures', () => {
    expect(evaluate({ frameAvailable: false }).reason).toBe('waiting for scene state');
    expect(evaluate({ sceneError: 'planner offline' }).reason).toBe(
      'scene unavailable: planner offline',
    );
  });
});
