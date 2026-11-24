import type { JsonValue, SourceModelProjection } from '../types/api';
import type { ComponentProfileFacts } from '../types/componentCatalog';

/**
 * Collision rules: one list on the Deployment (cell rules) and one on each
 * motion node and pick/place sequence (move rules). A rule holds every pair
 * made of one member of `a` and one member of `b` and gives it one value: a
 * clearance, a contact allowance or a release. The row without selections
 * ("Everything else") holds a clearance only. A move rule may instead switch a
 * generated tool's spring slider on (OD-371): `spring_slider` in place of `a`
 * (the slider and the objects it carries), `b`, and a contact holding its end
 * window only; the stroke and step come from the slider.
 */

/** Where a rule list is edited: the Deployment, a motion node, or a pick/place sequence. */
export type CollisionRuleScope = 'deployment' | 'node' | 'sequence';

export const MAX_COLLISION_RULES = 64;
export const MAX_COLLISION_RULE_SELECTIONS = 16;
export const MAX_COLLISION_RULE_ID_BYTES = 128;
export const MAX_COLLISION_CLEARANCE_M = 0.05;
/** The Deployment's first-rule clearance when nothing sets one (robot_defaults.tsv, table `cell`). */
export const DEFAULT_COLLISION_CLEARANCE_M = 0.001;
/** A contact value without `end_from_fraction` may touch from this fraction of the move on; 0 means the whole move. */
export const DEFAULT_CONTACT_END_FROM_FRACTION = 0.8;
export const EVERYTHING_COLLISION_RULE_ID = 'everything';

export const COLLISION_RULE_LEGS = ['approach', 'descend', 'retreat'] as const;
export type CollisionRuleLeg = typeof COLLISION_RULE_LEGS[number];

/** Nodes whose `collision_rules` rows name a leg. */
export const COLLISION_RULE_SEQUENCE_NODE_IDS: readonly string[] = [
  'planning.plan_pick_sequence',
  'planning.plan_place_sequence',
];

export const COLLISION_RULE_SELECTION_KINDS = [
  'component',
  'collision_body',
  'link',
  'model_group',
  'attached_object',
  'scene_objects',
  'scene_object',
  'active_tcp_contact_bodies',
] as const;
export type CollisionRuleSelectionKind = typeof COLLISION_RULE_SELECTION_KINDS[number];

/** Selections a move rule may use and a Deployment rule may not. */
const MOVE_ONLY_SELECTION_KINDS: readonly CollisionRuleSelectionKind[] = [
  'scene_object',
  'active_tcp_contact_bodies',
];

/** Selections a Deployment rule may use and a move rule may not. */
const DEPLOYMENT_ONLY_SELECTION_KINDS: readonly CollisionRuleSelectionKind[] = ['model_group'];

export const COLLISION_RULE_SELECTION_LABELS: Record<CollisionRuleSelectionKind, string> = {
  component: 'Component',
  collision_body: 'Collision body',
  link: 'Link',
  model_group: 'Model group (SRDF)',
  attached_object: 'Object carried by',
  scene_objects: 'All Scene objects',
  scene_object: 'Scene object',
  active_tcp_contact_bodies: 'Contact bodies of the move\'s TCP',
};

export function collisionRuleSelectionKinds(scope: CollisionRuleScope): CollisionRuleSelectionKind[] {
  return COLLISION_RULE_SELECTION_KINDS.filter((kind) => (
    scope === 'deployment'
      ? !MOVE_ONLY_SELECTION_KINDS.includes(kind)
      : !DEPLOYMENT_ONLY_SELECTION_KINDS.includes(kind)
  ));
}

export type CollisionRuleValueKind = 'clearance_m' | 'contact' | 'release';
const VALUE_KINDS: readonly CollisionRuleValueKind[] = ['clearance_m', 'contact', 'release'];

export type ReleaseEvidenceMode = 'signed_penetration' | 'binary_contact';

/** Release fields with the defaults of omitted keys filled in. */
export interface CollisionReleaseValue {
  evidence: ReleaseEvidenceMode;
  enforceMaxStartPenetration: boolean;
  maxStartPenetrationM: number;
  minReleaseProgressM: number;
  releaseToleranceM: number;
  requireStrictlyDecreasingPenetration: boolean;
  requireSeparationByEnd: boolean;
  allowRecontact: boolean;
}

export const SIGNED_RELEASE_DEFAULTS: CollisionReleaseValue = {
  evidence: 'signed_penetration',
  enforceMaxStartPenetration: true,
  maxStartPenetrationM: 0,
  minReleaseProgressM: 0,
  releaseToleranceM: 0.00001,
  requireStrictlyDecreasingPenetration: true,
  requireSeparationByEnd: true,
  allowRecontact: false,
};

type JsonRecord = Record<string, JsonValue>;

export function collisionRuleRecord(value: JsonValue | undefined): JsonRecord | null {
  return value !== null && value !== undefined && typeof value === 'object' && !Array.isArray(value)
    ? value
    : null;
}

/** The rows of a `collision_rules` value; null when it is present and not a list. */
export function collisionRuleRows(value: JsonValue | undefined): JsonValue[] | null {
  if (value === undefined) return [];
  return Array.isArray(value) ? value : null;
}

/** A row without selections: the "Everything else" row. */
export function isEverythingRow(row: JsonValue | undefined): boolean {
  const record = collisionRuleRecord(row);
  return record !== null && record.a === undefined && record.b === undefined
    && record.spring_slider === undefined;
}

/** A row that switches a spring slider on (`spring_slider` in place of `a`). */
export function isSpringSliderRow(row: JsonRecord): boolean {
  return row.spring_slider !== undefined;
}

export function everythingRowIndex(rows: readonly JsonValue[]): number {
  return rows.findIndex((row) => isEverythingRow(row));
}

export function collisionRuleValueKind(row: JsonRecord): CollisionRuleValueKind | null {
  const present = VALUE_KINDS.filter((kind) => row[kind] !== undefined);
  return present.length === 1 ? present[0] : null;
}

function finiteNumber(value: JsonValue | undefined): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value : null;
}

function numberOr(value: JsonValue | undefined, fallback: number): number | null {
  return value === undefined ? fallback : finiteNumber(value);
}

function booleanOr(value: JsonValue | undefined, fallback: boolean): boolean | null {
  if (value === undefined) return fallback;
  return typeof value === 'boolean' ? value : null;
}

/** The release value with the defaults of omitted keys; null when a key has the wrong type. */
export function releaseValueOf(value: JsonValue | undefined): CollisionReleaseValue | null {
  const record = collisionRuleRecord(value);
  if (record === null) return null;
  const evidence = record.release_evidence_mode ?? 'signed_penetration';
  if (evidence !== 'signed_penetration' && evidence !== 'binary_contact') return null;
  const signed = evidence === 'signed_penetration';
  const enforceMaxStartPenetration = booleanOr(record.enforce_max_start_penetration, signed);
  const maxStartPenetrationM = numberOr(record.max_start_penetration_m, 0);
  const minReleaseProgressM = numberOr(record.min_release_progress_m, 0);
  const releaseToleranceM = numberOr(
    record.release_tolerance_m,
    signed ? SIGNED_RELEASE_DEFAULTS.releaseToleranceM : 0,
  );
  const requireStrictlyDecreasingPenetration = booleanOr(
    record.require_strictly_decreasing_penetration,
    signed,
  );
  const requireSeparationByEnd = booleanOr(record.require_separation_by_end, true);
  const allowRecontact = booleanOr(record.allow_recontact, false);
  if (
    enforceMaxStartPenetration === null || maxStartPenetrationM === null
    || minReleaseProgressM === null || releaseToleranceM === null
    || requireStrictlyDecreasingPenetration === null || requireSeparationByEnd === null
    || allowRecontact === null
  ) return null;
  return {
    evidence,
    enforceMaxStartPenetration,
    maxStartPenetrationM,
    minReleaseProgressM,
    releaseToleranceM,
    requireStrictlyDecreasingPenetration,
    requireSeparationByEnd,
    allowRecontact,
  };
}

/**
 * The authored `release` object, every key spelled out. A lightweight (binary
 * contact) release has no penetration metrics: its signed-only fields are
 * written neutral (off, 0).
 */
export function releaseJson(value: CollisionReleaseValue): JsonRecord {
  const signed = value.evidence === 'signed_penetration';
  return {
    release_evidence_mode: value.evidence,
    enforce_max_start_penetration: signed && value.enforceMaxStartPenetration,
    max_start_penetration_m: signed ? value.maxStartPenetrationM : 0,
    min_release_progress_m: signed ? value.minReleaseProgressM : 0,
    release_tolerance_m: signed ? value.releaseToleranceM : 0,
    require_strictly_decreasing_penetration: signed && value.requireStrictlyDecreasingPenetration,
    require_separation_by_end: value.requireSeparationByEnd,
    allow_recontact: value.allowRecontact,
  };
}

function exactKeys(record: JsonRecord, keys: readonly string[]): boolean {
  const present = Object.keys(record);
  return present.length === keys.length && keys.every((key) => record[key] !== undefined);
}

function nonEmptyText(value: JsonValue | undefined): value is string {
  return typeof value === 'string' && value.trim() !== '';
}

/** Why one selection is not valid in this scope; null when it is. */
export function collisionRuleSelectionProblem(
  value: JsonValue | undefined,
  scope: CollisionRuleScope,
): string | null {
  const record = collisionRuleRecord(value);
  const kind = record?.kind;
  if (record === null || typeof kind !== 'string'
    || !(COLLISION_RULE_SELECTION_KINDS as readonly string[]).includes(kind)) {
    return 'Choose what the selection names.';
  }
  const selectionKind = kind as CollisionRuleSelectionKind;
  if (scope === 'deployment' && MOVE_ONLY_SELECTION_KINDS.includes(selectionKind)) {
    return `${COLLISION_RULE_SELECTION_LABELS[selectionKind]} is available on motion nodes only.`;
  }
  if (scope !== 'deployment' && DEPLOYMENT_ONLY_SELECTION_KINDS.includes(selectionKind)) {
    return `${COLLISION_RULE_SELECTION_LABELS[selectionKind]} is available on the Deployment's rules only.`;
  }
  switch (selectionKind) {
    case 'component':
      return exactKeys(record, ['kind', 'component_id']) && nonEmptyText(record.component_id)
        ? null : 'Choose a component.';
    case 'attached_object':
      // Optionally only the objects one collision body carries (a spring slider).
      return (exactKeys(record, ['kind', 'component_id'])
        || (exactKeys(record, ['kind', 'component_id', 'collision_body_role_id'])
          && nonEmptyText(record.collision_body_role_id)))
        && nonEmptyText(record.component_id)
        ? null : 'Choose a component.';
    case 'collision_body':
      return exactKeys(record, ['kind', 'component_id', 'collision_body_id'])
        && nonEmptyText(record.component_id) && nonEmptyText(record.collision_body_id)
        ? null : 'Choose a component and one of its collision bodies.';
    case 'link':
      return exactKeys(record, ['kind', 'component_id', 'link'])
        && nonEmptyText(record.component_id) && nonEmptyText(record.link)
        ? null : 'Choose a component and one of its links.';
    case 'model_group':
      return exactKeys(record, ['kind', 'component_id', 'group'])
        && nonEmptyText(record.component_id) && nonEmptyText(record.group)
        ? null : 'Choose a component and one of its model groups.';
    case 'scene_object':
      // A key of type scene_resource, or one scene_resource field of a structured key.
      return (exactKeys(record, ['kind', 'scene_resource_id']) && nonEmptyText(record.scene_resource_id))
        || (exactKeys(record, ['kind', 'blackboard_key']) && nonEmptyText(record.blackboard_key))
        || (exactKeys(record, ['kind', 'blackboard_key', 'field'])
          && nonEmptyText(record.blackboard_key) && nonEmptyText(record.field))
        ? null : 'Choose a Scene object or a Scene object key.';
    default:
      return exactKeys(record, ['kind']) ? null : 'This selection takes no further fields.';
  }
}

/** Why a spring slider row's slider and value are not valid; null when they are. */
function springSliderProblem(row: JsonRecord, scope: CollisionRuleScope): string | null {
  if (scope === 'deployment') return 'A spring slider is switched on by motion nodes only.';
  const slider = collisionRuleRecord(row.spring_slider);
  if (slider === null || !exactKeys(slider, ['component_id', 'element_id'])
    || !nonEmptyText(slider.component_id) || !nonEmptyText(slider.element_id)) {
    return 'Choose a tool and its spring slider.';
  }
  if (row.a !== undefined || row.clearance_m !== undefined || row.release !== undefined) {
    return 'A spring slider rule has the slider in place of side A and no clearance or release.';
  }
  if (row.contact === undefined) return null;
  const contact = collisionRuleRecord(row.contact);
  if (contact === null || Object.keys(contact).some((key) => key !== 'end_from_fraction')) {
    return 'A spring slider rule takes its stroke and step from the slider; only the end window is set here.';
  }
  const fraction = numberOr(contact.end_from_fraction, DEFAULT_CONTACT_END_FROM_FRACTION);
  return fraction !== null && fraction >= 0 && fraction <= 1
    ? null : 'The end window starts at a fraction from 0 to 1.';
}

function valueProblem(row: JsonRecord): string | null {
  const kind = collisionRuleValueKind(row);
  if (kind === null) return 'Give the rule exactly one value: a clearance, a contact or a release.';
  if (kind === 'clearance_m') {
    const clearance = finiteNumber(row.clearance_m);
    return clearance !== null && clearance >= 0 && clearance <= MAX_COLLISION_CLEARANCE_M
      ? null : `Clearance is a number from 0 to ${MAX_COLLISION_CLEARANCE_M} m.`;
  }
  if (kind === 'contact') {
    const contact = collisionRuleRecord(row.contact);
    if (contact === null
      || Object.keys(contact).some((key) => (
        key !== 'max_penetration_m' && key !== 'end_from_fraction' && key !== 'max_compression_step_m'))) {
      return 'A contact value holds only a depth limit, an end window and a spring step limit.';
    }
    if (contact.max_compression_step_m !== undefined) {
      // A spring (OD-356): the step limit needs the stroke as the depth limit.
      const step = finiteNumber(contact.max_compression_step_m);
      if (step === null || step <= 0) return 'The spring step limit is a number above 0 m.';
      if (contact.max_penetration_m === undefined) return 'A spring step limit needs the deepest penetration (the stroke).';
    }
    if (contact.max_penetration_m !== undefined) {
      const depth = finiteNumber(contact.max_penetration_m);
      if (depth === null || depth < 0) return 'The deepest penetration is a number of 0 m or more.';
    }
    if (contact.end_from_fraction !== undefined) {
      const fraction = finiteNumber(contact.end_from_fraction);
      if (fraction === null || fraction < 0 || fraction > 1) {
        return 'The end window starts at a fraction from 0 to 1.';
      }
    }
    return null;
  }
  const release = releaseValueOf(row.release);
  if (release === null) return 'The release value has a field of the wrong type.';
  return [release.maxStartPenetrationM, release.minReleaseProgressM, release.releaseToleranceM]
    .every((value) => value >= 0) ? null : 'Release distances are numbers of 0 m or more.';
}

function ruleLabel(row: JsonRecord, index: number): string {
  return typeof row.rule_id === 'string' && row.rule_id !== '' ? row.rule_id : `rule ${index + 1}`;
}

/** Why a rule list cannot be written in this scope; null when it can. */
export function collisionRulesProblem(
  rows: readonly JsonValue[],
  scope: CollisionRuleScope,
): string | null {
  if (rows.length > MAX_COLLISION_RULES) return `At most ${MAX_COLLISION_RULES} rules.`;
  const ids = new Set<string>();
  let everything = 0;
  for (const [index, value] of rows.entries()) {
    const row = collisionRuleRecord(value);
    if (row === null) return `Rule ${index + 1} is not an object.`;
    const label = ruleLabel(row, index);
    const id = row.rule_id;
    if (!nonEmptyText(id) || id !== id.trim()
      || new TextEncoder().encode(id).length > MAX_COLLISION_RULE_ID_BYTES) {
      return `Rule ${index + 1} needs an id of at most ${MAX_COLLISION_RULE_ID_BYTES} bytes.`;
    }
    if (ids.has(id)) return `Rule id ${id} is used twice.`;
    ids.add(id);
    const allowed = ['rule_id', 'a', 'b', 'leg', 'spring_slider', ...VALUE_KINDS];
    const unknown = Object.keys(row).find((key) => !allowed.includes(key));
    if (unknown !== undefined) return `${label}: ${unknown} is not a rule field.`;
    if (scope === 'sequence') {
      if (!(COLLISION_RULE_LEGS as readonly string[]).includes(row.leg as string)) {
        return `${label}: choose the leg the rule applies to.`;
      }
    } else if (row.leg !== undefined) {
      return `${label}: only pick and place sequences name a leg.`;
    }
    if (isEverythingRow(value)) {
      everything += 1;
      if (everything > 1) return 'Only one rule may be without selections.';
      if (scope === 'deployment' && index !== 0) return 'The rule without selections is the first rule.';
      if (collisionRuleValueKind(row) !== 'clearance_m') {
        return `${label}: the rule without selections holds a clearance only.`;
      }
    } else {
      const spring = isSpringSliderRow(row);
      if (spring) {
        const problem = springSliderProblem(row, scope);
        if (problem !== null) return `${label}: ${problem}`;
      }
      for (const side of spring ? ['b'] as const : ['a', 'b'] as const) {
        const selections = row[side];
        if (!Array.isArray(selections) || selections.length === 0
          || selections.length > MAX_COLLISION_RULE_SELECTIONS) {
          return `${label}: side ${side.toUpperCase()} needs 1 to ${MAX_COLLISION_RULE_SELECTIONS} selections.`;
        }
        for (const selection of selections) {
          const problem = collisionRuleSelectionProblem(selection, scope);
          if (problem !== null) return `${label}, side ${side.toUpperCase()}: ${problem}`;
        }
      }
    }
    const problem = isSpringSliderRow(row) ? null : valueProblem(row);
    if (problem !== null) return `${label}: ${problem}`;
  }
  if (scope === 'deployment' && everything === 0) {
    return 'Set the Everything else clearance: the Deployment\'s first rule is required.';
  }
  return null;
}

/** One rule row with its keys in the authored order. */
export function orderedCollisionRule(row: JsonRecord): JsonRecord {
  const ordered: JsonRecord = {};
  for (const key of ['rule_id', 'leg', 'spring_slider', 'a', 'b', ...VALUE_KINDS]) {
    if (row[key] !== undefined) ordered[key] = row[key];
  }
  return ordered;
}

/** A rule id not used in the list yet. */
export function nextCollisionRuleId(rows: readonly JsonValue[]): string {
  const used = new Set(rows.map((row) => collisionRuleRecord(row)?.rule_id));
  let suffix = 1;
  while (used.has(`rule_${suffix}`)) suffix += 1;
  return `rule_${suffix}`;
}

/**
 * The rows after setting or clearing the "Everything else" clearance. A new
 * row is the first one; `null` removes the row (motion nodes only).
 */
export function withEverythingClearance(
  rows: readonly JsonValue[],
  clearanceM: number | null,
  leg?: CollisionRuleLeg,
): JsonValue[] {
  const index = everythingRowIndex(rows);
  if (clearanceM === null) return rows.filter((_, at) => at !== index);
  if (index >= 0) {
    const current = collisionRuleRecord(rows[index]) ?? {};
    const next = orderedCollisionRule({ ...current, clearance_m: clearanceM });
    delete next.contact;
    delete next.release;
    return rows.map((row, at) => (at === index ? next : row));
  }
  const used = new Set(rows.map((row) => collisionRuleRecord(row)?.rule_id));
  const ruleId = used.has(EVERYTHING_COLLISION_RULE_ID)
    ? nextCollisionRuleId(rows) : EVERYTHING_COLLISION_RULE_ID;
  return [
    { rule_id: ruleId, ...(leg === undefined ? {} : { leg }), clearance_m: clearanceM },
    ...rows,
  ];
}

/** The clearance of the "Everything else" row; null when the list has none. */
export function everythingClearance(rows: readonly JsonValue[]): number | null {
  const row = collisionRuleRecord(rows[everythingRowIndex(rows)]);
  return row === null ? null : finiteNumber(row.clearance_m);
}

/** Lists the pickers offer; a `null` list is not known yet and its ids are typed. */
export interface CollisionRuleChoices {
  componentIds: readonly string[] | null;
  bodies: readonly { component_id: string; collision_body_id: string }[] | null;
  /** Links of the cell model per component (applied source model); null or absent: typed. */
  links?: readonly { component_id: string; link: string }[] | null;
  /** SRDF groups per component (applied source model, Deployment rules); null or absent: typed. */
  modelGroups?: readonly { component_id: string; group: string }[] | null;
  /** Scene object ids of the Program (move rules). */
  sceneObjectIds?: readonly string[];
  /** Blackboard keys of type scene_resource (move rules). */
  sceneResourceKeys?: readonly string[];
  /** scene_resource fields of structured blackboard keys (move rules). */
  sceneResourceKeyFields?: readonly { key: string; field: string }[];
}

/** A selection of the given kind filled from the first available choice. */
export function defaultCollisionRuleSelection(
  kind: CollisionRuleSelectionKind,
  choices: CollisionRuleChoices,
): JsonRecord {
  const componentId = choices.componentIds?.[0] ?? '';
  switch (kind) {
    case 'component':
    case 'attached_object':
      return { kind, component_id: componentId };
    case 'collision_body': {
      const body = choices.bodies?.[0];
      return {
        kind,
        component_id: body?.component_id ?? componentId,
        collision_body_id: body?.collision_body_id ?? '',
      };
    }
    case 'link':
      return {
        kind,
        component_id: componentId,
        link: choices.links?.find((row) => row.component_id === componentId)?.link ?? '',
      };
    case 'model_group':
      return {
        kind,
        component_id: componentId,
        group: choices.modelGroups?.find((row) => row.component_id === componentId)?.group ?? '',
      };
    case 'scene_object': {
      const objectId = choices.sceneObjectIds?.[0];
      const key = choices.sceneResourceKeys?.[0];
      return objectId !== undefined || key === undefined
        ? { kind, scene_resource_id: objectId ?? '' }
        : { kind, blackboard_key: key };
    }
    default:
      return { kind };
  }
}

/**
 * The rule switched to a spring slider (`spring` true): the first listed body
 * stands in for the slider, side A and every other value go, and the contact
 * keeps its end window. Switched back (`spring` false): the slider goes and
 * side A starts from the move's TCP.
 */
export function withSpringSlider(
  rule: JsonRecord,
  spring: boolean,
  choices: CollisionRuleChoices,
): JsonRecord {
  const next: JsonRecord = { ...rule };
  delete next.clearance_m;
  delete next.release;
  if (!spring) {
    delete next.spring_slider;
    next.a = [defaultCollisionRuleSelection('active_tcp_contact_bodies', choices)];
    next.contact = {};
    return orderedCollisionRule(next);
  }
  const body = choices.bodies?.[0];
  const window = collisionRuleRecord(rule.contact)?.end_from_fraction;
  delete next.a;
  next.spring_slider = {
    component_id: body?.component_id ?? choices.componentIds?.[0] ?? '',
    element_id: body?.collision_body_id ?? '',
  };
  next.contact = window === undefined ? {} : { end_from_fraction: window };
  return orderedCollisionRule(next);
}

/** A new rule row for the scope: the move's TCP (or the first component) against all Scene objects. */
export function newCollisionRule(
  rows: readonly JsonValue[],
  scope: CollisionRuleScope,
  choices: CollisionRuleChoices,
): JsonRecord {
  return orderedCollisionRule({
    rule_id: nextCollisionRuleId(rows),
    ...(scope === 'sequence' ? { leg: 'descend' } : {}),
    a: [defaultCollisionRuleSelection(
      scope === 'deployment' ? 'component' : 'active_tcp_contact_bodies', choices,
    )],
    b: [{ kind: 'scene_objects' }],
    contact: {},
  });
}

/**
 * The collision bodies of the components of a Deployment Draft, from the
 * component catalog: one row per component and collision body id, sorted.
 * A component whose profile the catalog does not describe contributes none.
 */
export function draftCollisionBodies(
  document: Record<string, JsonValue> | null,
  profiles: Readonly<Record<string, ComponentProfileFacts>>,
): Array<{ component_id: string; collision_body_id: string }> {
  const components = document?.components;
  const seen = new Set<string>();
  return (Array.isArray(components) ? components : []).flatMap((value) => {
    const row = collisionRuleRecord(value);
    const componentId = row?.component_id;
    const profileId = row?.component_profile_id;
    if (typeof componentId !== 'string' || typeof profileId !== 'string') return [];
    return (profiles[profileId]?.collisionBodyTargets ?? [])
      .flatMap((target) => target.collisionBodyRoleIds)
      .flatMap((bodyId) => {
        const key = JSON.stringify([componentId, bodyId]);
        if (seen.has(key)) return [];
        seen.add(key);
        return [{ component_id: componentId, collision_body_id: bodyId }];
      });
  }).sort((left, right) => (
    left.component_id.localeCompare(right.component_id)
    || left.collision_body_id.localeCompare(right.collision_body_id)
  ));
}

/**
 * The link and SRDF group choices of the applied cell model: each
 * component's links by their name in the cell model, and the groups its
 * command targets use, by their source name. Both sorted; null while the
 * source model is not known.
 */
export function sourceModelCollisionChoices(source: SourceModelProjection | null): {
  links: Array<{ component_id: string; link: string }> | null;
  modelGroups: Array<{ component_id: string; group: string }> | null;
} {
  if (source === null) return { links: null, modelGroups: null };
  const unique = <T extends { component_id: string }>(rows: T[], key: (row: T) => string) => {
    const seen = new Set<string>();
    return rows.filter((row) => {
      const id = JSON.stringify([row.component_id, key(row)]);
      if (seen.has(id)) return false;
      seen.add(id);
      return true;
    }).sort((left, right) => (
      left.component_id.localeCompare(right.component_id) || key(left).localeCompare(key(right))
    ));
  };
  return {
    links: unique(source.occurrences.flatMap((occurrence) => (
      occurrence.component_id === null ? [] : occurrence.links.map((link) => ({
        component_id: occurrence.component_id as string,
        link: link.viewer_link_name,
      }))
    )), (row) => row.link),
    modelGroups: unique(source.targets.flatMap((target) => target.model_groups.map((group) => ({
      component_id: target.component_id,
      group: group.source_group_name,
    }))), (row) => row.group),
  };
}
