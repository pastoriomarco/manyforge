import { useEffect, useMemo, useState } from 'react';

import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { JsonValue, ProgramAuthoringDocument } from '../types/api';
import { groupByNamespace } from '../utils/blackboardNamespaces';
import { NamespaceGroupOptions, useNamespaceGroups } from './common/NamespaceGroupOptions';
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

type ContactMode = 'contact_grasp' | 'contact_release';
type ReleaseEvidence = 'signed_penetration' | 'binary_contact';

/** Spec 250 §3.2 release envelope (signed defaults; binary keeps only the last two). */
interface ReleaseEnvelope {
  evidence: ReleaseEvidence;
  enforceMaxStartPenetration: boolean;
  maxStartPenetrationM: number;
  minReleaseProgressM: number;
  releaseToleranceM: number;
  requireStrictlyDecreasingPenetration: boolean;
  requireSeparationByEnd: boolean;
  allowRecontact: boolean;
}

const SIGNED_RELEASE_DEFAULTS: ReleaseEnvelope = {
  evidence: 'signed_penetration',
  enforceMaxStartPenetration: true,
  maxStartPenetrationM: 0,
  minReleaseProgressM: 0,
  releaseToleranceM: 0.00001,
  requireStrictlyDecreasingPenetration: true,
  requireSeparationByEnd: true,
  allowRecontact: false,
};

interface ContactPolicyDraft {
  policyId: string;
  zoneId: string;
  mode: ContactMode;
  bodySource: 'component_model_body' | 'motion_endpoint';
  targetOptionId: string;
  runtimeObjectKey: string;
  maxPenetrationM: number;
  terminalWindowStartFraction: number;
  release: ReleaseEnvelope;
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

function numberOr(value: JsonValue | undefined, fallback: number): number | null {
  if (value === undefined) return fallback;
  return typeof value === 'number' ? value : null;
}

function booleanOr(value: JsonValue | undefined, fallback: boolean): boolean | null {
  if (value === undefined) return fallback;
  return typeof value === 'boolean' ? value : null;
}

/** The authored release envelope with spec defaults for omitted fields. */
function releaseFromZone(zone: Record<string, JsonValue>): ReleaseEnvelope | null {
  const evidence = zone.release_evidence_mode;
  if (evidence !== 'signed_penetration' && evidence !== 'binary_contact') return null;
  const signed = evidence === 'signed_penetration';
  const envelope = {
    evidence,
    enforceMaxStartPenetration: booleanOr(zone.enforce_max_start_penetration, signed),
    maxStartPenetrationM: numberOr(zone.max_start_penetration_m, 0),
    minReleaseProgressM: numberOr(zone.min_release_progress_m, 0),
    releaseToleranceM: numberOr(zone.release_tolerance_m, signed ? SIGNED_RELEASE_DEFAULTS.releaseToleranceM : 0),
    requireStrictlyDecreasingPenetration: booleanOr(zone.require_strictly_decreasing_penetration, signed),
    requireSeparationByEnd: booleanOr(zone.require_separation_by_end, true),
    allowRecontact: booleanOr(zone.allow_recontact, false),
  };
  return Object.values(envelope).some((value) => value === null) ? null : envelope as ReleaseEnvelope;
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
  const endpoint = selectors.find(
    (row): row is Record<string, JsonValue> => row?.kind === 'motion_endpoint',
  );
  const component = selectors.find((row) => row?.kind === 'component_model_body');
  const runtime = selectors.find((row) => row?.kind === 'runtime_object');
  const option = options.find((row) => (
    row.componentRole === component?.component_role_id
    && row.collisionBodyRole === component?.collision_body_role_id
    && row.modelRole === component?.model_role_id
  ));
  const mode = policy?.mode;
  if (
    typeof policy?.policy_id !== 'string'
    || typeof zone?.zone_id !== 'string'
    || (mode !== 'contact_grasp' && mode !== 'contact_release')
    || (endpoint === undefined && option === undefined)
    || (endpoint !== undefined && Object.keys(endpoint).length !== 1)
    || (endpoint !== undefined && component !== undefined)
    || typeof runtime?.runtime_object_id !== 'string'
    || !keys.includes(runtime.runtime_object_id)
  ) return null;
  const release = mode === 'contact_release' ? releaseFromZone(zone) : SIGNED_RELEASE_DEFAULTS;
  if (
    release === null
    || (mode === 'contact_grasp' && (
      typeof zone.max_penetration_m !== 'number'
      || typeof zone.terminal_window_start_fraction !== 'number'))
  ) return null;
  return {
    policyId: policy.policy_id,
    zoneId: zone.zone_id,
    mode,
    bodySource: endpoint === undefined ? 'component_model_body' : 'motion_endpoint',
    targetOptionId: option?.id ?? '',
    runtimeObjectKey: runtime.runtime_object_id,
    // A release policy keeps the grasp defaults for a later mode switch only.
    maxPenetrationM: typeof zone.max_penetration_m === 'number' ? zone.max_penetration_m : 0.002,
    terminalWindowStartFraction: typeof zone.terminal_window_start_fraction === 'number'
      ? zone.terminal_window_start_fraction : 0.8,
    release,
  };
}

function nextPolicyId(existing: Set<string>): string {
  let candidate = 'contact_policy';
  let suffix = 2;
  while (existing.has(candidate)) candidate = `contact_policy_${suffix++}`;
  return candidate;
}

const RELEASE_METRIC_FIELDS = [
  ['maxStartPenetrationM', 'Maximum starting penetration (m)'],
  ['minReleaseProgressM', 'Minimum release progress (m)'],
  ['releaseToleranceM', 'Release tolerance (m)'],
] as const;

function metricValid(value: number): boolean {
  return Number.isFinite(value) && value >= 0 && value <= 1000;
}

function envelopeValid(draft: ContactPolicyDraft): boolean {
  if (draft.mode === 'contact_grasp') {
    return Number.isFinite(draft.maxPenetrationM)
      && draft.maxPenetrationM > 0
      && Number.isFinite(draft.terminalWindowStartFraction)
      && draft.terminalWindowStartFraction >= 0
      && draft.terminalWindowStartFraction <= 1;
  }
  const release = draft.release;
  return release.evidence === 'binary_contact' || (
    metricValid(release.maxStartPenetrationM)
    && metricValid(release.minReleaseProgressM)
    && metricValid(release.releaseToleranceM)
  );
}

/** The closed zone envelope of the selected mode; binary release writes no metrics. */
function zoneEnvelope(draft: ContactPolicyDraft): Record<string, JsonValue> {
  if (draft.mode === 'contact_grasp') {
    return {
      max_penetration_m: draft.maxPenetrationM,
      terminal_window_start_fraction: draft.terminalWindowStartFraction,
    };
  }
  const release = draft.release;
  return release.evidence === 'binary_contact' ? {
    release_evidence_mode: 'binary_contact',
    require_separation_by_end: release.requireSeparationByEnd,
    allow_recontact: release.allowRecontact,
  } : {
    release_evidence_mode: 'signed_penetration',
    enforce_max_start_penetration: release.enforceMaxStartPenetration,
    max_start_penetration_m: release.maxStartPenetrationM,
    min_release_progress_m: release.minReleaseProgressM,
    release_tolerance_m: release.releaseToleranceM,
    require_strictly_decreasing_penetration: release.requireStrictlyDecreasingPenetration,
    require_separation_by_end: release.requireSeparationByEnd,
    allow_recontact: release.allowRecontact,
  };
}

function validDraft(
  draft: ContactPolicyDraft,
  options: ContactTargetOption[],
  keys: string[],
): boolean {
  return PORTABLE_ID.test(draft.policyId)
    && PORTABLE_ID.test(draft.zoneId)
    && (draft.bodySource === 'motion_endpoint'
      || options.some((option) => option.id === draft.targetOptionId))
    && keys.includes(draft.runtimeObjectKey)
    && envelopeValid(draft);
}

export function ProgramContactPolicyEditor({
  program,
  draftRevision,
  busy,
  busyReason = null,
  mutate,
}: {
  program: ProgramAuthoringDocument;
  draftRevision: number;
  busy: boolean;
  busyReason?: string | null;
  mutate: ProgramWorkspaceController['mutate'];
}) {
  const blockedTitle = busy ? busyReason ?? undefined : undefined;
  const options = useMemo(() => targetOptions(program), [program]);
  const keys = useMemo(() => runtimeObjectKeys(program), [program]);
  const keyNamespaces = useNamespaceGroups();
  const policies = program.contact_policies ?? EMPTY_POLICIES;
  const [selectedId, setSelectedId] = useState(() => policyId(policies[0] ?? null) ?? '');
  const selected = policies.find((row) => policyId(row) === selectedId) ?? null;
  const empty = useMemo<ContactPolicyDraft>(() => ({
    policyId: nextPolicyId(new Set(policies.map(policyId).filter((id): id is string => id !== null))),
    zoneId: 'tool_contact',
    mode: 'contact_grasp',
    bodySource: 'component_model_body',
    targetOptionId: options[0]?.id ?? '',
    runtimeObjectKey: keys[0] ?? '',
    maxPenetrationM: 0.002,
    terminalWindowStartFraction: 0.8,
    release: SIGNED_RELEASE_DEFAULTS,
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
    if (
      busy
      || !validDraft(next, options, keys)
      || (next.bodySource === 'component_model_body' && target === undefined)
    ) return;
    const contactBody = next.bodySource === 'motion_endpoint'
      ? { kind: 'motion_endpoint' }
      : {
        kind: 'component_model_body',
        component_role_id: target!.componentRole,
        collision_body_role_id: target!.collisionBodyRole,
        ...(target!.modelRole === undefined ? {} : { model_role_id: target!.modelRole }),
      };
    void mutate('program_draft_upsert_contact_policies', {
      contact_policies: [{
        api_version: 'manyforge.contact_policy.v1',
        policy_id: next.policyId,
        mode: next.mode,
        sensing_mode: 'geometry_only',
        default_unmatched_response: 'hard_stop',
        zones: [{
          zone_id: next.zoneId,
          body_pairs: [{
            body_a: contactBody,
            body_b: {
              kind: 'runtime_object',
              runtime_object_id: next.runtimeObjectKey,
            },
          }],
          violation_response: 'hard_stop',
          ...zoneEnvelope(next),
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
    <div className="scene-authoring-column dpanel" aria-label="Contact policies">
      <ProgramSupportContactEditor
        program={program}
        draftRevision={draftRevision}
        busy={busy}
        busyReason={busyReason}
        mutate={mutate}
      />
      <section className="dcard">
      <header>
        <span>Robot/object contact policy</span>
        <button
          type="button"
          disabled={busy || keys.length === 0}
          title={blockedTitle}
          onClick={() => {
            setSelectedId('');
            setDraft(empty);
          }}
        >New contact policy</button>
      </header>
      <p>
        Allowed contact with one runtime object. Choose explicit component bodies
        or the selected motion endpoint’s associated bodies. Grasp allows contact
        that starts during the move; Release lets a move start in contact and
        must separate from the object.
      </p>
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
      {keys.length === 0 ? (
        <p className="scene-commit-error inline-message severity-error" role="status">
          A contact policy needs one scene_resource blackboard key.
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
              title={blockedTitle}
              pattern="[a-z][a-z0-9_.-]*"
              onChange={(event) => update({ ...draft, policyId: event.target.value })}
            />
          </label>
          <label>
            <span>Zone ID</span>
            <input
              value={draft.zoneId}
              disabled={busy}
              title={blockedTitle}
              pattern="[a-z][a-z0-9_.-]*"
              onChange={(event) => update({ ...draft, zoneId: event.target.value })}
              onBlur={() => existing && materialize(draft)}
            />
          </label>
          <label>
            <span>Contact kind</span>
            <select
              value={draft.mode}
              disabled={busy}
              title={blockedTitle}
              onChange={(event) => update({
                ...draft,
                mode: event.target.value as ContactMode,
              }, existing)}
            >
              <option value="contact_grasp">Grasp (contact starts during the move)</option>
              <option value="contact_release">Release (the move starts in contact)</option>
            </select>
          </label>
          <label>
            <span>Contact body source</span>
            <select
              value={draft.bodySource}
              disabled={busy}
              title={blockedTitle}
              onChange={(event) => update({
                ...draft,
                bodySource: event.target.value as ContactPolicyDraft['bodySource'],
              }, existing)}
            >
              <option value="component_model_body">Explicit component body</option>
              <option value="motion_endpoint">Selected motion endpoint</option>
            </select>
          </label>
          {draft.bodySource === 'component_model_body' ? (
            options.length === 0 ? (
              <p className="scene-commit-error inline-message severity-error" role="status">
                This Program has no declared component collision bodies. Choose the selected motion endpoint if its modeled frame exposes contact bodies.
              </p>
            ) : (
              <label>
                <span>Component target and collision body</span>
                <select
                  value={draft.targetOptionId}
                  disabled={busy}
                  title={blockedTitle}
                  onChange={(event) => update(
                    { ...draft, targetOptionId: event.target.value }, existing,
                  )}
                >
                  {options.map((option) => (
                    <option key={option.id} value={option.id}>{option.label}</option>
                  ))}
                </select>
              </label>
            )
          ) : (
            <p className="program-form-hint">
              The operation uses only contact bodies associated with its selected endpoint. Selecting an endpoint alone grants no contact permission.
            </p>
          )}
          <label>
            <span>Runtime object key</span>
            <select
              value={draft.runtimeObjectKey}
              disabled={busy}
              title={blockedTitle}
              onChange={(event) => {
                if (keyNamespaces.handleToggle(event.target.value)) return;
                update({ ...draft, runtimeObjectKey: event.target.value }, existing);
              }}
            >
              <NamespaceGroupOptions
                groups={groupByNamespace(keys, (key) => key)}
                expanded={keyNamespaces.expanded}
                current={draft.runtimeObjectKey}
                valueOf={(key) => key}
                option={(key) => <option key={key} value={key}>{key}</option>}
              />
            </select>
          </label>
          {draft.mode === 'contact_grasp' && (
            <>
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
              title={blockedTitle}
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
              title={blockedTitle}
              onChange={(event) => update({
                ...draft,
                terminalWindowStartFraction: event.currentTarget.valueAsNumber,
              })}
              onBlur={() => existing && materialize(draft)}
            />
          </label>
            </>
          )}
          {draft.mode === 'contact_release' && (
            <>
              <label>
                <span>Release evidence</span>
                <select
                  value={draft.release.evidence}
                  disabled={busy}
                  title={blockedTitle}
                  onChange={(event) => update({
                    ...draft,
                    release: event.target.value === 'binary_contact'
                      ? { ...draft.release, evidence: 'binary_contact' }
                      : { ...SIGNED_RELEASE_DEFAULTS,
                        requireSeparationByEnd: draft.release.requireSeparationByEnd,
                        allowRecontact: draft.release.allowRecontact },
                  }, existing)}
                >
                  <option value="signed_penetration">Signed penetration</option>
                  <option value="binary_contact">Binary contact</option>
                </select>
              </label>
              {draft.release.evidence === 'signed_penetration' && (
                <>
                  <label>
                    <input
                      type="checkbox"
                      checked={draft.release.enforceMaxStartPenetration}
                      disabled={busy}
                      title={blockedTitle}
                      onChange={(event) => update({ ...draft, release: {
                        ...draft.release, enforceMaxStartPenetration: event.target.checked,
                      } }, existing)}
                    />
                    <span>Limit the starting penetration</span>
                  </label>
                  {RELEASE_METRIC_FIELDS.map(([field, label]) => (
                    <label key={field}>
                      <span>{label}</span>
                      <input
                        type="number"
                        min={0}
                        max={1000}
                        step={0.0001}
                        value={Number.isFinite(draft.release[field]) ? draft.release[field] : ''}
                        disabled={busy}
                        title={blockedTitle}
                        onChange={(event) => update({ ...draft, release: {
                          ...draft.release, [field]: event.currentTarget.valueAsNumber,
                        } })}
                        onBlur={() => existing && materialize(draft)}
                      />
                    </label>
                  ))}
                  <label>
                    <input
                      type="checkbox"
                      checked={draft.release.requireStrictlyDecreasingPenetration}
                      disabled={busy}
                      title={blockedTitle}
                      onChange={(event) => update({ ...draft, release: {
                        ...draft.release, requireStrictlyDecreasingPenetration: event.target.checked,
                      } }, existing)}
                    />
                    <span>Penetration must strictly decrease</span>
                  </label>
                </>
              )}
              <label>
                <input
                  type="checkbox"
                  checked={draft.release.requireSeparationByEnd}
                  disabled={busy}
                  title={blockedTitle}
                  onChange={(event) => update({ ...draft, release: {
                    ...draft.release, requireSeparationByEnd: event.target.checked,
                  } }, existing)}
                />
                <span>Must be separated by the end of the move</span>
              </label>
              <label>
                <input
                  type="checkbox"
                  checked={draft.release.allowRecontact}
                  disabled={busy}
                  title={blockedTitle}
                  onChange={(event) => update({ ...draft, release: {
                    ...draft.release, allowRecontact: event.target.checked,
                  } }, existing)}
                />
                <span>Allow contact again after separating</span>
              </label>
            </>
          )}
          <div className="dactions">
            {!existing && (
              <button
                type="button"
                disabled={busy || !validDraft(draft, options, keys)}
                title={blockedTitle}
                onClick={() => materialize(draft)}
              >Add contact policy</button>
            )}
            {existing && (
              <button
                type="button"
                className="danger-button"
                disabled={busy}
                title={blockedTitle}
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
      </section>
    </div>
  );
}
