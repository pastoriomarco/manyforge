import type { JsonValue } from '../types/api';

/**
 * A Tool made from its parameters (`manyforge.tool`, OD-357): one component,
 * mounted as a whole. Chains of fixed elements lead from its body to its tool
 * ends (jaw pair, spring slider, probe tip); each TCP belongs to a tool end or
 * the body. Its parameters live inline in its Draft row
 * (`model_source.parameters`); the Deployment holds only the wiring of each
 * actuated tool end (a jaw pair): one `gripper_target_configurations` row
 * `{controllable_component_id, command_target_id, pneumatic_io}`.
 */
export const GENERATED_TOOL_PROFILE_ID = 'manyforge.tool';
export const ELEMENT_KINDS = ['fixed', 'jaw_pair', 'spring_slider', 'tip'] as const;
export type ElementKind = typeof ELEMENT_KINDS[number];
export const GEOMETRY_KINDS = ['box', 'cylinder', 'sphere', 'mesh'] as const;
export type GeometryKind = typeof GEOMETRY_KINDS[number];
export const ELEMENT_KIND_LABELS: Record<ElementKind, string> = {
  fixed: 'Fixed body', jaw_pair: 'Jaw pair', spring_slider: 'Spring slider', tip: 'Probe tip',
};

export type JsonObject = Record<string, JsonValue>;

export function object(value: JsonValue | undefined): JsonObject | null {
  return value !== null && value !== undefined && typeof value === 'object' && !Array.isArray(value)
    ? value : null;
}

export function list(value: JsonValue | undefined): JsonValue[] {
  return Array.isArray(value) ? value : [];
}

export interface GeneratedToolRow {
  index: number;
  componentId: string;
  /** The inline parameters; null when the row names a parameter file. */
  parameters: JsonObject | null;
  /** The parameter file's path when the row names one. */
  reference: string | null;
  /** What the row holds in `model_source.parameters`, as authored. */
  source: JsonValue;
  wiring: JsonObject[];
}

/** The Draft row of a Tool made from its parameters, or null. */
export function generatedToolRow(
  document: Record<string, JsonValue> | null, componentId: string,
): GeneratedToolRow | null {
  const components = list(document?.components);
  const index = components.findIndex((row) => object(row)?.component_id === componentId);
  const row = object(components[index]);
  if (row === null || row.component_profile_id !== GENERATED_TOOL_PROFILE_ID) return null;
  const source = object(row.model_source)?.parameters ?? null;
  const inline = object(source);
  const isInline = inline !== null && 'api_version' in inline;
  const reference = isInline ? null
    : String(object(source)?.project_path ?? object(source)?.external_path ?? '');
  const runtime = object(object(row.configuration)?.runtime_configuration);
  return {
    index,
    componentId,
    parameters: isInline ? inline : null,
    reference,
    source,
    wiring: list(runtime?.gripper_target_configurations).flatMap((item) => {
      const wired = object(item);
      return wired === null ? [] : [wired];
    }),
  };
}

/** The ids of the Draft's Tools made from parameters. */
export function generatedToolIds(document: Record<string, JsonValue> | null): Set<string> {
  return new Set(list(document?.components).flatMap((row) => {
    const value = object(row);
    return value?.component_profile_id === GENERATED_TOOL_PROFILE_ID
      && typeof value.component_id === 'string' ? [value.component_id] : [];
  }));
}

export function parametersPath(index: number): Array<string | number> {
  return ['components', index, 'model_source', 'parameters'];
}

export function wiringPath(index: number): Array<string | number> {
  return ['components', index, 'configuration', 'runtime_configuration', 'gripper_target_configurations'];
}

/** A field's path as the backend names it in a refusal, e.g. `chains[0].elements[1].jaw_pair.max_aperture_m`. */
export function fieldPath(...parts: Array<string | number>): string {
  return parts.reduce<string>((path, part) => (
    typeof part === 'number' ? `${path}[${part}]` : path === '' ? part : `${path}.${part}`
  ), '');
}

/** Whether a refusal's field is this field or inside it. */
export function refusalAt(refused: string | null | undefined, field: string): boolean {
  if (!refused) return false;
  return refused === field || refused.startsWith(`${field}.`) || refused.startsWith(`${field}[`);
}

export interface ToolElement {
  chain: number;
  element: number;
  kind: ElementKind;
  id: string;
  fields: JsonObject;
}

export function elements(parameters: JsonObject | null): ToolElement[] {
  return list(parameters?.chains).flatMap((chain, chainIndex) => (
    list(object(chain)?.elements).flatMap((value, elementIndex) => {
      const row = object(value);
      const kind = row === null ? undefined : Object.keys(row)[0];
      const fields = kind === undefined ? null : object(row?.[kind]);
      return fields === null || !(ELEMENT_KINDS as readonly string[]).includes(kind as string) ? [] : [{
        chain: chainIndex, element: elementIndex, kind: kind as ElementKind,
        id: typeof fields.id === 'string' ? fields.id : '', fields,
      }];
    })
  ));
}

/** The tool ends (jaw pairs, spring sliders, tips): what a TCP may belong to. */
export function toolEnds(parameters: JsonObject | null): ToolElement[] {
  return elements(parameters).filter((element) => element.kind !== 'fixed');
}

/** The actuated tool ends: one wiring row each. */
export function jawPairs(parameters: JsonObject | null): ToolElement[] {
  return elements(parameters).filter((element) => element.kind === 'jaw_pair');
}

/** A TCP's contact bodies when it names none (as Load derives them). */
export function defaultContactBodies(parameters: JsonObject | null, attachTo: string): string[] {
  const end = toolEnds(parameters).find((element) => element.id === attachTo);
  if (end === undefined) return [];
  if (end.kind !== 'jaw_pair') return [end.id];
  return [object(end.fields.pads) !== null ? `${end.id}_pads` : `${end.id}_fingers`];
}

export function uniqueId(base: string, taken: Iterable<string>): string {
  const used = new Set(taken);
  if (!used.has(base)) return base;
  for (let index = 2; ; index += 1) if (!used.has(`${base}_${index}`)) return `${base}_${index}`;
}

export function newPart(name: string): JsonObject {
  return { name, collision: { box: { size_m: [0.02, 0.02, 0.02] } } };
}

/**
 * A picked file's path relative to the folder the mesh paths are relative to
 * (both absolute): what the mesh picker writes, `..` where it leaves the
 * folder (OD-364).
 */
export function relativeMeshPath(folder: string, path: string): string {
  const parts = (value: string) => value.split('/').filter((part) => part !== '');
  const base = parts(folder);
  const target = parts(path);
  let common = 0;
  while (common < base.length && common < target.length && base[common] === target[common]) common += 1;
  return [...base.slice(common).map(() => '..'), ...target.slice(common)].join('/');
}

export function newGeometry(kind: GeometryKind): JsonObject {
  if (kind === 'box') return { box: { size_m: [0.02, 0.02, 0.02] } };
  if (kind === 'cylinder') return { cylinder: { radius_m: 0.01, length_m: 0.02 } };
  if (kind === 'sphere') return { sphere: { radius_m: 0.01 } };
  return { mesh: { path: 'meshes/part.stl' } };
}

/** A new element of a kind, with the fields Load requires. */
export function newElement(kind: ElementKind, id: string, partNames: Iterable<string>): JsonObject {
  const part = newPart(uniqueId(`${id}_part`, partNames));
  if (kind === 'jaw_pair') {
    return { jaw_pair: {
      id, positive_jaw: [{ ...part, name: uniqueId(`${id}_jaw`, partNames) }],
      min_aperture_m: 0, max_aperture_m: 0.04,
    } };
  }
  if (kind === 'spring_slider') {
    return { spring_slider: { id, parts: [part], axis: [0, 0, 1], stroke_m: 0.005, trip_depth_m: 0.001 } };
  }
  return { [kind]: { id, parts: [part] } };
}

/**
 * The release depth a spring slider's "Set release depth" starts from: half
 * its trip depth (any value above 0 and below the trip depth is allowed).
 */
export function releaseDepthStart(tripDepth: JsonValue | undefined): number {
  return typeof tripDepth === 'number' && tripDepth > 0 ? tripDepth / 2 : 0.0005;
}

/** Every part name of the parameters (they are unique in the file). */
export function partNames(parameters: JsonObject | null): string[] {
  const names = (parts: JsonValue | undefined) => list(parts).flatMap((part) => {
    const name = object(part)?.name;
    return typeof name === 'string' ? [name] : [];
  });
  return [
    ...names(parameters?.body),
    ...elements(parameters).flatMap((element) => [
      ...names(element.fields.parts), ...names(element.fields.positive_jaw),
      ...names(element.fields.negative_jaw), ...names(object(element.fields.pads)?.positive),
      ...names(object(element.fields.pads)?.negative),
    ]),
  ];
}

/** A copy of `value` with `path` set (`undefined` removes it). */
export function withValue(value: JsonValue, path: Array<string | number>, next: JsonValue | undefined): JsonValue {
  if (path.length === 0) return next === undefined ? null : next;
  const [head, ...rest] = path;
  if (Array.isArray(value) && typeof head === 'number') {
    const copy = [...value];
    if (rest.length === 0 && next === undefined) copy.splice(head, 1);
    else copy[head] = withValue(copy[head] ?? {}, rest, next);
    return copy;
  }
  const record = { ...(object(value) ?? {}) };
  if (rest.length === 0 && next === undefined) delete record[head as string];
  else record[head as string] = withValue(record[head as string] ?? (typeof rest[0] === 'number' ? [] : {}), rest, next);
  return record;
}

/** The wiring rows with one tool end's `pneumatic_io` set, or its row removed (`undefined`). */
export function withWiring(
  rows: JsonObject[], componentId: string, endId: string, pneumaticIo: JsonValue | undefined,
): JsonObject[] {
  const others = rows.filter((row) => row.command_target_id !== endId);
  if (pneumaticIo === undefined) return others;
  const row: JsonObject = { controllable_component_id: componentId, command_target_id: endId, pneumatic_io: pneumaticIo };
  const existing = rows.find((candidate) => candidate.command_target_id === endId);
  if (existing?.grasp_confirmation !== undefined) row.grasp_confirmation = existing.grasp_confirmation;
  return [...others, row].sort((left, right) => String(left.command_target_id).localeCompare(String(right.command_target_id)));
}

/**
 * The wiring rows after a parameters edit: a renamed jaw pair keeps its row
 * under its new id, a removed one loses it. Null when nothing changes.
 */
export function followedWiring(
  rows: JsonObject[], before: JsonObject | null, after: JsonObject | null,
): JsonObject[] | null {
  const old = jawPairs(before);
  const next = jawPairs(after);
  const renamed = new Map<string, string>();
  for (const pair of old) {
    const moved = next.find((candidate) => candidate.chain === pair.chain && candidate.element === pair.element
      && candidate.kind === 'jaw_pair');
    if (moved !== undefined && moved.id !== pair.id && !old.some((other) => other.id === moved.id)) {
      renamed.set(pair.id, moved.id);
    }
  }
  const ids = new Set(next.map((pair) => pair.id));
  const result = rows.flatMap((row) => {
    const id = String(row.command_target_id);
    const target = renamed.get(id) ?? id;
    return ids.has(target) ? [target === id ? row : { ...row, command_target_id: target }] : [];
  });
  return JSON.stringify(result) === JSON.stringify(rows) ? null : result;
}

/** A pose's position and roll/pitch/yaw (degrees); an xyzw orientation is shown as such. */
export function poseFields(pose: JsonValue | undefined): {
  position: [number, number, number]; rpy: [number, number, number] | null;
} {
  const row = object(pose);
  const position = list(row?.position_m).map(Number);
  const orientation = object(row?.orientation);
  const rpy = orientation === null ? [0, 0, 0]
    : Array.isArray(orientation.rpy_deg) ? orientation.rpy_deg.map(Number) : null;
  return {
    position: position.length === 3 ? position as [number, number, number] : [0, 0, 0],
    rpy: rpy === null ? null : rpy as [number, number, number],
  };
}

/** A pose value; identity parts are left out, as in the example files. */
export function poseValue(position: [number, number, number], rpy: [number, number, number]): JsonObject | undefined {
  const value: JsonObject = {};
  if (position.some((item) => item !== 0)) value.position_m = position;
  if (rpy.some((item) => item !== 0)) value.orientation = { rpy_deg: rpy };
  return Object.keys(value).length === 0 ? undefined : value;
}
