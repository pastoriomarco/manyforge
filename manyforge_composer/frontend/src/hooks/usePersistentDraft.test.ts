import { describe, expect, it } from 'vitest';
import { persistentDraftHasSettled } from './usePersistentDraft';

describe('persistent authored draft settlement', () => {
  it('clears only a finite draft within the observed tolerance', () => {
    expect(persistentDraftHasSettled('0.5', 0.5005, 0.001)).toBe(true);
    expect(persistentDraftHasSettled('0.5', 0.51, 0.001)).toBe(false);
    expect(persistentDraftHasSettled('invalid', 0.5, 0.001)).toBe(false);
    expect(persistentDraftHasSettled(null, 0.5, 0.001)).toBe(false);
  });
});
