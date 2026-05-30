import { create } from 'zustand';

import { ApiError, api } from '../api/client';
import type {
  CandidateCreateRequest,
  CandidateExecuteResponse,
  CandidateResponse,
} from '../types/api';

/**
 * Validated Robot Control candidate store (Phase 4-P5.2).
 *
 * Authoring a draft target creates a server-owned candidate that the backend
 * plans + validates WITHOUT moving the robot. The store tracks the latest
 * candidate (latest-wins; a newer draft supersedes the prior), refreshes its
 * status so it can go stale/expired without a new edit, and exposes Approve
 * (the only action that moves the robot) and Revert (no motion).
 *
 * The UI authors at frame rate; this store debounces the server candidate
 * request so validation runs at a bounded cadence. Dropped requests are
 * explicit in the candidate generation/status rather than hidden behind a stale
 * green indicator.
 */

// Statuses that will not change without a new author()/approve()/revert(), so
// polling can stop.
const TERMINAL_STATUSES = new Set([
  'superseded',
  'expired',
  'completed',
  'stopped',
  'interrupted',
  'failed',
  'reverted',
  'error',
]);
const SERVER_REVERT_SKIP_STATUSES = new Set([...TERMINAL_STATUSES, 'executing']);

const AUTHOR_DEBOUNCE_MS = 150;
const STATUS_POLL_MS = 1000;
const EXECUTION_POLL_MS = 500;

export interface CandidateRejection {
  reasonCode: string;
  detail: string;
}

interface RobotControlCandidateStore {
  candidate: CandidateResponse | null;
  // True while a create/approve request is in flight (for spinner affordances;
  // must NOT gate unrelated global Run/Stop controls).
  busy: boolean;
  approving: boolean;
  // Last Approve rejection (stale / replan-required / blocked / infra). Cleared
  // on the next author()/approve()/revert().
  rejection: CandidateRejection | null;
  // Last infrastructure error (network/5xx) distinct from a candidate-status
  // rejection.
  error: string | null;

  /** Author (or replace) the latest draft candidate. Debounced, latest-wins.
   *  Never moves the robot. */
  author: (request: CandidateCreateRequest) => void;
  /** Reject the active draft locally (no backend call, no motion): replace it
   *  with a non-approvable error placeholder. Used when the UI cannot form a
   *  valid request (e.g. the gizmo cannot resolve the robot base frame), so a
   *  prior ready candidate is never left approvable. */
  rejectDraft: (detail: string) => void;
  /** Approve the active candidate through normal execution. Only succeeds for a
   *  fresh `ready` candidate; otherwise resolves with no motion + a rejection. */
  approve: () => Promise<void>;
  /** Revert the active candidate (no motion) and clear overlays. */
  revert: () => Promise<void>;
  /** Clear local candidate/overlay state (best-effort server revert). */
  clear: () => void;
}

// Module-scoped timers + a generation token guarding against out-of-order
// responses from superseded drafts.
let authorTimer: ReturnType<typeof setTimeout> | null = null;
let pollTimer: ReturnType<typeof setTimeout> | null = null;
let pendingRequest: CandidateCreateRequest | null = null;
let generation = 0;

function clearTimers(): void {
  if (authorTimer !== null) {
    clearTimeout(authorTimer);
    authorTimer = null;
  }
  if (pollTimer !== null) {
    clearTimeout(pollTimer);
    pollTimer = null;
  }
}

export const useRobotControlCandidateStore = create<RobotControlCandidateStore>(
  (set, get) => {
    function schedulePoll(
      candidateId: string,
      token: number,
      intervalMs = STATUS_POLL_MS,
    ): void {
      if (pollTimer !== null) {
        clearTimeout(pollTimer);
      }
      pollTimer = setTimeout(() => {
        if (token !== generation) {
          return; // superseded by a newer draft
        }
        api.robotControl.candidates
          .get(candidateId)
          .then((updated) => {
            if (token !== generation) {
              return;
            }
            set({ candidate: updated });
            if (!TERMINAL_STATUSES.has(updated.status)) {
              schedulePoll(
                candidateId,
                token,
                updated.status === 'executing'
                  ? EXECUTION_POLL_MS
                  : STATUS_POLL_MS,
              );
            }
          })
          .catch(() => {
            // Transient poll failure: try again on the next tick while current.
            if (token === generation) {
              schedulePoll(candidateId, token, intervalMs);
            }
          });
      }, intervalMs);
    }

    return {
      candidate: null,
      busy: false,
      approving: false,
      rejection: null,
      error: null,

      author: (request: CandidateCreateRequest) => {
        if (get().candidate?.status === 'executing') {
          return;
        }
        // Latest-wins: bump the generation immediately so any in-flight
        // create/poll for the prior draft is ignored when it resolves.
        generation += 1;
        const token = generation;
        pendingRequest = request;
        // A new draft is being composed: the prior candidate is now stale and
        // must NOT be approvable during the debounce + create window. Replace
        // it synchronously with a local `planning` placeholder (commitEligible
        // false) so the Approve button disables instantly on any edit, even
        // before the server create resolves. The latest-wins generation guard
        // swaps in the real candidate when create() returns.
        //
        // The placeholder CARRIES THE PRIOR GHOST so the 3D overlay does not
        // flicker off while validation is pending: the previously displayed
        // ghost stays visible (visually stale, status `planning`) until the new
        // validated/blocked ghost arrives. Because the Approve gate keys off
        // `status === 'ready'`, a carried ghost on a `planning` placeholder
        // never implies executability.
        const prior = get().candidate;
        const placeholder: CandidateResponse = {
          candidateId: prior?.candidateId ?? '',
          generation: prior?.generation ?? 0,
          sourceSurface: request.sourceSurface,
          inputMode: request.inputMode,
          status: 'planning',
          commitEligible: false,
          jointTarget: request.jointTarget ?? null,
          cartesianTarget: request.cartesianTarget ?? null,
          gripperTarget: request.gripperTarget ?? null,
          ghost: prior?.ghost ?? null,
          diagnostics: { reasonCode: 'planning', summary: 'composing draft' },
          maxAgeNs: prior?.maxAgeNs ?? 0,
          expiresAtNs: 0,
          createdAtNs: prior?.createdAtNs ?? 0,
          updatedAtNs: 0,
        };
        // Stop polling the prior (now-superseded) candidate; the bumped
        // generation already invalidates any in-flight create/poll responses.
        if (pollTimer !== null) {
          clearTimeout(pollTimer);
          pollTimer = null;
        }
        set({
          candidate: placeholder,
          rejection: null,
          error: null,
          busy: true,
        });
        if (authorTimer !== null) {
          clearTimeout(authorTimer);
        }
        authorTimer = setTimeout(() => {
          const payload = pendingRequest;
          if (payload === null || token !== generation) {
            return;
          }
          api.robotControl.candidates
            .create(payload)
            .then((candidate) => {
              if (token !== generation) {
                return; // a newer draft superseded this one
              }
              set({ candidate, busy: false });
              if (!TERMINAL_STATUSES.has(candidate.status)) {
                schedulePoll(candidate.candidateId, token);
              }
            })
            .catch((err: unknown) => {
              if (token !== generation) {
                return;
              }
              set({
                busy: false,
                error:
                  err instanceof Error ? err.message : 'candidate request failed',
              });
            });
        }, AUTHOR_DEBOUNCE_MS);
      },

      rejectDraft: (detail: string) => {
        // The UI could not form a valid request for the active draft (e.g. the
        // gizmo cannot resolve the robot base frame). Fail closed: bump the
        // generation so any in-flight create/poll for the prior draft is
        // ignored, stop timers, and replace the candidate with a non-approvable
        // error placeholder. The placeholder's candidateId is BLANKED so a stale
        // approve() has nothing to execute, and we best-effort revert the prior
        // server-owned candidate so it cannot linger `ready` on the server.
        // No motion.
        generation += 1;
        clearTimers();
        const prior = get().candidate;
        if (
          prior !== null &&
          prior.candidateId !== '' &&
          !SERVER_REVERT_SKIP_STATUSES.has(prior.status)
        ) {
          api.robotControl.candidates.revert(prior.candidateId).catch(() => {});
        }
        set({
          candidate: {
            candidateId: '',
            generation: prior?.generation ?? 0,
            sourceSurface: prior?.sourceSurface ?? 'cartesian_gizmo',
            inputMode: prior?.inputMode ?? 'cartesian',
            status: 'error',
            commitEligible: false,
            jointTarget: prior?.jointTarget ?? null,
            cartesianTarget: prior?.cartesianTarget ?? null,
            diagnostics: { reasonCode: 'frame_unavailable', summary: detail },
            maxAgeNs: prior?.maxAgeNs ?? 0,
            expiresAtNs: 0,
            createdAtNs: prior?.createdAtNs ?? 0,
            updatedAtNs: 0,
          },
          busy: false,
          rejection: null,
          error: detail,
        });
      },

      approve: async () => {
        const candidate = get().candidate;
        if (candidate === null) {
          return;
        }
        // Fail closed at the action level, not just via the disabled button:
        // only a settled, current `ready` candidate may be executed. A `planning`
        // placeholder (debounce window) or an `error` placeholder from
        // rejectDraft() keeps the PRIOR candidateId, so without this guard a
        // direct or stale approve() could dispatch a prior server candidate that
        // is still ready. Never call execute() unless local state proves ready.
        if (
          candidate.status !== 'ready' ||
          candidate.commitEligible !== true ||
          get().busy
        ) {
          return;
        }
        set({ approving: true, rejection: null, error: null });
        try {
          const result: CandidateExecuteResponse =
            await api.robotControl.candidates.execute(candidate.candidateId);
          generation += 1;
          const token = generation;
          clearTimers();
          set({
            candidate: result.candidate ?? candidate,
            approving: false,
          });
          const executingCandidate = result.candidate ?? candidate;
          // Arm candidates execute through the cycle/intervention machinery.
          // Keep polling the server-owned candidate until it maps the concrete
          // runtime outcome (completed / stopped / interrupted / failed). Do
          // not infer completion from the global cycle state alone: that loses
          // the execution id, move id, and structured kernel diagnostics.
          if (
            result.session !== null &&
            executingCandidate.status === 'executing'
          ) {
            schedulePoll(executingCandidate.candidateId, token, EXECUTION_POLL_MS);
          }
        } catch (err: unknown) {
          // A fresh-only / freshness rejection arrives as a 409 carrying a
          // structured reason; surface it without motion.
          if (err instanceof ApiError && err.payload && typeof err.payload === 'object') {
            const payload = err.payload as Record<string, unknown>;
            const reasonCode =
              typeof payload.reasonCode === 'string'
                ? payload.reasonCode
                : 'candidate_not_ready';
            const detail =
              typeof payload.detail === 'string' ? payload.detail : err.message;
            const updated = payload.candidate;
            set({
              approving: false,
              rejection: { reasonCode, detail },
              candidate:
                updated && typeof updated === 'object'
                  ? (updated as CandidateResponse)
                  : candidate,
            });
          } else {
            set({
              approving: false,
              error: err instanceof Error ? err.message : 'approve failed',
            });
          }
        }
      },

      revert: async () => {
        const candidate = get().candidate;
        if (candidate !== null && candidate.status === 'executing') {
          // Execution cancellation belongs to Stop, not Revert. Do NOT call the
          // server revert and do NOT clear local state: clearing would make it
          // look like the in-flight move was cancelled, which Revert cannot do.
          // The candidate stays visible as `executing` until execution ends.
          return;
        }
        generation += 1; // stop any in-flight create/poll
        clearTimers();
        set({ candidate: null, busy: false, rejection: null, error: null });
        if (candidate === null) {
          return;
        }
        if (
          candidate.candidateId === '' ||
          SERVER_REVERT_SKIP_STATUSES.has(candidate.status)
        ) {
          // Nothing to revert server-side: a blank id is a local-only
          // placeholder (e.g. from rejectDraft), and a terminal candidate
          // (superseded / expired / reverted / error) has no live server draft
          // to discard. Local overlays are already cleared above.
          return;
        }
        try {
          await api.robotControl.candidates.revert(candidate.candidateId);
        } catch {
          // Local overlays are already cleared; a failed server revert only
          // affects the (now-orphaned) server candidate, not robot motion.
        }
      },

      clear: () => {
        const candidate = get().candidate;
        generation += 1;
        clearTimers();
        set({ candidate: null, busy: false, rejection: null, error: null });
        if (
          candidate !== null &&
          !SERVER_REVERT_SKIP_STATUSES.has(candidate.status)
        ) {
          // Best-effort: discard the server-side draft so it cannot linger.
          api.robotControl.candidates.revert(candidate.candidateId).catch(() => {});
        }
      },
    };
  },
);
