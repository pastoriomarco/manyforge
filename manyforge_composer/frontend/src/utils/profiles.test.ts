import { describe, expect, it } from 'vitest';
import { parseProfile, profileSatisfies } from './profiles';

describe('catalog profile comparison', () => {
  it('parses major and optional minor versions', () => {
    expect(parseProfile('v2')).toEqual([2, 0]);
    expect(parseProfile('v2.3')).toEqual([2, 3]);
    expect(parseProfile('latest')).toBeNull();
  });

  it('accepts target profiles at or above the minimum', () => {
    expect(profileSatisfies('v1.2', 'v1.2')).toBe(true);
    expect(profileSatisfies('v1.2', 'v1.3')).toBe(true);
    expect(profileSatisfies('v2.0', 'v1.9')).toBe(false);
  });
});
