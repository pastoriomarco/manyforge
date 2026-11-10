import type { CatalogCapabilityRequirement } from '../types/api';

/**
 * Filter catalog entries against an already-qualified capability set. Extracting
 * capabilities from a deployment remains future work; this helper owns no artifact
 * or runtime schema.
 */
export function supportsRequiredCapabilities(
  available: ReadonlySet<string>,
  required: readonly CatalogCapabilityRequirement[],
): boolean {
  return required.every((requirement) => available.has(requirement.capabilityId));
}
