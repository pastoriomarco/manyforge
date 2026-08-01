import { describe, expect, it } from 'vitest';
import type { CatalogCapabilityRequirement } from '../types/api';
import { supportsRequiredCapabilities } from './deploymentCapabilities';

const requirements: CatalogCapabilityRequirement[] = [
  { capabilityId: 'motion.plan', scope: 'target' },
  { capabilityId: 'motion.execute', scope: 'target' },
];

describe('catalog capability filtering', () => {
  it('requires every declared capability without interpreting deployment DTOs', () => {
    expect(
      supportsRequiredCapabilities(
        new Set(['motion.plan', 'motion.execute']),
        requirements,
      ),
    ).toBe(true);
    expect(
      supportsRequiredCapabilities(new Set(['motion.plan']), requirements),
    ).toBe(false);
  });
});
