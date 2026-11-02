import { describe, expect, it } from 'vitest';
import { styleTokens } from './readStyleSource';
import { CSS_SHARED_TOKENS } from './tokens';

describe('tokens.ts and tokens.css', () => {
  it('hold the same values for every shared token', () => {
    const css = styleTokens();
    for (const [name, value] of Object.entries(CSS_SHARED_TOKENS)) {
      expect(css.get(name)?.toLowerCase(), name).toBe(value.toLowerCase());
    }
  });
});
