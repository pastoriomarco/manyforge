/** The server hashes the exact authoring body using RFC 8785 canonical JSON. */
function canonical(value: unknown): string {
  if (value === null || typeof value === 'boolean' || typeof value === 'string') return JSON.stringify(value);
  if (typeof value === 'number' && Number.isFinite(value)) return JSON.stringify(value);
  if (Array.isArray(value)) return `[${value.map(canonical).join(',')}]`;
  if (typeof value === 'object') {
    const row = value as Record<string, unknown>;
    return `{${Object.keys(row).sort().map((key) => `${JSON.stringify(key)}:${canonical(row[key])}`).join(',')}}`;
  }
  throw new Error('Authoring document is not canonical JSON');
}

export async function authoringDigest(authoring: unknown): Promise<string> {
  const bytes = new TextEncoder().encode(canonical(authoring));
  const hash = await crypto.subtle.digest('SHA-256', bytes);
  return `sha256:${Array.from(new Uint8Array(hash), (byte) => byte.toString(16).padStart(2, '0')).join('')}`;
}

export interface ReferenceIssue {
  code: string;
  location: { document: 'program'; path: string; node_instance_id: string | null; parameter: string | null };
  payload: Record<string, unknown>;
}

export interface CandidateTarget { component_id: string; command_target_id: string }
export interface CandidateTcpEntry { target: CandidateTarget; frame: { component_id: string; frame_id: string } }
export interface CandidateStopRoute { capability_id: string; control_binding_id: string; control_binding_revision: string; control_route_id: string;
  available: boolean; reason: { code: string; message: string; details: Record<string, string> } | null;
  stages: { stage_id: string; stage_kind: string; command_kind: string; stop_action_class: string }[] }
export interface ReferenceSelectorFacts {
  components: { component_id: string }[];
  targets: { target: CandidateTarget; capabilities: { capability_id: string; available: boolean; reason: string | null }[]; source_joint_names: string[];
    tcp_entries: { entry: CandidateTcpEntry; end_effector: boolean; component_default: boolean;
      mounted_here: boolean; mounted_elsewhere: boolean; available: boolean; reason: string | null }[];
    default_tcp: CandidateTcpEntry | null; stop_routes: CandidateStopRoute[] }[];
  signals: { reference: { component_id: string; signal_name: string }; ambiguous: boolean; target: CandidateTarget | null;
    value_type: string | null; direction: 'input' | 'output' | null; transition_capability: string | null;
    update_mode: string | null; liveness: 'in_process' | 'periodic' | 'change_only' | null; provider_profile: string | null }[];
  frames: { reference: { component_id: string; frame_id: string }; ambiguous: boolean; moving_targets: CandidateTarget[] }[];
  bodies: { reference: { component_id: string; collision_body_id: string }; ambiguous: boolean; moving_targets: CandidateTarget[] }[];
  /** The Deployment's interrupt rules; `scope` and `condition` stay in their normalized Deployment form. */
  rules: { rule_id: string; scope: string | { execution_groups: string[] }; on_trip: 'stop' | 'notify'; active_at_start: boolean; condition: Record<string, unknown> }[];
}

export function referenceResult(value: unknown, digest: string, generation: Record<string, unknown>): { issues: ReferenceIssue[]; total: number; truncated: boolean; selectorFacts: ReferenceSelectorFacts } {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('Invalid reference validation');
  const row = value as Record<string, unknown>;
  const reference = row.reference_validation;
  if (!reference || typeof reference !== 'object' || Array.isArray(reference)) throw new Error('Invalid reference validation');
  const result = reference as Record<string, unknown>;
  const actual = result.workspace_generation;
  if (!actual || typeof actual !== 'object' || Array.isArray(actual)
    || Object.keys(generation).some((key) => (actual as Record<string, unknown>)[key] !== generation[key])
    || Object.keys(actual).length !== Object.keys(generation).length
    || result.program_content_digest !== digest || result.schema_version !== 'manyforge.program_reference_validation_response.v2'
    || !Array.isArray(result.diagnostics) || !Number.isSafeInteger(result.total_count)
    || (result.total_count as number) < result.diagnostics.length || typeof result.truncated !== 'boolean'
    || result.truncated !== ((result.total_count as number) > result.diagnostics.length)) throw new Error('Reference validation capture differs');
  const issues = result.diagnostics.map((raw): ReferenceIssue => {
    if (!raw || typeof raw !== 'object' || Array.isArray(raw)) throw new Error('Invalid reference diagnostic');
    const item = raw as Record<string, unknown>;
    const loc = item.location as Record<string, unknown> | null;
    if (typeof item.code !== 'string' || !loc || typeof loc !== 'object' || loc.document !== 'program'
      || typeof loc.path !== 'string' || !(loc.node_instance_id == null || typeof loc.node_instance_id === 'string')
      || !(loc.parameter == null || typeof loc.parameter === 'string') || !item.payload
      || typeof item.payload !== 'object' || Array.isArray(item.payload)) throw new Error('Invalid reference diagnostic');
    return { code: item.code, location: { document: 'program', path: loc.path,
      node_instance_id: (loc.node_instance_id as string | null | undefined) ?? null,
      parameter: (loc.parameter as string | null | undefined) ?? null }, payload: item.payload as Record<string, unknown> };
  });
  return { issues, total: result.total_count as number, truncated: result.truncated, selectorFacts: selectorFacts(result.selector_facts) };
}

function selectorFacts(value: unknown): ReferenceSelectorFacts {
  const object = (item: unknown, keys: string[]): Record<string, unknown> => {
    if (!item || typeof item !== 'object' || Array.isArray(item)) throw new Error('Invalid selector fact');
    const row = item as Record<string, unknown>;
    if (Object.keys(row).length !== keys.length || keys.some((key) => !(key in row))) throw new Error('Invalid selector fields');
    return row;
  };
  const text = (item: unknown): string => {
    if (typeof item !== 'string' || item.length === 0 || item.includes('\0')) throw new Error('Invalid selector identity');
    return item;
  };
  const nullableText = (item: unknown): string | null => item === null ? null : text(item);
  const list = <T>(item: unknown, parse: (row: unknown) => T): T[] => {
    if (!Array.isArray(item)) throw new Error('Invalid selector rows');
    return item.map(parse);
  };
  const target = (item: unknown): CandidateTarget => {
    const row = object(item, ['component_id', 'command_target_id']);
    return { component_id: text(row.component_id), command_target_id: text(row.command_target_id) };
  };
  const tcp = (item: unknown): CandidateTcpEntry => {
    const row = object(item, ['target', 'frame']);
    const frame = object(row.frame, ['component_id', 'frame_id']);
    return { target: target(row.target), frame: { component_id: text(frame.component_id), frame_id: text(frame.frame_id) } };
  };
  const root = object(value, ['components', 'targets', 'signals', 'frames', 'bodies', 'rules']);
  const components = list(root.components, (item) => { const row = object(item, ['component_id']); return { component_id: text(row.component_id) }; });
  const targets = list(root.targets, (item) => {
    const row = object(item, ['target', 'capabilities', 'source_joint_names', 'tcp_entries', 'default_tcp', 'stop_routes']);
    return { target: target(row.target), source_joint_names: list(row.source_joint_names, text),
      default_tcp: row.default_tcp === null ? null : tcp(row.default_tcp),
      tcp_entries: list(row.tcp_entries, (item2) => {
        const entry = object(item2, ['entry', 'end_effector', 'component_default', 'mounted_here', 'mounted_elsewhere', 'available', 'reason']);
        if (typeof entry.end_effector !== 'boolean' || typeof entry.component_default !== 'boolean'
          || typeof entry.mounted_here !== 'boolean' || typeof entry.mounted_elsewhere !== 'boolean'
          || typeof entry.available !== 'boolean') throw new Error('Invalid TCP marks');
        return { entry: tcp(entry.entry), end_effector: entry.end_effector, component_default: entry.component_default,
          mounted_here: entry.mounted_here, mounted_elsewhere: entry.mounted_elsewhere, available: entry.available, reason: nullableText(entry.reason) };
      }),
      stop_routes: list(row.stop_routes, (item2) => {
        const route = object(item2, ['capability_id', 'control_binding_id', 'control_binding_revision', 'control_route_id', 'stages', 'available', 'reason']);
        if (typeof route.available !== 'boolean') throw new Error('Invalid Stop route availability');
        let reason: CandidateStopRoute['reason'] = null;
        if (route.reason !== null) {
          const refusal = object(route.reason, ['code', 'message', 'details']);
          if (!refusal.details || typeof refusal.details !== 'object' || Array.isArray(refusal.details)
            || Object.values(refusal.details).some((value) => typeof value !== 'string')) throw new Error('Invalid Stop route refusal');
          reason = { code: text(refusal.code), message: text(refusal.message), details: refusal.details as Record<string, string> };
        }
        return { capability_id: text(route.capability_id), control_binding_id: text(route.control_binding_id), control_binding_revision: text(route.control_binding_revision),
          control_route_id: text(route.control_route_id), available: route.available, reason, stages: list(route.stages, (item3) => {
            const stage = object(item3, ['stage_id', 'stage_kind', 'command_kind', 'stop_action_class']);
            return { stage_id: text(stage.stage_id), stage_kind: text(stage.stage_kind), command_kind: text(stage.command_kind), stop_action_class: text(stage.stop_action_class) };
          }) };
      }),
      capabilities: list(row.capabilities, (item2) => {
        const cap = object(item2, ['capability_id', 'available', 'reason']);
        if (typeof cap.available !== 'boolean') throw new Error('Invalid capability availability');
        return { capability_id: text(cap.capability_id), available: cap.available, reason: nullableText(cap.reason) };
      }) };
  });
  const signals = list(root.signals, (item) => {
    const row = object(item, ['reference', 'ambiguous', 'target', 'value_type', 'direction', 'transition_capability', 'update_mode', 'liveness', 'provider_profile']);
    const ref = object(row.reference, ['component_id', 'signal_name']);
    if (typeof row.ambiguous !== 'boolean' || ![null, 'input', 'output'].includes(row.direction as string | null)
      || ![null, 'in_process', 'periodic', 'change_only'].includes(row.liveness as string | null)) throw new Error('Invalid signal selector');
    const result = { reference: { component_id: text(ref.component_id), signal_name: text(ref.signal_name) }, ambiguous: row.ambiguous,
      target: row.target === null ? null : target(row.target), value_type: nullableText(row.value_type),
      direction: row.direction as 'input' | 'output' | null, transition_capability: nullableText(row.transition_capability),
      update_mode: nullableText(row.update_mode), liveness: row.liveness as 'in_process' | 'periodic' | 'change_only' | null,
      provider_profile: nullableText(row.provider_profile) };
    if (result.ambiguous && [result.target, result.value_type, result.direction, result.transition_capability, result.update_mode, result.liveness, result.provider_profile].some((v) => v !== null)) throw new Error('Ambiguous signal has selected facts');
    return result;
  });
  const features = <K extends 'frame_id' | 'collision_body_id'>(item: unknown, key: K) => {
    const row = object(item, ['reference', 'ambiguous', 'moving_targets']);
    const ref = object(row.reference, ['component_id', key]);
    if (typeof row.ambiguous !== 'boolean') throw new Error('Invalid feature selector');
    const moving = list(row.moving_targets, target);
    if (row.ambiguous && moving.length !== 0) throw new Error('Ambiguous feature has selected targets');
    return { reference: { component_id: text(ref.component_id), [key]: text(ref[key]) } as { component_id: string } & Record<K, string>, ambiguous: row.ambiguous, moving_targets: moving };
  };
  const rules = list(root.rules, (item) => {
    const row = object(item, ['rule_id', 'scope', 'on_trip', 'active_at_start', 'condition']);
    const scope = typeof row.scope === 'string' ? row.scope
      : { execution_groups: list(object(row.scope, ['execution_groups']).execution_groups, text) };
    if ((row.on_trip !== 'stop' && row.on_trip !== 'notify') || typeof row.active_at_start !== 'boolean'
      || !row.condition || typeof row.condition !== 'object' || Array.isArray(row.condition)) throw new Error('Invalid rule selector');
    return { rule_id: text(row.rule_id), scope, on_trip: row.on_trip as 'stop' | 'notify', active_at_start: row.active_at_start,
      condition: row.condition as Record<string, unknown> };
  });
  return { components, targets, signals, frames: list(root.frames, (row) => features(row, 'frame_id')),
    bodies: list(root.bodies, (row) => features(row, 'collision_body_id')), rules };
}
