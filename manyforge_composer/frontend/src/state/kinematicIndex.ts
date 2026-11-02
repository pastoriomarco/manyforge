import type { JsonValue, SourceModelProjection } from '../types/api';

/**
 * The cell's mount index (B6), the GUI form of the C++ `CellKinematicIndex`
 * mount queries. One helper, two inputs that are never mixed: the Applied
 * view reads the source model's `components[]` rows, the Draft view reads
 * the Deployment Draft's attachment rows. It holds no state of its own.
 */
export interface MountPoint {
  componentId: string;
  interfaceId: string;
}

export interface KinematicIndex {
  /** The parent interface a component is mounted on, or null. */
  mountedOn: (componentId: string) => MountPoint | null;
  /** Components mounted directly on a component, in input order. */
  childrenOf: (componentId: string) => string[];
  /**
   * The component mounted on one interface of a component: the one marked
   * primary (B5), else the first; null when none is mounted there.
   */
  toolOn: (componentId: string, interfaceId: string) => string | null;
  /** The mount chain above a component, nearest first. */
  ancestorsOf: (componentId: string) => string[];
  /** True when `member` is mounted on `carrier`, directly or through a chain. */
  carries: (carrier: string, member: string) => boolean;
  /** The component and everything mounted on it, transitively. */
  selfAndMounted: (componentId: string) => string[];
}

interface MountEdge {
  parent: MountPoint;
  child: string;
  /** B5: the primary tool of its parent interface (Draft view only). */
  primary: boolean;
}

function indexOf(edges: readonly MountEdge[]): KinematicIndex {
  const parentOf = new Map(edges.map((edge) => [edge.child, edge.parent]));
  const childrenOf = (componentId: string) => edges
    .filter((edge) => edge.parent.componentId === componentId)
    .map((edge) => edge.child);
  const ancestorsOf = (componentId: string) => {
    const chain: string[] = [];
    // A Draft is unvalidated; the visited check only ends a chain that loops.
    let parent = parentOf.get(componentId);
    while (parent !== undefined && !chain.includes(parent.componentId)) {
      chain.push(parent.componentId);
      parent = parentOf.get(parent.componentId);
    }
    return chain;
  };
  return {
    mountedOn: (componentId) => parentOf.get(componentId) ?? null,
    childrenOf,
    toolOn: (componentId, interfaceId) => {
      const mounted = edges.filter((edge) => (
        edge.parent.componentId === componentId && edge.parent.interfaceId === interfaceId
      ));
      return (mounted.find((edge) => edge.primary) ?? mounted[0])?.child ?? null;
    },
    ancestorsOf,
    carries: (carrier, member) => member !== carrier && ancestorsOf(member).includes(carrier),
    selfAndMounted: (componentId) => {
      const result = [componentId];
      for (let index = 0; index < result.length; index += 1) {
        for (const child of childrenOf(result[index])) {
          if (!result.includes(child)) result.push(child);
        }
      }
      return result;
    },
  };
}

/** The Applied view: the active source model's component rows. */
export function fromSourceModel(sourceModel: Pick<SourceModelProjection, 'components'>): KinematicIndex {
  return indexOf(sourceModel.components.flatMap((row) => row.mounted_on === null ? [] : [{
    parent: { componentId: row.mounted_on.component_id, interfaceId: row.mounted_on.interface_id },
    child: row.component_id,
    primary: false,
  }]));
}

/** One Deployment attachment row: `child` mounted on the `parent` interface. */
export interface DraftAttachment {
  parent: { componentId: string; interface: string };
  child: { componentId: string; interface: string };
  /** B5: the row carries `primary: true`. */
  primary: boolean;
}

function record(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && value !== undefined && typeof value === 'object' && !Array.isArray(value)
    ? value : null;
}

/** The Draft's attachment rows (intent v2 documents only). */
export function draftAttachments(document: Record<string, JsonValue> | null): DraftAttachment[] {
  const rows = document?.attachments;
  return Array.isArray(rows) ? rows.flatMap((row) => {
    const parent = record(record(row)?.parent);
    const child = record(record(row)?.child);
    return typeof parent?.component_id === 'string' && typeof parent.interface === 'string'
      && typeof child?.component_id === 'string' && typeof child.interface === 'string'
      ? [{
        parent: { componentId: parent.component_id, interface: parent.interface },
        child: { componentId: child.component_id, interface: child.interface },
        primary: record(row)?.primary === true,
      }]
      : [];
  }) : [];
}

/** The Draft view: the Deployment Draft's attachment rows, saved or not. */
export function fromDraft(document: Record<string, JsonValue> | null): KinematicIndex {
  return indexOf(draftAttachments(document).map((row) => ({
    parent: { componentId: row.parent.componentId, interfaceId: row.parent.interface },
    child: row.child.componentId,
    primary: row.primary,
  })));
}
