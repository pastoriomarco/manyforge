import { useEffect, useState } from 'react';

import type { CatalogJsonSchema, JsonValue } from '../types/api';
import { groupByNamespace } from '../utils/blackboardNamespaces';
import { programBlackboardKeys } from '../utils/programAuthoring';
import {
  ProgramSchemaValueEditor,
  type ProgramSchemaValueEditorContext,
} from './property/ProgramSchemaValueEditor';

interface GraspLostPolicy {
  raise_alarm: boolean;
  stop_motion: boolean;
  set_key: string | null;
  detach: boolean;
}

const DEFAULT_POLICY: GraspLostPolicy = {
  raise_alarm: true,
  stop_motion: true,
  set_key: null,
  detach: true,
};

const KEY_SCHEMA: CatalogJsonSchema = {
  type: 'string',
  'x-manyforge-semantic-type': 'blackboard_key',
};

const KEY_REQUIRED_HINT = 'Choose a declared writable bool key to enable this response.';
const KEY_INVALID_HINT = 'The selected key must be a declared writable bool key.';

function policyValue(value: JsonValue | undefined): GraspLostPolicy {
  const row: Record<string, JsonValue> = value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value : {};
  return {
    raise_alarm: typeof row.raise_alarm === 'boolean' ? row.raise_alarm : DEFAULT_POLICY.raise_alarm,
    stop_motion: typeof row.stop_motion === 'boolean' ? row.stop_motion : DEFAULT_POLICY.stop_motion,
    set_key: typeof row.set_key === 'string' ? row.set_key : null,
    detach: typeof row.detach === 'boolean' ? row.detach : DEFAULT_POLICY.detach,
  };
}

function writableBoolKeys(context: ProgramSchemaValueEditorContext): Set<string> {
  return new Set(programBlackboardKeys(context.program, context.globalRuleIds)
    .filter((key) => key.type === 'bool' && key.access !== 'read' && key.expression === undefined)
    .map((key) => key.key));
}

/** The literal four-choice policy attached to one carried-object attach node. */
export function GraspLostPolicyEditor({
  value,
  nodeId,
  context,
  disabled,
  onCommit,
}: {
  value: JsonValue | undefined;
  nodeId: string;
  context: ProgramSchemaValueEditorContext;
  disabled: boolean;
  onCommit: (value: JsonValue) => void;
}) {
  const serializedValue = JSON.stringify(policyValue(value)) ?? '{}';
  const incoming = JSON.parse(serializedValue) as GraspLostPolicy;
  const serializedKey = JSON.stringify(incoming.set_key) ?? 'null';
  const [policy, setPolicy] = useState(() => policyValue(value));
  const [setKeyEnabled, setSetKeyEnabled] = useState(() => policyValue(value).set_key !== null);
  const [error, setError] = useState<string | null>(null);
  const keys = writableBoolKeys(context);
  const keyChoices = groupByNamespace([...keys], (key) => key).flatMap((group) => (
    group.items.map((key) => ({ value: key, label: key, group: group.label }))
  ));

  useEffect(() => {
    setPolicy(JSON.parse(serializedValue) as GraspLostPolicy);
    setError(null);
  }, [nodeId, serializedValue]);

  useEffect(() => {
    setSetKeyEnabled((JSON.parse(serializedKey) as string | null) !== null);
  }, [nodeId, serializedKey]);

  const commit = (next: GraspLostPolicy) => {
    if (next.set_key !== null && !keys.has(next.set_key)) {
      setError(KEY_INVALID_HINT);
      return;
    }
    setPolicy(next);
    setError(null);
    onCommit(next as unknown as JsonValue);
  };

  const setBoolean = (field: 'raise_alarm' | 'stop_motion' | 'detach', checked: boolean) => {
    const next: GraspLostPolicy = { ...policy, [field]: checked };
    // The catalog/capture contract refuses the invalid alarm-without-Stop pair.
    // Keep the editor on the valid side of that rule as the user checks Alarm.
    if (field === 'raise_alarm' && checked) next.stop_motion = true;
    if (next.raise_alarm) next.stop_motion = true;
    commit(next);
  };

  const toggleSetKey = (checked: boolean) => {
    setSetKeyEnabled(checked);
    if (checked) {
      setError(null);
      return;
    }
    commit({ ...policy, set_key: null });
  };

  const setKey = (key: string) => {
    const next = { ...policy, set_key: key };
    setSetKeyEnabled(true);
    commit(next);
  };

  return (
    <fieldset className="program-grasp-lost-policy" aria-label="Grasp lost response">
      <legend>On grasp lost</legend>
      <label>
        <input type="checkbox" aria-label="Raise alarm on grasp loss" checked={policy.raise_alarm}
          disabled={disabled} onChange={(event) => setBoolean('raise_alarm', event.target.checked)} />
        Raise alarm
      </label>
      <label>
        <input type="checkbox" aria-label="Stop motion on grasp loss"
          checked={policy.raise_alarm || policy.stop_motion}
          disabled={disabled || policy.raise_alarm}
          onChange={(event) => setBoolean('stop_motion', event.target.checked)} />
        Stop motion
      </label>
      {policy.raise_alarm && (
        <p className="program-form-hint" role="note">Stop motion is required while the alarm is enabled.</p>
      )}
      <label>
        <input type="checkbox" aria-label="Set key on grasp loss" checked={setKeyEnabled}
          disabled={disabled || (!setKeyEnabled && keys.size === 0 && context.onCreateReference === undefined)}
          onChange={(event) => toggleSetKey(event.target.checked)} />
        Set a blackboard key
      </label>
      {setKeyEnabled && (
        <div className="program-grasp-lost-key">
          <label className="program-grasp-lost-key-field">
            Key to set on loss
            <ProgramSchemaValueEditor
              schema={KEY_SCHEMA}
              value={policy.set_key ?? undefined}
              disabled={disabled}
              context={{ ...context, semanticChoices: keyChoices }}
              path="on_grasp_lost.set_key"
              onCommit={(next) => {
                if (typeof next === 'string') setKey(next);
              }}
              onInvalidChange={(message) => setError(message)}
            />
          </label>
          {keys.size === 0 && context.onCreateReference === undefined && (
            <p className="program-form-hint" role="note">Declare a writable bool key first.</p>
          )}
          {policy.set_key === null && <p className="program-form-hint" role="note">{KEY_REQUIRED_HINT}</p>}
        </div>
      )}
      <label>
        <input type="checkbox" aria-label="Detach object on grasp loss" checked={policy.detach}
          disabled={disabled} onChange={(event) => setBoolean('detach', event.target.checked)} />
        Detach object
      </label>
      {error !== null && <p className="program-form-error inline-message severity-error" role="alert">{error}</p>}
    </fieldset>
  );
}
