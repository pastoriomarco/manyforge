import { useEffect, useState } from 'react';

export interface PersistentDraft {
  draft: string | null;
  isDraft: boolean;
  setDraft: (value: string | null) => void;
  clearDraft: () => void;
}

export function persistentDraftHasSettled(
  draft: string | null,
  observedValue: number,
  tolerance: number,
): boolean {
  if (draft === null) return false;
  const target = Number(draft);
  return Number.isFinite(target)
    && Number.isFinite(observedValue)
    && Number.isFinite(tolerance)
    && tolerance >= 0
    && Math.abs(target - observedValue) <= tolerance;
}

/**
 * Hold an authored numeric draft until its qualified owner discards it or an
 * observed value settles. The I6 caller must supply `ownerActive`; this hook no
 * longer subscribes to the deleted singleton candidate store.
 */
export function usePersistentDraft(
  observedValue: number,
  settleTolerance: number,
  ownerActive: boolean,
): PersistentDraft {
  const [draft, setDraftState] = useState<string | null>(null);

  useEffect(() => {
    if (!ownerActive) setDraftState(null);
  }, [ownerActive]);

  useEffect(() => {
    if (persistentDraftHasSettled(draft, observedValue, settleTolerance)) {
      setDraftState(null);
    }
  }, [draft, observedValue, settleTolerance]);

  return {
    draft,
    isDraft: draft !== null,
    setDraft: (value) => setDraftState(ownerActive ? value : null),
    clearDraft: () => setDraftState(null),
  };
}
