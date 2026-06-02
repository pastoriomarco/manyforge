import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { ApiError } from '../api/client';
import type { CandidateResponse } from '../types/api';
import { useRobotControlCandidateStore } from './robotControlCandidateStore';

// Mock the API client: keep the real ApiError (the store uses `instanceof`),
// replace the network surface with vitest mocks.
vi.mock('../api/client', async (importActual) => {
  const actual = await importActual<typeof import('../api/client')>();
  return {
    ...actual,
    api: {
      cycle: {
        status: vi.fn(),
      },
      robotControl: {
        candidates: {
          create: vi.fn(),
          get: vi.fn(),
          execute: vi.fn(),
          revert: vi.fn(),
        },
      },
    },
  };
});

// Import the (mocked) api after vi.mock is registered.
import { api } from '../api/client';

const cycle = api.cycle as unknown as {
  status: ReturnType<typeof vi.fn>;
};

const candidates = api.robotControl.candidates as unknown as {
  create: ReturnType<typeof vi.fn>;
  get: ReturnType<typeof vi.fn>;
  execute: ReturnType<typeof vi.fn>;
  revert: ReturnType<typeof vi.fn>;
};

function makeCandidate(
  overrides: Partial<CandidateResponse> = {},
): CandidateResponse {
  return {
    candidateId: 'cand-1',
    generation: 1,
    sourceSurface: 'joint_slider',
    inputMode: 'joint',
    status: 'ready',
    commitEligible: true,
    diagnostics: { reasonCode: 'ready', summary: 'ok' },
    ghost: { blocked: false, finalJointNames: ['j1'], finalJointPositionsRad: [0.3] },
    maxAgeNs: 30_000_000_000,
    expiresAtNs: 0,
    createdAtNs: 0,
    updatedAtNs: 0,
    ...overrides,
  };
}

const JOINT_REQUEST = {
  sourceSurface: 'joint_slider' as const,
  inputMode: 'joint' as const,
  jointTarget: { jointNames: ['j1'], positionsRad: [0.3] },
};

describe('robotControlCandidateStore', () => {
  beforeEach(() => {
    vi.useFakeTimers();
    candidates.create.mockReset();
    candidates.get.mockReset();
    candidates.execute.mockReset();
    candidates.revert.mockReset();
    cycle.status.mockReset();
    candidates.get.mockResolvedValue(makeCandidate());
    candidates.revert.mockResolvedValue(makeCandidate({ status: 'reverted' }));
    cycle.status.mockResolvedValue({ state: 'direct_control', velocityScale: 1 });
    // Reset store + cancel module timers.
    useRobotControlCandidateStore.getState().clear();
    useRobotControlCandidateStore.setState({
      candidate: null,
      busy: false,
      approving: false,
      rejection: null,
      error: null,
    });
  });

  afterEach(() => {
    useRobotControlCandidateStore.getState().clear();
    vi.clearAllTimers();
    vi.useRealTimers();
  });

  it('authors a candidate after debounce and never moves the robot', async () => {
    candidates.create.mockResolvedValue(makeCandidate());
    useRobotControlCandidateStore.getState().author(JOINT_REQUEST);
    // Nothing fires before the debounce window elapses.
    expect(candidates.create).not.toHaveBeenCalled();
    await vi.advanceTimersByTimeAsync(150);
    expect(candidates.create).toHaveBeenCalledTimes(1);
    expect(candidates.create).toHaveBeenCalledWith(JOINT_REQUEST);
    expect(useRobotControlCandidateStore.getState().candidate?.status).toBe('ready');
    // Authoring is a no-motion preview: it never executes.
    expect(candidates.execute).not.toHaveBeenCalled();
  });

  it('is latest-wins: rapid edits coalesce to the last draft', async () => {
    candidates.create.mockResolvedValue(makeCandidate());
    const store = useRobotControlCandidateStore.getState();
    store.author({
      ...JOINT_REQUEST,
      jointTarget: { jointNames: ['j1'], positionsRad: [0.1] },
    });
    store.author({
      ...JOINT_REQUEST,
      jointTarget: { jointNames: ['j1'], positionsRad: [0.9] },
    });
    await vi.advanceTimersByTimeAsync(150);
    expect(candidates.create).toHaveBeenCalledTimes(1);
    expect(candidates.create).toHaveBeenCalledWith({
      ...JOINT_REQUEST,
      jointTarget: { jointNames: ['j1'], positionsRad: [0.9] },
    });
  });

  it('approve dispatches a ready candidate and marks it executing', async () => {
    candidates.create.mockResolvedValue(makeCandidate());
    candidates.execute.mockResolvedValue({
      candidate: makeCandidate({ status: 'executing', commitEligible: false }),
      session: { sessionId: 'sess-1', status: 'running' },
      timestamp_ns: 1,
    });
    useRobotControlCandidateStore.getState().author(JOINT_REQUEST);
    await vi.advanceTimersByTimeAsync(150);

    await useRobotControlCandidateStore.getState().approve();
    expect(candidates.execute).toHaveBeenCalledWith('cand-1');
    expect(useRobotControlCandidateStore.getState().candidate?.status).toBe(
      'executing',
    );
    expect(useRobotControlCandidateStore.getState().rejection).toBeNull();
  });

  it('ignores new authoring requests while an arm candidate is executing', async () => {
    useRobotControlCandidateStore.setState({
      candidate: makeCandidate({
        status: 'executing',
        commitEligible: false,
        execution: {
          moveId: 'rc_approve_1',
          executionId: 7,
          state: 'executing',
          progress: 0.2,
          sessionId: 'sess-1',
          sessionStatus: 'running',
        },
      }),
      busy: false,
      approving: false,
      rejection: null,
      error: null,
    });

    useRobotControlCandidateStore.getState().author({
      ...JOINT_REQUEST,
      jointTarget: { jointNames: ['j1'], positionsRad: [0.9] },
    });
    await vi.advanceTimersByTimeAsync(150);

    expect(candidates.create).not.toHaveBeenCalled();
    expect(useRobotControlCandidateStore.getState().candidate?.status).toBe(
      'executing',
    );
    expect(
      useRobotControlCandidateStore.getState().candidate?.execution?.moveId,
    ).toBe('rc_approve_1');
  });

  it('maps an executing arm candidate to the server terminal outcome', async () => {
    candidates.create.mockResolvedValue(makeCandidate());
    candidates.execute.mockResolvedValue({
      candidate: makeCandidate({
        status: 'executing',
        commitEligible: false,
        execution: {
          moveId: 'rc_approve_1',
          executionId: 7,
          state: 'executing',
          progress: 0.2,
          sessionId: 'sess-1',
          sessionStatus: 'running',
        },
      }),
      session: { sessionId: 'sess-1', status: 'running' },
      timestamp_ns: 1,
    });
    candidates.get.mockResolvedValueOnce(
      makeCandidate({
        status: 'completed',
        commitEligible: false,
        diagnostics: {
          reasonCode: 'execution_completed',
          summary: 'Candidate execution completed.',
        },
        execution: {
          moveId: 'rc_approve_1',
          executionId: 7,
          state: 'completed',
          progress: 1,
          sessionId: 'sess-1',
          sessionStatus: 'completed',
          sessionActualOutcome: 'stay_idle',
        },
      }),
    );

    useRobotControlCandidateStore.getState().author(JOINT_REQUEST);
    await vi.advanceTimersByTimeAsync(150);
    await useRobotControlCandidateStore.getState().approve();
    expect(useRobotControlCandidateStore.getState().candidate?.status).toBe(
      'executing',
    );

    await vi.advanceTimersByTimeAsync(500);
    expect(useRobotControlCandidateStore.getState().candidate?.status).toBe(
      'completed',
    );
    expect(candidates.get).toHaveBeenCalledWith('cand-1');
    expect(
      useRobotControlCandidateStore.getState().candidate?.diagnostics?.reasonCode,
    ).toBe('execution_completed');
  });

  it('polls a sessionless gripper execution to its server terminal outcome', async () => {
    const gripperReady = makeCandidate({
      sourceSurface: 'gripper_slider',
      inputMode: 'gripper',
      gripperTarget: { gripperId: 'primary', targetPosition: 0.4 },
      ghost: null,
    });
    candidates.create.mockResolvedValue(gripperReady);
    candidates.execute.mockResolvedValue({
      candidate: makeCandidate({
        ...gripperReady,
        status: 'executing',
        commitEligible: false,
      }),
      session: null,
      timestamp_ns: 1,
    });
    candidates.get.mockResolvedValueOnce(
      makeCandidate({
        ...gripperReady,
        status: 'completed',
        commitEligible: false,
        diagnostics: {
          reasonCode: 'execution_completed',
          summary: 'Candidate execution completed.',
        },
      }),
    );

    useRobotControlCandidateStore.getState().author({
      sourceSurface: 'gripper_slider',
      inputMode: 'gripper',
      gripperTarget: { gripperId: 'primary', targetPosition: 0.4 },
    });
    await vi.advanceTimersByTimeAsync(150);
    await useRobotControlCandidateStore.getState().approve();
    expect(useRobotControlCandidateStore.getState().candidate?.status).toBe(
      'executing',
    );

    await vi.advanceTimersByTimeAsync(500);
    expect(candidates.get).toHaveBeenCalledWith('cand-1');
    expect(useRobotControlCandidateStore.getState().candidate?.status).toBe(
      'completed',
    );
  });

  it('approve fails closed (no executing) on a freshness rejection', async () => {
    candidates.create.mockResolvedValue(makeCandidate());
    candidates.execute.mockRejectedValue(
      new ApiError('stale', 409, {
        reasonCode: 'stale_planning_snapshot',
        detail: 'robot moved since planning',
        candidate: makeCandidate({ status: 'stale', commitEligible: false }),
      }),
    );
    useRobotControlCandidateStore.getState().author(JOINT_REQUEST);
    await vi.advanceTimersByTimeAsync(150);

    await useRobotControlCandidateStore.getState().approve();
    const state = useRobotControlCandidateStore.getState();
    expect(state.rejection?.reasonCode).toBe('stale_planning_snapshot');
    expect(state.candidate?.status).toBe('stale');
    expect(state.candidate?.status).not.toBe('executing');
  });

  it('author() makes the prior ready candidate non-approvable before the debounce', async () => {
    // First draft settles to a ready candidate.
    candidates.create.mockResolvedValueOnce(makeCandidate({ candidateId: 'cand-1' }));
    useRobotControlCandidateStore.getState().author(JOINT_REQUEST);
    await vi.advanceTimersByTimeAsync(150);
    const settled = useRobotControlCandidateStore.getState();
    expect(settled.candidate?.candidateId).toBe('cand-1');
    expect(settled.candidate?.status).toBe('ready');
    expect(settled.candidate?.commitEligible).toBe(true);
    expect(settled.busy).toBe(false);

    // A new edit arrives. BEFORE the debounce fires, the prior ready candidate
    // must already be non-approvable: busy true + a planning, non-eligible
    // placeholder (no stale green candidate that Approve could execute).
    useRobotControlCandidateStore.getState().author({
      ...JOINT_REQUEST,
      jointTarget: { jointNames: ['j1'], positionsRad: [0.7] },
    });
    const pending = useRobotControlCandidateStore.getState();
    expect(pending.busy).toBe(true);
    expect(pending.candidate?.status).toBe('planning');
    expect(pending.candidate?.commitEligible).toBe(false);
    // Nothing executed during this window.
    expect(candidates.execute).not.toHaveBeenCalled();
  });

  it('authoring a different move group best-effort reverts the prior server candidate', async () => {
    candidates.create.mockResolvedValueOnce(makeCandidate({ candidateId: 'arm-1' }));
    useRobotControlCandidateStore.getState().author(JOINT_REQUEST);
    await vi.advanceTimersByTimeAsync(150);
    expect(useRobotControlCandidateStore.getState().candidate?.inputMode).toBe('joint');

    candidates.revert.mockClear();
    candidates.create.mockResolvedValueOnce(
      makeCandidate({
        candidateId: 'gripper-1',
        sourceSurface: 'gripper_slider',
        inputMode: 'gripper',
        gripperTarget: { gripperId: 'primary', targetPosition: 0.4 },
        ghost: null,
      }),
    );

    useRobotControlCandidateStore.getState().author({
      sourceSurface: 'gripper_slider',
      inputMode: 'gripper',
      gripperTarget: { gripperId: 'primary', targetPosition: 0.4 },
    });

    expect(candidates.revert).toHaveBeenCalledTimes(1);
    expect(candidates.revert).toHaveBeenCalledWith('arm-1');
    expect(useRobotControlCandidateStore.getState().candidate?.inputMode).toBe(
      'gripper',
    );
    expect(useRobotControlCandidateStore.getState().candidate?.commitEligible).toBe(false);

    await vi.advanceTimersByTimeAsync(150);
    expect(useRobotControlCandidateStore.getState().candidate?.candidateId).toBe(
      'gripper-1',
    );
  });

  it('after the new create resolves, approve targets the NEW candidateId only', async () => {
    // First draft → cand-1 (ready).
    candidates.create.mockResolvedValueOnce(makeCandidate({ candidateId: 'cand-1' }));
    useRobotControlCandidateStore.getState().author(JOINT_REQUEST);
    await vi.advanceTimersByTimeAsync(150);
    expect(useRobotControlCandidateStore.getState().candidate?.candidateId).toBe(
      'cand-1',
    );

    // Second edit → cand-2 (ready). The prior cand-1 is superseded.
    candidates.create.mockResolvedValueOnce(makeCandidate({ candidateId: 'cand-2' }));
    candidates.execute.mockResolvedValue({
      candidate: makeCandidate({
        candidateId: 'cand-2',
        status: 'executing',
        commitEligible: false,
      }),
      session: { sessionId: 'sess-2', status: 'running' },
      timestamp_ns: 1,
    });
    useRobotControlCandidateStore.getState().author({
      ...JOINT_REQUEST,
      jointTarget: { jointNames: ['j1'], positionsRad: [0.7] },
    });
    await vi.advanceTimersByTimeAsync(150);
    const settled = useRobotControlCandidateStore.getState();
    expect(settled.candidate?.candidateId).toBe('cand-2');
    expect(settled.busy).toBe(false);

    await useRobotControlCandidateStore.getState().approve();
    // Approve executed the NEW candidate, never the stale cand-1.
    expect(candidates.execute).toHaveBeenCalledTimes(1);
    expect(candidates.execute).toHaveBeenCalledWith('cand-2');
    expect(candidates.execute).not.toHaveBeenCalledWith('cand-1');
  });

  it('revert clears the candidate and never moves the robot', async () => {
    candidates.create.mockResolvedValue(makeCandidate());
    useRobotControlCandidateStore.getState().author(JOINT_REQUEST);
    await vi.advanceTimersByTimeAsync(150);
    expect(useRobotControlCandidateStore.getState().candidate).not.toBeNull();

    await useRobotControlCandidateStore.getState().revert();
    expect(candidates.revert).toHaveBeenCalledWith('cand-1');
    expect(useRobotControlCandidateStore.getState().candidate).toBeNull();
    expect(candidates.execute).not.toHaveBeenCalled();
  });

  it('rejectDraft makes a prior ready candidate non-approvable and best-effort reverts it', async () => {
    candidates.create.mockResolvedValue(makeCandidate({ candidateId: 'cand-1' }));
    useRobotControlCandidateStore.getState().author(JOINT_REQUEST);
    await vi.advanceTimersByTimeAsync(150);
    const ready = useRobotControlCandidateStore.getState();
    expect(ready.candidate?.status).toBe('ready');
    expect(ready.candidate?.commitEligible).toBe(true);

    // The gizmo could not resolve the base frame: reject the draft.
    candidates.revert.mockClear();
    useRobotControlCandidateStore.getState().rejectDraft('base frame unavailable');
    const rejected = useRobotControlCandidateStore.getState();
    // The prior ready candidate must no longer be approvable; its id is dropped.
    expect(rejected.candidate?.status).not.toBe('ready');
    expect(rejected.candidate?.commitEligible).toBe(false);
    expect(rejected.candidate?.candidateId).toBe('');
    // Best-effort server cleanup of the prior candidate; never executes.
    expect(candidates.revert).toHaveBeenCalledWith('cand-1');
    expect(candidates.execute).not.toHaveBeenCalled();
  });

  it('approve() does NOT execute after rejectDraft (fail closed at the action)', async () => {
    candidates.create.mockResolvedValue(makeCandidate({ candidateId: 'cand-1' }));
    useRobotControlCandidateStore.getState().author(JOINT_REQUEST);
    await vi.advanceTimersByTimeAsync(150);
    expect(useRobotControlCandidateStore.getState().candidate?.status).toBe('ready');

    useRobotControlCandidateStore.getState().rejectDraft('base frame unavailable');
    // Invoke approve() directly (simulating a stale event / non-button caller).
    await useRobotControlCandidateStore.getState().approve();
    expect(candidates.execute).not.toHaveBeenCalled();
  });

  it('keeps the prior ghost on the planning placeholder (no overlay flicker) while non-approvable', async () => {
    candidates.create.mockResolvedValueOnce(makeCandidate({ candidateId: 'cand-1' }));
    useRobotControlCandidateStore.getState().author(JOINT_REQUEST);
    await vi.advanceTimersByTimeAsync(150);
    const ready = useRobotControlCandidateStore.getState().candidate;
    expect(ready?.status).toBe('ready');
    expect(ready?.ghost).toBeDefined();
    const priorGhost = ready?.ghost;

    // A new edit installs the synchronous planning placeholder: it must KEEP the
    // prior ghost (so the 3D overlay does not flicker off) but stay non-approvable.
    useRobotControlCandidateStore.getState().author({
      ...JOINT_REQUEST,
      jointTarget: { jointNames: ['j1'], positionsRad: [0.7] },
    });
    const pending = useRobotControlCandidateStore.getState().candidate;
    expect(pending?.status).toBe('planning');
    expect(pending?.commitEligible).toBe(false);
    expect(pending?.ghost).toEqual(priorGhost);
  });

  it('approve() does NOT execute during the debounce planning placeholder', async () => {
    candidates.create.mockResolvedValue(makeCandidate({ candidateId: 'cand-1' }));
    // First draft settles ready.
    useRobotControlCandidateStore.getState().author(JOINT_REQUEST);
    await vi.advanceTimersByTimeAsync(150);
    expect(useRobotControlCandidateStore.getState().candidate?.status).toBe('ready');

    // A new edit installs a synchronous planning placeholder (busy, non-eligible)
    // that retains the prior candidateId; the debounce has NOT elapsed.
    useRobotControlCandidateStore.getState().author({
      ...JOINT_REQUEST,
      jointTarget: { jointNames: ['j1'], positionsRad: [0.7] },
    });
    expect(useRobotControlCandidateStore.getState().candidate?.status).toBe('planning');
    // approve() during the planning window must not dispatch the prior candidate.
    await useRobotControlCandidateStore.getState().approve();
    expect(candidates.execute).not.toHaveBeenCalled();
  });

  it('revert skips backend revert for a blank-id / terminal candidate', async () => {
    // Local-only placeholder (blank id, terminal status) as rejectDraft installs.
    useRobotControlCandidateStore.setState({
      candidate: makeCandidate({
        candidateId: '',
        status: 'error',
        commitEligible: false,
      }),
    });
    candidates.revert.mockClear();
    await useRobotControlCandidateStore.getState().revert();
    // Nothing to revert server-side; local state still cleared, no motion.
    expect(candidates.revert).not.toHaveBeenCalled();
    expect(useRobotControlCandidateStore.getState().candidate).toBeNull();
  });

  it('revert does NOT cancel an executing candidate (no server revert, not cleared)', async () => {
    candidates.create.mockResolvedValue(makeCandidate({ candidateId: 'cand-1' }));
    candidates.execute.mockResolvedValue({
      candidate: makeCandidate({
        candidateId: 'cand-1',
        status: 'executing',
        commitEligible: false,
      }),
      session: { sessionId: 'sess-1', status: 'running' },
      timestamp_ns: 1,
    });
    useRobotControlCandidateStore.getState().author(JOINT_REQUEST);
    await vi.advanceTimersByTimeAsync(150);
    await useRobotControlCandidateStore.getState().approve();
    expect(useRobotControlCandidateStore.getState().candidate?.status).toBe(
      'executing',
    );

    candidates.revert.mockClear();
    await useRobotControlCandidateStore.getState().revert();
    // Revert is a no-op for an executing candidate: cancellation is Stop's job.
    expect(candidates.revert).not.toHaveBeenCalled();
    // Local state is NOT cleared — that would look like the move was cancelled.
    expect(useRobotControlCandidateStore.getState().candidate?.status).toBe(
      'executing',
    );
  });
});
