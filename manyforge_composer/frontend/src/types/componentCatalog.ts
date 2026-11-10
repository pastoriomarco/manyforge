import type { JsonValue } from './api';

/**
 * What one Add component entry adds; a tool mounts on a robot's flange; a
 * machine is an owner with its members; a sensor (e.g. the
 * point-cloud source) is a `kind: sensor` model without a lane; so is the
 * AI assistant (`kind: assistant`, the AI tab's chat).
 */
export type ComponentCatalogKind = 'machine' | 'robot' | 'tool' | 'io_device' | 'pose_source' | 'ai_model' | 'sensor'
  | 'assistant';

/**
 * A model's own payload (TOOLCTX), from its model asset: package data, not
 * editable in the GUI. Mass in kg; centre of mass in m in the asset's root
 * link (a tool's mount).
 */
export interface ComponentModelPayload {
  massKg: number;
  centerOfMassM: [number, number, number];
}

/**
 * One command target with TCPs: the frame roles its component type tags as
 * TCPs and the one it declares as the target's default.
 */
export interface ComponentTcpTarget {
  commandTargetId: string;
  tcpFrameRoleIds: string[];
  defaultTcpFrameRoleId: string;
}

/** Component-type facts of one component profile. */
export interface ComponentProfileFacts {
  tags: string[];
  /** Attachment interface ids, e.g. a robot's `flange`. */
  attachmentInterfaces: string[];
  /** The targets with TCPs, sorted by id; none for a type without TCPs. */
  tcpTargets: ComponentTcpTarget[];
  /** Absent when target capability facts are incomplete. */
  hasMotionTarget?: boolean;
  /** All declared target frame roles, including frames not tagged as TCPs. */
  frameTargets?: Array<{ commandTargetId: string; frameRoleIds: string[] }>;
  /** Absent: the model defines no payload (0 kg). */
  payload?: ComponentModelPayload;
  /** The lanes the model declares (`component.yaml` `lanes.<lane>`), sorted. */
  lanes?: string[];
}

/**
 * One addable component: complete, known-good Deployment component rows from
 * a curated example Scenario. `components` holds the component and every
 * component it names (a pneumatic robot's I/O device), ready for the Draft.
 */
export interface ComponentCatalogEntry {
  /** A display key (Scenario/component), not an identity. */
  entryId: string;
  kind: ComponentCatalogKind;
  componentId: string;
  componentProfileId: string;
  modelFamilyId: string | null;
  /** Component-type tags, matched against Program role requirements. */
  tags: string[];
  lane: string | null;
  description: string | null;
  sources: string[];
  components: Array<Record<string, JsonValue>>;
}

/** One member slot of a machine model. */
export interface MachineCatalogMember {
  role: string;
  modelId: string;
  title: string;
  /** Its ready Deployment row (default id `<default_owner_id>_<role>`), without `owner` and `role`. */
  row: Record<string, JsonValue>;
}

/**
 * A machine model (`kind: machine`): adding it adds the
 * owner and one member per slot, ids `<owner_id>_<role>`.
 */
export interface MachineCatalogEntry {
  modelId: string;
  title: string;
  lanes: string[];
  defaultOwnerId: string;
  /** The owner's ready Deployment row (id `default_owner_id`), without `lane`. */
  ownerRow: Record<string, JsonValue>;
  members: MachineCatalogMember[];
}

export interface ComponentCatalog {
  schemaVersion: 'manyforge.composer_component_catalog.v1';
  /** The Draft's project folder the model paths are relative to; null = absolute paths. */
  projectRoot: string | null;
  entries: ComponentCatalogEntry[];
  /** The machine models (the catalog's `kind: machine` entries). */
  machines: MachineCatalogEntry[];
  /** Component profiles that exist but have no curated component row yet. */
  profilesWithoutEntry: string[];
  /** Type facts of every profile with a component type (Draft rows included). */
  componentProfiles: Record<string, ComponentProfileFacts>;
}
