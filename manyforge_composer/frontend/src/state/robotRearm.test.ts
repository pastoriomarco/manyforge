import { describe, expect, it, vi } from 'vitest';
import type { CCLCComponentSnapshot } from '../types/cclc';
import type { CCLCAdmission } from '../types/cclc';
import {
  ComponentRearmRequiredError,
  MotionIntentGate,
  RearmAcknowledgedIncidentError,
  RearmOutcomeUnknownError,
  narrowRearm,
  type NarrowRearmPorts,
  REARM_CANCELLED,
  REARM_CANCELLED_NOTHING_STARTED,
  REARM_WAIT_MS,
  REARMED_PREVIEW_AGAIN,
  narrowRearmRefusal,
  rearmRequiredFromResult,
  sendWithRearm,
  type RobotRearmPorts,
} from './robotRearm';

function snapshot(state: string, reason: string | null, incident: unknown = null): CCLCComponentSnapshot {
  return {
    controllableComponentId: 'arm',
    lifecycleState: state,
    lifecycleProgress: {
      phase: 'satisfied',
      nextIntent: null,
      blockingCauseClass: reason === null ? 'none' : 'availability',
      blockingReason: reason,
      waitingSinceNs: null,
      delayed: false,
      incident,
    },
  } as unknown as CCLCComponentSnapshot;
}

const MOVED = 'moved_externally:rearm_at_next_motion';

function ports(
  snapshots: CCLCComponentSnapshot[],
  options: { refuse?: string; onSleep?: () => void } = {},
): RobotRearmPorts & { statuses: Array<string | null>; rearmed: string[] } {
  let now = 0;
  let read = 0;
  const statuses: Array<string | null> = [];
  const rearmed: string[] = [];
  return {
    statuses,
    rearmed,
    rearmComponent: async (id) => {
      if (options.refuse !== undefined) throw new Error(options.refuse);
      rearmed.push(id);
    },
    component: async () => snapshots[Math.min(read++, snapshots.length - 1)]!,
    sleep: async (ms) => { now += ms; options.onSleep?.(); },
    now: () => now,
    onStatus: (status) => { statuses.push(status); },
  };
}

const refusal = (inMotion = '') => rearmRequiredFromResult(
  'component_rearm_required',
  { ok: false, error: { code: 'component_rearm_required', message: 'm',
    details: inMotion === '' ? { components: 'arm' } : { components: 'arm', in_motion: inMotion } } },
  'The robot was moved outside ManyForge.',
)!;

const ARMING = [snapshot('disabled', null), snapshot('active', MOVED), snapshot('active', null)];
const always = () => true;

describe('one press after an external move', () => {
  it('Run: refused, re-arms only the moved robot, waits until active, re-sends once', async () => {
    const send = vi.fn().mockRejectedValueOnce(refusal()).mockResolvedValueOnce('received');
    const rearm = ports(ARMING);
    await expect(sendWithRearm('program_run', send, rearm, always)).resolves.toBe('received');
    expect(send).toHaveBeenCalledTimes(2);
    expect(rearm.rearmed).toEqual(['arm']);
    expect(rearm.statuses).toEqual(['Re-arming robot…', null]);
  });

  it('Robot Control operation: re-arms, then asks for a new preview; never re-sent', async () => {
    const send = vi.fn().mockRejectedValueOnce(refusal());
    const rearm = ports(ARMING);
    await expect(sendWithRearm('operation_start', send, rearm, always))
      .rejects.toThrow(REARMED_PREVIEW_AGAIN);
    expect(send).toHaveBeenCalledTimes(1);
    expect(rearm.rearmed).toEqual(['arm']);
  });

  it('a Stop during the re-arm wait cancels the pending resend', async () => {
    let generation = 1;
    const send = vi.fn().mockRejectedValueOnce(refusal());
    // The operator presses Stop while Composer polls for the re-armed robot:
    // the Recover was sent, so the message does not claim none started.
    const rearm = ports(ARMING, { onSleep: () => { generation += 1; } });
    const result = sendWithRearm('program_run', send, rearm, () => generation === 1);
    await expect(result).rejects.toThrow(REARM_CANCELLED);
    await expect(result).rejects.not.toThrow('No re-arm was started');
    expect(send).toHaveBeenCalledTimes(1);
    expect(rearm.statuses).toEqual(['Re-arming robot…', null]);
  });

  it('a newer motion command supersedes the older one waiting for its re-arm', async () => {
    let generation = 0;
    const issue = () => { generation += 1; const mine = generation; return () => generation === mine; };
    const olderSend = vi.fn().mockRejectedValueOnce(refusal());
    const newerSend = vi.fn().mockResolvedValue('received');
    const olderCurrent = issue();
    let newer: Promise<unknown> | null = null;
    const rearm = ports(ARMING, {
      onSleep: () => { newer ??= sendWithRearm('program_run', newerSend, ports([]), issue()); },
    });
    await expect(sendWithRearm('program_run', olderSend, rearm, olderCurrent))
      .rejects.toThrow(REARM_CANCELLED);
    await expect(newer).resolves.toBe('received');
    expect(olderSend).toHaveBeenCalledTimes(1);
    expect(newerSend).toHaveBeenCalledTimes(1);
  });

  it('a refused narrow re-arm stops with its reason; nothing is re-sent', async () => {
    const send = vi.fn().mockRejectedValueOnce(refusal());
    const rearm = ports([], { refuse: 'The robot was not re-armed: arm has an open incident; use Recover cell.' });
    await expect(sendWithRearm('program_run', send, rearm, always)).rejects.toThrow('use Recover cell');
    expect(send).toHaveBeenCalledTimes(1);
  });

  it('stops when the robot is not active within the bounded wait', async () => {
    const send = vi.fn().mockRejectedValueOnce(refusal());
    const stuck = ports([snapshot('disabled', null)]);
    await expect(sendWithRearm('program_run', send, stuck, always))
      .rejects.toThrow(`within ${REARM_WAIT_MS / 1000} s`);
    expect(send).toHaveBeenCalledTimes(1);
  });

  it('a robot still moving under its own controller is not re-armed', async () => {
    const send = vi.fn().mockRejectedValueOnce(refusal('arm'));
    const rearm = ports([]);
    await expect(sendWithRearm('program_run', send, rearm, always)).rejects.toThrow('still moving');
    expect(rearm.rearmed).toEqual([]);
    expect(send).toHaveBeenCalledTimes(1);
  });

  it('other refusals and other command kinds pass through unchanged', async () => {
    const other = new Error('stale');
    await expect(sendWithRearm('program_run', vi.fn().mockRejectedValue(other), ports([]), always))
      .rejects.toBe(other);
    const notMotion = refusal();
    await expect(sendWithRearm('operation_stop', vi.fn().mockRejectedValue(notMotion), ports([]), always))
      .rejects.toBe(notMotion);
    expect(rearmRequiredFromResult('stale', null, 'x')).toBeNull();
    expect(refusal()).toBeInstanceOf(ComponentRearmRequiredError);
  });
});

describe('the narrow re-arm refuses anything else', () => {
  it('re-arms only a moved robot on an admitting cell', () => {
    expect(narrowRearmRefusal(snapshot('active', MOVED), true)).toBeNull();
    expect(narrowRearmRefusal(snapshot('active', MOVED, { incidentId: 'i' }), true)).toContain('open incident');
    expect(narrowRearmRefusal(snapshot('faulted', 'intervention_required:fault'), true)).toContain('Recover cell');
    expect(narrowRearmRefusal(snapshot('active', 'moved_externally:in_motion'), true)).toContain('still moving');
    expect(narrowRearmRefusal(snapshot('active', MOVED), false)).toContain('Recover cell');
  });
});

describe('the pending re-arm intent (Cancel, Stop, Full Stop, Close)', () => {
  it('the notice Cancel during the wait cancels the resend', async () => {
    const gate = new MotionIntentGate();
    const send = vi.fn().mockRejectedValueOnce(refusal());
    const rearm = ports(ARMING, { onSleep: () => gate.cancel() });
    await expect(sendWithRearm('program_run', send, rearm, gate.issue('program_run')))
      .rejects.toThrow(REARM_CANCELLED);
    expect(send).toHaveBeenCalledTimes(1);
  });

  it.each(['operation_stop', 'program_motion_stop', 'program_full_stop', 'cell_full_stop',
    'operation_reset', 'program_reset'])('%s pressed during the wait cancels the resend', async (kind) => {
    const gate = new MotionIntentGate();
    const send = vi.fn().mockRejectedValueOnce(refusal());
    // The Stop is issued (and sent) at once; only the waiting Run is cancelled.
    const rearm = ports(ARMING, { onSleep: () => { gate.issue(kind); } });
    await expect(sendWithRearm('program_run', send, rearm, gate.issue('program_run')))
      .rejects.toThrow(REARM_CANCELLED);
    expect(send).toHaveBeenCalledTimes(1);
  });

  it('a runtime scale change does not cancel; Close (disabled source) does', () => {
    const gate = new MotionIntentGate();
    const current = gate.issue('program_run');
    gate.issue('runtime_scale');
    expect(current()).toBe(true);
    gate.setEnabled(false);
    expect(current()).toBe(false);
  });
});

describe('the narrow re-arm is checked again at its Recover (SX-487, SX-488)', () => {
  const accepted: CCLCAdmission = {
    replayStatus: 'accepted_new', requestId: 'r', transactionId: 'lifecycle.1', status: null,
    incidentRecovery: null,
  };

  function narrow(
    result: CCLCAdmission | (() => Promise<CCLCAdmission>),
    onRead: (read: 'identity' | 'owner' | 'component') => void = () => {},
  ) {
    const submitted: string[] = [];
    const ports: NarrowRearmPorts = {
      identity: async () => { onRead('identity'); return {}; },
      owner: async () => { onRead('owner'); return {}; },
      component: async () => { onRead('component'); return snapshot('active', MOVED); },
      admission: () => ({ authoringAllowed: true, admissionGeneration: 9 }),
      submit: async (current) => {
        submitted.push(current.controllableComponentId);
        return typeof result === 'function' ? result() : result;
      },
    };
    return { ports, submitted };
  }

  it('an incident acknowledged by the re-arm Recover stops the sequence; nothing is re-sent', async () => {
    const { ports: owner, submitted } = narrow({
      ...accepted, transactionId: null,
      incidentRecovery: {
        incidentId: 'incident.arm.2', controllableComponentId: 'arm', causeClass: 'emergency_stop',
        acknowledgedBy: 'composer', acknowledgedAtNs: 1 as never,
      },
    });
    const rearm = ports(ARMING);
    rearm.rearmComponent = (id, isCurrent) => narrowRearm(owner, id, isCurrent);
    const send = vi.fn().mockRejectedValueOnce(refusal());
    const result = sendWithRearm('program_run', send, rearm, always);
    await expect(result).rejects.toBeInstanceOf(RearmAcknowledgedIncidentError);
    await expect(result).rejects.toThrow(
      'An incident (emergency_stop) occurred during the re-arm and was acknowledged. '
      + 'Check the robot, then press Run again.',
    );
    expect(submitted).toEqual(['arm']);
    expect(send).toHaveBeenCalledTimes(1);
  });

  it('a stream latch acknowledged by the re-arm Recover stops too', async () => {
    const { ports: owner } = narrow({
      ...accepted, transactionId: null,
      streamAcknowledgement: { controllableComponentId: 'arm', acknowledgedAtNs: 1 as never },
    });
    await expect(narrowRearm(owner, 'arm', always)).rejects.toThrow('stream condition');
  });

  it('a lost Recover reply is an unknown outcome, never a known refusal; nothing is re-sent', async () => {
    const { ports: owner, submitted } = narrow(async () => { throw new Error('network timeout'); });
    const rearm = ports(ARMING);
    rearm.rearmComponent = (id, isCurrent) => narrowRearm(owner, id, isCurrent);
    const send = vi.fn().mockRejectedValueOnce(refusal());
    const result = sendWithRearm('program_run', send, rearm, always);
    await expect(result).rejects.toBeInstanceOf(RearmOutcomeUnknownError);
    await expect(result).rejects.toThrow(
      'The re-arm outcome is unknown: the robot may have been recovered and an incident '
      + "may have been acknowledged. Check the robot's lifecycle state and incidents "
      + '(Components) before running again.',
    );
    expect(submitted).toEqual(['arm']);
    expect(send).toHaveBeenCalledTimes(1);
  });

  it.each(['identity', 'owner', 'component'] as const)(
    'Cancel while the %s read is pending sends no Recover',
    async (pending) => {
      const gate = new MotionIntentGate();
      const isCurrent = gate.issue('program_run');
      const { ports: owner, submitted } = narrow(accepted, (read) => {
        if (read === pending) gate.cancel();
      });
      await expect(narrowRearm(owner, 'arm', isCurrent))
        .rejects.toThrow(REARM_CANCELLED_NOTHING_STARTED);
      expect(submitted).toEqual([]);
    },
  );

  it('an accepted Recover re-arms', async () => {
    const { ports: owner, submitted } = narrow(accepted);
    await expect(narrowRearm(owner, 'arm', always)).resolves.toBeUndefined();
    expect(submitted).toEqual(['arm']);
  });
});

describe('cancel wording (SX-489)', () => {
  it('says no re-arm started only when no Recover was submitted', async () => {
    const send = vi.fn().mockRejectedValueOnce(refusal());
    const rearm = ports(ARMING);
    const result = sendWithRearm('program_run', send, rearm, () => false);
    await expect(result).rejects.toThrow(REARM_CANCELLED_NOTHING_STARTED);
    expect(rearm.rearmed).toEqual([]);
    expect(REARM_CANCELLED).toBe('Cancelled: the waiting command was not sent.');
  });

  it('a cancel after a first robot was re-armed does not claim none started', async () => {
    let calls = 0;
    const gate = new MotionIntentGate();
    const isCurrent = gate.issue('program_run');
    const rearm = ports(ARMING);
    rearm.rearmComponent = async (_id, current) => {
      calls += 1;
      if (calls === 2) { gate.cancel(); if (!current()) throw new Error(REARM_CANCELLED_NOTHING_STARTED); }
    };
    const send = vi.fn().mockRejectedValueOnce(rearmRequiredFromResult(
      'component_rearm_required',
      { ok: false, error: { code: 'component_rearm_required', message: 'm',
        details: { components: 'arm,other' } } },
      'moved',
    ));
    const result = sendWithRearm('program_run', send, rearm, isCurrent);
    await expect(result).rejects.toThrow(REARM_CANCELLED);
    await expect(result).rejects.not.toThrow('No re-arm was started');
  });
});
