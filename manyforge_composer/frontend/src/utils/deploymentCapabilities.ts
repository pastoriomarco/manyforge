import type { FullDeployment } from '../types/api';

export function deploymentCapabilityKeys(
  deployment: FullDeployment | null | undefined,
): Set<string> {
  const capabilities = new Set<string>();
  for (const adapter of Object.values(deployment?.adapters ?? {})) {
    for (const capability of adapter.capabilityKeys ?? []) {
      if (capability.trim()) {
        capabilities.add(capability);
      }
    }
  }
  return capabilities;
}

export function supportsRequiredCapabilities(
  available: ReadonlySet<string>,
  required: readonly string[],
): boolean {
  return required.every((capability) => available.has(capability));
}
