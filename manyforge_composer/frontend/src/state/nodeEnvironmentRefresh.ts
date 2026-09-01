import type { ComposerHeadlessState } from '../types/headless';

function objectValue(value: unknown): Record<string, unknown> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
}

/**
 * Identity and health facts that can change executable-node availability.
 *
 * Program-cycle counters, observation ages, and presentation generations are
 * deliberately absent: Run/Stop/Reset must not re-read a large structural
 * environment whose binding and owner health did not change.
 */
export function nodeEnvironmentRefreshKey(
  snapshot: ComposerHeadlessState,
): string {
  const manager = snapshot.runtime.sections.manager;
  const active = objectValue(objectValue(manager.payload)?.active);
  const components = snapshot.componentOverview.components
    .map((component) => ({
      id: component.id,
      ownerEpoch: component.ownerEpoch,
      ownerId: component.ownerId,
      reasonCode: component.reasonCode,
      required: component.required,
      status: component.status,
    }))
    .sort((left, right) => left.id.localeCompare(right.id));
  return JSON.stringify({
    receiverEpoch: snapshot.receiverEpoch,
    manager: {
      available: manager.available,
      freshness: manager.freshness,
      graphRevision: typeof active?.graph_revision === 'string'
        ? active.graph_revision
        : null,
      knownGaps: manager.knownGaps,
      retainedBundleId: typeof active?.retained_bundle_id === 'string'
        ? active.retained_bundle_id
        : null,
      sourceOwnerEpoch: manager.sourceOwnerEpoch,
      sourceOwnerId: manager.sourceOwnerId,
    },
    readiness: snapshot.componentOverview.readiness,
    capabilities: [...snapshot.componentOverview.capabilities].sort(),
    components,
  });
}
