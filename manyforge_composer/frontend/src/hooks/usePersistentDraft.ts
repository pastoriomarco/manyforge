import { useEffect, useState } from 'react';

import { useRobotControlCandidateStore } from '../store/robotControlCandidateStore';
import type { CandidateInputMode } from '../types/api';

export interface PersistentDraft {
  /** Current draft text, or null when the control follows the live value. */
  draft: string | null;
  /** True while an authored draft target is held (diverged from live). */
  isDraft: boolean;
  setDraft: (value: string | null) => void;
  clearDraft: () => void;
}

/**
 * Holds a persistent authored-target draft for a Robot Control numeric input.
 *
 * The authored target is KEPT after a commit (spinner click, Enter, blur,
 * slider/gizmo release) — the control does NOT snap back to the live value.
 * The draft is cleared only when:
 *   - the authored candidate is discarded (Revert / mode-tab switch -> the
 *     candidate store candidate becomes null), or
 *   - another move group owns the shared candidate / Approve button, or
 *   - the live value catches up to the draft within `settleTolerance` (e.g.
 *     after an Approve executes and the robot reaches the target), so the
 *     control resumes following live state.
 *
 * This hook never commands motion; Approve remains the only motion path.
 */
export function usePersistentDraft(
  liveValue: number,
  settleTolerance: number,
  requiredCandidateMode?: CandidateInputMode,
): PersistentDraft {
  const [draft, setDraft] = useState<string | null>(null);
  const candidatePresent = useRobotControlCandidateStore(
    (s) => s.candidate !== null,
  );
  const candidateMode = useRobotControlCandidateStore(
    (s) => s.candidate?.inputMode ?? null,
  );

  // Discard: when the authored candidate is gone (Revert / mode switch / clear)
  // or the shared Approve candidate is now owned by another move group, return
  // the control to live. With one Approve button there is only one visible
  // staged command; stale local drafts from another group must not stay amber.
  useEffect(() => {
    const compatible =
      requiredCandidateMode === undefined || candidateMode === requiredCandidateMode;
    if (!candidatePresent || !compatible) {
      setDraft((current) => (current === null ? current : null));
    }
  }, [candidateMode, candidatePresent, requiredCandidateMode]);

  // Live catch-up: once the live value reaches the draft (after execution),
  // drop the draft so the control resumes following live state.
  useEffect(() => {
    if (draft === null) return;
    const target = Number(draft);
    if (!Number.isFinite(target)) return;
    if (Math.abs(target - liveValue) <= settleTolerance) {
      setDraft(null);
    }
  }, [draft, liveValue, settleTolerance]);

  return {
    draft,
    isDraft: draft !== null,
    setDraft,
    clearDraft: () => setDraft(null),
  };
}
