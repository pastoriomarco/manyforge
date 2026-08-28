import { useEffect, useMemo, useState } from 'react';

import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { JsonValue, ProgramAuthoringDocument } from '../types/api';
import { ProgramSupportContactEditor } from './ProgramSupportContactEditor';

const PORTABLE_ID = /^[a-z][a-z0-9_.-]*$/;
const EMPTY_POLICIES: JsonValue[] = [];

interface ContactTargetOption {
  id: string;
  label: string;
  componentRole: string;
  collisionBodyRole: string;
  modelRole?: string;
}

interface ContactPolicyDraft {
  policyId: string;
  zoneId: string;
  targetOptionId: string;
  runtimeObjectKey: string;
  maxPenetrationM: number;
  terminalWindowStartFraction: number;
}

function objectValue(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, JsonValue>
    : null;
}

function stringArray(value: JsonValue | undefined): string[] {
  return Array.isArray(value)
    ? value.filter((item): item is string => typeof item === 'string')
    : [];
}

function targetOptions(program: ProgramAuthoringDocument): ContactTargetOption[] {
  const targets = Object.entries(program.target_roles).flatMap(([targetRole, value]) => {
    const target = objectValue(value);
    if (target === null) return [];
    const componentRole = target.component_role;
    if (typeof componentRole !== 'string') return [];
    return [{
      targetRole,
      componentRole,
      collisionBodyRoles: stringArray(target.required_collision_body_roles),
      modelRoles: stringArray(target.required_model_roles),
    }];
  });
  const options: ContactTargetOption[] = [];
  for (const target of targets) {
    for (const collisionBodyRole of target.collisionBodyRoles) {
      const sameBody = targets.filter((candidate) => (
        candidate.componentRole === target.componentRole
        && candidate.collisionBodyRoles.includes(collisionBodyRole)
      ));
      if (sameBody.length === 1) {
        options.push({
          id: `${target.targetRole}:${collisionBodyRole}`,
          label: `${target.targetRole} · ${collisionBodyRole}`,
          componentRole: target.componentRole,
          collisionBodyRole,
        });
        continue;
      }
      for (const modelRole of target.modelRoles) {
        const sameModel = sameBody.filter((candidate) => (
          candidate.modelRoles.includes(modelRole)
        ));
        if (sameModel.length !== 1) continue;
        options.push({
          id: `${target.targetRole}:${modelRole}:${collisionBodyRole}`,
          label: `${target.targetRole} · ${modelRole} · ${collisionBodyRole}`,
          componentRole: target.componentRole,
          collisionBodyRole,
          modelRole,
        });
      }
    }
  }
  return options.sort((left, right) => left.label.localeCompare(right.label));
}

function runtimeObjectKeys(program: ProgramAuthoringDocument): string[] {
  return (program.blackboard?.keys ?? [])
    .filter((row) => row.type === 'scene_resource')
    .map((row) => row.key)
    .sort();
}

function policyId(value: JsonValue): string | null {
  const row = objectValue(value);
  return typeof row?.policy_id === 'string' ? row.policy_id : null;
}

function fromPolicy(
  value: JsonValue,
  options: ContactTargetOption[],
  keys: string[],
): ContactPolicyDraft | null {
  const policy = objectValue(value);
  const zones = policy?.zones;
  const zone = Array.isArray(zones) ? objectValue(zones[0]) : null;
  const pairs = zone?.body_pairs;
  const pair = Array.isArray(pairs) ? objectValue(pairs[0]) : null;
  const selectors = [objectValue(pair?.body_a), objectValue(pair?.body_b)];
  const component = selectors.find((row) => row?.kind === 'component_model_body');
  const runtime = selectors.find((row) => row?.kind === 'runtime_object');
  const option = options.find((row) => (
    row.componentRole === component?.component_role_id
    && row.collisionBodyRole === component?.collision_body_role_id
    && row.modelRole === component?.model_role_id
  ));
  if (
    typeof policy?.policy_id !== 'string'
    || typeof zone?.zone_id !== 'string'
    || option === undefined
    || typeof runtime?.runtime_object_id !== 'string'
    || !keys.includes(runtime.runtime_object_id)
    || typeof zone.max_penetration_m !== 'number'
    || typeof zone.terminal_window_start_fraction !== 'number'
  ) return null;
  return {
    policyId: policy.policy_id,
    zoneId: zone.zone_id,
    targetOptionId: option.id,
    runtimeObjectKey: runtime.runtime_object_id,
    maxPenetrationM: zone.max_penetration_m,
    terminalWindowStartFraction: zone.terminal_window_start_fraction,
  };
}

function nextPolicyId(existing: Set<string>): string {
  let candidate = 'contact_grasp';
  let suffix = 2;
  while (existing.has(candidate)) candidate = `contact_grasp_${suffix++}`;
  return candidate;
}

function validDraft(
  draft: ContactPolicyDraft,
  options: ContactTargetOption[],
  keys: string[],
): boolean {
  return PORTABLE_ID.test(draft.policyId)
    && PORTABLE_ID.test(draft.zoneId)
    && options.some((option) => option.id === draft.targetOptionId)
    && keys.includes(draft.runtimeObjectKey)
    && Number.isFinite(draft.maxPenetrationM)
    && draft.maxPenetrationM > 0
    && Number.isFinite(draft.terminalWindowStartFraction)
    && draft.terminalWindowStartFraction >= 0
    && draft.terminalWindowStartFraction <= 1;
}

export function ProgramContactPolicyEditor({
  program,
  draftRevision,
  busy,
  mutate,
}: {
  program: ProgramAuthoringDocument;
  draftRevision: number;
  busy: boolean;
  mutate: ProgramWorkspaceController['mutate'];
}) {
  const options = useMemo(() => targetOptions(program), [program]);
  const keys = useMemo(() => runtimeObjectKeys(program), [program]);
  const policies = program.contact_policies ?? EMPTY_POLICIES;
  const [selectedId, setSelectedId] = useState(() => policyId(policies[0] ?? null) ?? '');
  const selected = policies.find((row) => policyId(row) === selectedId) ?? null;
  const empty = useMemo<ContactPolicyDraft>(() => ({
    policyId: nextPolicyId(new Set(policies.map(policyId).filter((id): id is string => id !== null))),
    zoneId: 'tool_contact',
    targetOptionId: options[0]?.id ?? '',
    runtimeObjectKey: keys[0] ?? '',
    maxPenetrationM: 0.002,
    terminalWindowStartFraction: 0.8,
  }), [keys, options, policies]);
  const [draft, setDraft] = useState<ContactPolicyDraft>(() => (
    selected === null ? empty : fromPolicy(selected, options, keys) ?? empty
  ));
  const existing = selected !== null;

  useEffect(() => {
    const current = policies.find((row) => policyId(row) === selectedId) ?? null;
    if (current !== null) setDraft(fromPolicy(current, options, keys) ?? empty);
    else setDraft(empty);
  }, [draftRevision, empty, keys, options, policies, selectedId]);

  const materialize = (next: ContactPolicyDraft) => {
    const target = options.find((option) => option.id === next.targetOptionId);
    if (busy || target === undefined || !validDraft(next, options, keys)) return;
    void mutate('program_draft_upsert_contact_policies', {
      contact_policies: [{
        api_version: 'manyforge.contact_policy.v1',
        policy_id: next.policyId,
        mode: 'contact_grasp',
        sensing_mode: 'geometry_only',
        default_unmatched_response: 'hard_stop',
        zones: [{
          zone_id: next.zoneId,
          body_pairs: [{
            body_a: {
              kind: 'component_model_body',
              component_role_id: target.componentRole,
              collision_body_role_id: target.collisionBodyRole,
              ...(target.modelRole === undefined
                ? {}
                : { model_role_id: target.modelRole }),
            },
            body_b: {
              kind: 'runtime_object',
              runtime_object_id: next.runtimeObjectKey,
            },
          }],
          violation_response: 'hard_stop',
          max_penetration_m: next.maxPenetrationM,
          terminal_window_start_fraction: next.terminalWindowStartFraction,
        }],
      } as unknown as JsonValue],
    }).then((result) => {
      if (!existing && result !== null) setSelectedId(next.policyId);
    });
  };
  const update = (next: ContactPolicyDraft, commit = false) => {
    setDraft(next);
    if (commit) materialize(next);
  };

  return (
    <div className="scene-authoring-column contact-policy-editor">
      <div className="scene-authoring-heading">
        <div>
          <p className="eyebrow">Program Draft</p>
          <h3>Contact policies</h3>
        </div>
        <button
          type="button"
          disabled={busy || options.length === 0 || keys.length === 0}
          onClick={() => {
            setSelectedId('');
            setDraft(empty);
          }}
        >New grasp policy</button>
      </div>
      <p className="muted">
        Policies are authored in this Program Draft. Live contact evidence
        remains read-only logical-scene and Kernel truth.
      </p>
      <ProgramSupportContactEditor
        program={program}
        draftRevision={draftRevision}
        busy={busy}
        mutate={mutate}
      />
      <div className="contact-policy-subheading">
        <strong>Robot/object grasp policy</strong>
        <span>Allowed component-body contact with one runtime object.</span>
      </div>
      <label>
        <span>Policy</span>
        <select
          value={selectedId}
          onChange={(event) => setSelectedId(event.target.value)}
        >
          <option value="">New contact policy</option>
          {policies.map((row) => {
            const id = policyId(row);
            return id === null ? null : <option key={id} value={id}>{id}</option>;
          })}
        </select>
      </label>
      {options.length === 0 || keys.length === 0 ? (
        <p className="scene-commit-error" role="status">
          A contact policy needs one declared collision-body target and one
          scene_resource blackboard key.
        </p>
      ) : (
        <div
          className="contact-policy-fields"
          onKeyDownCapture={(event) => {
            if (event.key === 'Enter' && event.target instanceof HTMLElement) {
              event.preventDefault();
              event.target.blur();
            }
          }}
        >
          <label>
            <span>Policy ID</span>
            <input
              value={draft.policyId}
              disabled={busy || existing}
              pattern="[a-z][a-z0-9_.-]*"
              onChange={(event) => update({ ...draft, policyId: event.target.value })}
            />
          </label>
          <label>
            <span>Zone ID</span>
            <input
              value={draft.zoneId}
              disabled={busy}
              pattern="[a-z][a-z0-9_.-]*"
              onChange={(event) => update({ ...draft, zoneId: event.target.value })}
              onBlur={() => existing && materialize(draft)}
            />
          </label>
          <label>
            <span>Component target and collision body</span>
            <select
              value={draft.targetOptionId}
              disabled={busy}
              onChange={(event) => update(
                { ...draft, targetOptionId: event.target.value }, existing,
              )}
            >
              {options.map((option) => (
                <option key={option.id} value={option.id}>{option.label}</option>
              ))}
            </select>
          </label>
          <label>
            <span>Runtime object key</span>
            <select
              value={draft.runtimeObjectKey}
              disabled={busy}
              onChange={(event) => update(
                { ...draft, runtimeObjectKey: event.target.value }, existing,
              )}
            >
              {keys.map((key) => <option key={key} value={key}>{key}</option>)}
            </select>
          </label>
          <label>
            <span>Maximum penetration (m)</span>
            <input
              type="number"
              min={0.000001}
              step={0.0001}
              value={Number.isFinite(draft.maxPenetrationM)
                ? draft.maxPenetrationM
                : ''}
              disabled={busy}
              onChange={(event) => update({
                ...draft,
                maxPenetrationM: event.currentTarget.valueAsNumber,
              })}
              onBlur={() => existing && materialize(draft)}
            />
          </label>
          <label>
            <span>Terminal window start</span>
            <input
              type="number"
              min={0}
              max={1}
              step={0.05}
              value={Number.isFinite(draft.terminalWindowStartFraction)
                ? draft.terminalWindowStartFraction
                : ''}
              disabled={busy}
              onChange={(event) => update({
                ...draft,
                terminalWindowStartFraction: event.currentTarget.valueAsNumber,
              })}
              onBlur={() => existing && materialize(draft)}
            />
          </label>
          <div className="scene-authoring-actions">
            {!existing && (
              <button
                type="button"
                disabled={busy || !validDraft(draft, options, keys)}
                onClick={() => materialize(draft)}
              >Add contact policy</button>
            )}
            {existing && (
              <button
                type="button"
                className="danger-button"
                disabled={busy}
                onClick={() => void mutate(
                  'program_draft_remove_contact_policies',
                  { contact_policy_ids: [draft.policyId] },
                ).then((result) => {
                  if (result !== null) setSelectedId('');
                })}
              >Remove policy</button>
            )}
          </div>
        </div>
      )}
    </div>
  );
}
