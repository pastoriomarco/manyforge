import { useEffect, useMemo, useState } from 'react';

import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type {
  JsonValue,
  ProgramAuthoringDocument,
  ProgramSceneResource,
} from '../types/api';

type ContactModality =
  | 'signed_penetration'
  | 'binary_contact'
  | 'bounded_penetration'
  | 'contact_process';

interface SupportContactDraft {
  supportedObjectId: string;
  supportObjectId: string;
  modality: ContactModality;
  enforceMaxStart: boolean;
  maxStartPenetrationM: number;
  releaseToleranceM: number;
  minReleaseProgressM: number;
  maxContactPenetrationM: number;
  requireSeparationByEnd: boolean;
  requireDecreasingPenetration: boolean;
  allowRecontact: boolean;
}

function record(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, JsonValue>
    : null;
}

function finiteNumber(value: JsonValue | undefined, fallback: number): number {
  return typeof value === 'number' && Number.isFinite(value) ? value : fallback;
}

function boolean(value: JsonValue | undefined, fallback: boolean): boolean {
  return typeof value === 'boolean' ? value : fallback;
}

function rawContacts(resource: ProgramSceneResource): Record<string, JsonValue>[] {
  const contacts = resource.metadata?.support_contacts;
  return Array.isArray(contacts)
    ? contacts.flatMap((value) => {
        const row = record(value);
        return row === null ? [] : [row];
      })
    : [];
}

function draftFromContact(
  supportedObjectId: string,
  contact: Record<string, JsonValue>,
): SupportContactDraft | null {
  const supportObjectId = contact.support_object_id;
  if (typeof supportObjectId !== 'string') return null;
  const mode = contact.mode;
  const modality: ContactModality = mode === 'bounded_penetration'
    ? 'bounded_penetration'
    : mode === 'contact_process'
      ? 'contact_process'
      : contact.release_evidence_mode === 'binary_contact'
        ? 'binary_contact'
        : 'signed_penetration';
  return {
    supportedObjectId,
    supportObjectId,
    modality,
    enforceMaxStart: boolean(contact.enforce_max_start_penetration, true),
    maxStartPenetrationM: finiteNumber(contact.max_start_penetration_m, 0),
    releaseToleranceM: finiteNumber(contact.release_tolerance_m, 0.00001),
    minReleaseProgressM: finiteNumber(contact.min_release_progress_m, 0),
    maxContactPenetrationM: finiteNumber(contact.max_contact_penetration_m, 0),
    requireSeparationByEnd: boolean(contact.require_separation_by_end, true),
    requireDecreasingPenetration: boolean(
      contact.require_strictly_decreasing_penetration,
      true,
    ),
    allowRecontact: boolean(contact.allow_recontact, false),
  };
}

function contactValue(draft: SupportContactDraft): Record<string, JsonValue> {
  if (draft.modality === 'bounded_penetration') {
    return {
      support_object_id: draft.supportObjectId,
      mode: 'bounded_penetration',
      max_contact_penetration_m: draft.maxContactPenetrationM,
    };
  }
  if (draft.modality === 'contact_process') {
    return {
      support_object_id: draft.supportObjectId,
      mode: 'contact_process',
    };
  }
  if (draft.modality === 'binary_contact') {
    return {
      support_object_id: draft.supportObjectId,
      mode: 'contact_release',
      release_evidence_mode: 'binary_contact',
      require_separation_by_end: draft.requireSeparationByEnd,
      allow_recontact: draft.allowRecontact,
    };
  }
  return {
    support_object_id: draft.supportObjectId,
    mode: 'contact_release',
    release_evidence_mode: 'signed_penetration',
    enforce_max_start_penetration: draft.enforceMaxStart,
    ...(draft.enforceMaxStart
      ? { max_start_penetration_m: draft.maxStartPenetrationM }
      : {}),
    release_tolerance_m: draft.releaseToleranceM,
    min_release_progress_m: draft.minReleaseProgressM,
    require_separation_by_end: draft.requireSeparationByEnd,
    require_strictly_decreasing_penetration:
      draft.requireDecreasingPenetration,
    allow_recontact: draft.allowRecontact,
  };
}

function valid(draft: SupportContactDraft): boolean {
  if (
    !draft.supportedObjectId
    || !draft.supportObjectId
    || draft.supportedObjectId === draft.supportObjectId
  ) return false;
  const values = draft.modality === 'bounded_penetration'
    ? [draft.maxContactPenetrationM]
    : draft.modality === 'signed_penetration'
      ? [
          draft.maxStartPenetrationM,
          draft.releaseToleranceM,
          draft.minReleaseProgressM,
        ]
      : [];
  return values.every((value) => Number.isFinite(value) && value >= 0);
}

function nextDefault(objectIds: string[]): SupportContactDraft {
  return {
    supportedObjectId: objectIds[0] ?? '',
    supportObjectId: objectIds[1] ?? '',
    modality: 'signed_penetration',
    enforceMaxStart: true,
    maxStartPenetrationM: 0,
    releaseToleranceM: 0.00001,
    minReleaseProgressM: 0,
    maxContactPenetrationM: 0,
    requireSeparationByEnd: true,
    requireDecreasingPenetration: true,
    allowRecontact: false,
  };
}

export function ProgramSupportContactEditor({
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
  const objectIds = useMemo(
    () => (program.scene ?? []).map((row) => row.scene_resource_id).sort(),
    [program.scene],
  );
  const rows = useMemo(() => (program.scene ?? []).flatMap((resource) => (
    rawContacts(resource).flatMap((contact) => {
      const draft = draftFromContact(resource.scene_resource_id, contact);
      return draft === null ? [] : [draft];
    })
  )), [program.scene]);
  const [selectedKey, setSelectedKey] = useState('');
  const selected = rows.find((row) => (
    `${row.supportedObjectId}\0${row.supportObjectId}` === selectedKey
  )) ?? null;
  const [draft, setDraft] = useState(() => nextDefault(objectIds));

  useEffect(() => {
    setDraft(selected ?? nextDefault(objectIds));
  }, [draftRevision, objectIds, selected]);

  const replace = (next: SupportContactDraft, remove = false) => {
    if (busy || (!remove && !valid(next))) return;
    const resource = program.scene?.find(
      (row) => row.scene_resource_id === next.supportedObjectId,
    );
    if (resource === undefined) return;
    const previousSupportId = selected?.supportObjectId ?? next.supportObjectId;
    const contacts = rawContacts(resource).filter(
      (row) => row.support_object_id !== previousSupportId,
    );
    if (!remove) contacts.push(contactValue(next));
    contacts.sort((left, right) => String(left.support_object_id)
      .localeCompare(String(right.support_object_id)));
    void mutate('program_draft_replace_support_contacts', {
      scene_resource_id: next.supportedObjectId,
      support_contacts: contacts,
    }).then((result) => {
      if (result !== null) {
        setSelectedKey(remove
          ? ''
          : `${next.supportedObjectId}\0${next.supportObjectId}`);
      }
    });
  };
  const update = (next: SupportContactDraft, commit = false) => {
    setDraft(next);
    if (commit && selected !== null) replace(next);
  };

  return (
    <details className="dcard" open>
      <summary>Object support contacts</summary>
      <p className="muted">
        Declare which Program scene object supports another and what geometric
        evidence is required. Current contact evidence remains read-only.
      </p>
      {objectIds.length < 2 ? (
        <p className="scene-commit-error inline-message severity-error" role="status">
          Add at least two Program scene resources to author a support contact.
        </p>
      ) : (
        <div className="contact-policy-fields">
          <label>
            <span>Contact</span>
            <select
              value={selectedKey}
              onChange={(event) => {
                const key = event.currentTarget.value;
                setSelectedKey(key);
                const row = rows.find((candidate) => (
                  `${candidate.supportedObjectId}\0${candidate.supportObjectId}`
                    === key
                ));
                setDraft(row ?? nextDefault(objectIds));
              }}
            >
              <option value="">New support contact</option>
              {rows.map((row) => {
                const key = `${row.supportedObjectId}\0${row.supportObjectId}`;
                return (
                  <option key={key} value={key}>
                    {row.supportedObjectId} supported by {row.supportObjectId}
                  </option>
                );
              })}
            </select>
          </label>
          <div className="program-template-grid dgrid">
            <label>
              <span>Supported object</span>
              <select
                value={draft.supportedObjectId}
                disabled={busy || selected !== null}
                title={blockedTitle}
                onChange={(event) => {
                  const supportedObjectId = event.currentTarget.value;
                  update({
                    ...draft,
                    supportedObjectId,
                    supportObjectId: draft.supportObjectId === supportedObjectId
                      ? objectIds.find((id) => id !== supportedObjectId) ?? ''
                      : draft.supportObjectId,
                  });
                }}
              >
                {objectIds.map((id) => <option key={id}>{id}</option>)}
              </select>
            </label>
            <label>
              <span>Support object</span>
              <select
                value={draft.supportObjectId}
                disabled={busy}
                title={blockedTitle}
                onChange={(event) => update(
                  { ...draft, supportObjectId: event.currentTarget.value },
                  selected !== null,
                )}
              >
                {objectIds.filter((id) => id !== draft.supportedObjectId)
                  .map((id) => <option key={id}>{id}</option>)}
              </select>
            </label>
          </div>
          <label>
            <span>Validation evidence</span>
            <select
              value={draft.modality}
              disabled={busy}
              title={blockedTitle}
              onChange={(event) => update(
                {
                  ...draft,
                  modality: event.currentTarget.value as ContactModality,
                },
                selected !== null,
              )}
            >
              <option value="signed_penetration">Precise release</option>
              <option value="binary_contact">Lightweight release</option>
              <option value="bounded_penetration">Bounded contact</option>
              <option value="contact_process" disabled>Contact process (reserved)</option>
            </select>
          </label>
          {draft.modality === 'signed_penetration' && (<>
            <SupportCheck
              label="Limit starting penetration"
              value={draft.enforceMaxStart}
              busy={busy}
              blockedTitle={blockedTitle}
              onChange={(value) => update(
                { ...draft, enforceMaxStart: value },
                selected !== null,
              )}
            />
            {draft.enforceMaxStart && (
              <SupportNumber
                label="Maximum starting penetration (m)"
                value={draft.maxStartPenetrationM}
                busy={busy}
                blockedTitle={blockedTitle}
                onChange={(value) => update({ ...draft, maxStartPenetrationM: value })}
                onCommit={() => selected !== null && replace(draft)}
              />
            )}
            <div className="program-template-grid dgrid">
              <SupportNumber
                label="Release tolerance (m)"
                value={draft.releaseToleranceM}
                busy={busy}
                blockedTitle={blockedTitle}
                onChange={(value) => update({ ...draft, releaseToleranceM: value })}
                onCommit={() => selected !== null && replace(draft)}
              />
              <SupportNumber
                label="Minimum release progress (m)"
                value={draft.minReleaseProgressM}
                busy={busy}
                blockedTitle={blockedTitle}
                onChange={(value) => update({ ...draft, minReleaseProgressM: value })}
                onCommit={() => selected !== null && replace(draft)}
              />
            </div>
          </>)}
          {draft.modality === 'bounded_penetration' && (
            <SupportNumber
              label="Maximum contact penetration (m)"
              value={draft.maxContactPenetrationM}
              busy={busy}
              blockedTitle={blockedTitle}
              onChange={(value) => update({ ...draft, maxContactPenetrationM: value })}
              onCommit={() => selected !== null && replace(draft)}
            />
          )}
          {(draft.modality === 'signed_penetration'
            || draft.modality === 'binary_contact') && (
            <div className="contact-policy-checks">
              <SupportCheck
                label="Require separation by end"
                value={draft.requireSeparationByEnd}
                busy={busy}
                blockedTitle={blockedTitle}
                onChange={(value) => update(
                  { ...draft, requireSeparationByEnd: value },
                  selected !== null,
                )}
              />
              {draft.modality === 'signed_penetration' && (
                <SupportCheck
                  label="Require decreasing penetration"
                  value={draft.requireDecreasingPenetration}
                  busy={busy}
                  blockedTitle={blockedTitle}
                  onChange={(value) => update(
                    { ...draft, requireDecreasingPenetration: value },
                    selected !== null,
                  )}
                />
              )}
              <SupportCheck
                label="Allow re-contact"
                value={draft.allowRecontact}
                busy={busy}
                blockedTitle={blockedTitle}
                onChange={(value) => update(
                  { ...draft, allowRecontact: value },
                  selected !== null,
                )}
              />
            </div>
          )}
          <div className="dactions">
            {selected === null ? (
              <button
                type="button"
                disabled={busy || !valid(draft)}
                title={blockedTitle}
                onClick={() => replace(draft)}
              >Add support contact</button>
            ) : (
              <button
                type="button"
                className="danger-button"
                disabled={busy}
                title={blockedTitle}
                onClick={() => replace(draft, true)}
              >Remove support contact</button>
            )}
          </div>
        </div>
      )}
    </details>
  );
}

function SupportNumber({
  label,
  value,
  busy,
  blockedTitle,
  onChange,
  onCommit,
}: {
  label: string;
  value: number;
  busy: boolean;
  blockedTitle?: string;
  onChange: (value: number) => void;
  onCommit: () => void;
}) {
  return (
    <label>
      <span>{label}</span>
      <input
        type="number"
        min={0}
        step="any"
        value={Number.isFinite(value) ? value : ''}
        disabled={busy}
        title={blockedTitle}
        onChange={(event) => onChange(event.currentTarget.valueAsNumber)}
        onBlur={onCommit}
        onKeyDown={(event) => {
          if (event.key === 'Enter') event.currentTarget.blur();
        }}
      />
    </label>
  );
}

function SupportCheck({
  label,
  value,
  busy,
  blockedTitle,
  onChange,
}: {
  label: string;
  value: boolean;
  busy: boolean;
  blockedTitle?: string;
  onChange: (value: boolean) => void;
}) {
  return (
    <label className="contact-policy-check">
      <input
        type="checkbox"
        checked={value}
        disabled={busy}
        title={blockedTitle}
        onChange={(event) => onChange(event.currentTarget.checked)}
      />
      <span>{label}</span>
    </label>
  );
}
