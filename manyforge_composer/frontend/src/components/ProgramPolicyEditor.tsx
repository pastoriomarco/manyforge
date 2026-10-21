import { useEffect, useMemo, useRef, useState } from 'react';

import type {
  ProgramInterventionStopScope,
  ProgramNodeRecoveryPolicy,
  ProgramOperatorStopPolicy,
  ProgramTreeNode,
} from '../types/api';

const DEFAULT_OPERATOR_STOP_POLICY: ProgramOperatorStopPolicy = 'inherit';
const DEFAULT_INTERVENTION_STOP_SCOPE: ProgramInterventionStopScope = 'all';
/** Largest finite retry limit; 0 on the wire means Unlimited. */
export const MAX_RETRY_LIMIT = 255;

export interface ProgramPolicyForm {
  allow_intervention: boolean;
  /** Unlimited is authored as `max_retries: 0`, never shown as a bare 0. */
  retry_limit_mode: 'unlimited' | 'limited';
  /** The finite limit while limited; NaN while the field is empty. */
  retry_limit: number;
  /** Written only while intervention is allowed; the default `all` is omitted. */
  intervention_stop_scope: ProgramInterventionStopScope;
  operator_stop_policy: ProgramOperatorStopPolicy;
  execution_group_mode: 'inherit' | 'all' | 'named';
  execution_group_names: string;
}

export type ProgramPolicyField =
  | 'recovery_policy.max_retries'
  | 'execution_groups';

export function effectiveProgramPolicy(node: ProgramTreeNode): ProgramPolicyForm {
  const maxRetries = node.recovery_policy?.max_retries ?? 0;
  return {
    allow_intervention: node.recovery_policy?.allow_intervention ?? false,
    retry_limit_mode: maxRetries === 0 ? 'unlimited' : 'limited',
    retry_limit: maxRetries === 0 ? 1 : maxRetries,
    intervention_stop_scope: node.recovery_policy?.intervention_stop_scope
      ?? DEFAULT_INTERVENTION_STOP_SCOPE,
    operator_stop_policy: node.operator_stop_policy ?? DEFAULT_OPERATOR_STOP_POLICY,
    execution_group_mode: Array.isArray(node.execution_groups)
      ? 'named' : node.execution_groups ?? 'inherit',
    execution_group_names: Array.isArray(node.execution_groups)
      ? node.execution_groups.join(', ') : '',
  };
}

/** The authored two-field policy the form describes. */
export function programRecoveryPolicy(form: ProgramPolicyForm): ProgramNodeRecoveryPolicy {
  const policy: ProgramNodeRecoveryPolicy = {
    allow_intervention: form.allow_intervention,
    max_retries: form.retry_limit_mode === 'unlimited' ? 0 : form.retry_limit,
  };
  if (form.allow_intervention && form.intervention_stop_scope === 'own_groups') {
    policy.intervention_stop_scope = 'own_groups';
  }
  return policy;
}

/** "Unlimited" or "N retries": the authored zero is never shown as 0. */
export function retryLimitLabel(maxRetries: number): string {
  return maxRetries === 0
    ? 'Unlimited'
    : `${maxRetries} ${maxRetries === 1 ? 'retry' : 'retries'}`;
}

export function validateProgramPolicy(
  form: ProgramPolicyForm,
): Partial<Record<ProgramPolicyField, string>> {
  const errors: Partial<Record<ProgramPolicyField, string>> = {};
  if (form.execution_group_mode === 'named') {
    const groups = form.execution_group_names.split(',').map((name) => name.trim());
    if (groups.length > 4096 || new Set(groups).size !== groups.length
        || groups.some((name) => name.length > 128 || !/^[a-z][a-z0-9_.-]*$/.test(name)
          || name === 'all' || name === 'inherit')) {
      errors.execution_groups = 'Use unique lower-case group names separated by commas (letters, digits, _, . or -; start with a letter; at most 128 characters). All and inherit are reserved.';
    }
  }
  if (
    form.retry_limit_mode === 'limited'
    && (!Number.isInteger(form.retry_limit)
      || form.retry_limit < 1
      || form.retry_limit > MAX_RETRY_LIMIT)
  ) {
    errors['recovery_policy.max_retries'] = `A retry limit is a whole number from 1 to ${MAX_RETRY_LIMIT}. Choose Unlimited for no limit.`;
  }
  return errors;
}

function splitGroupNames(names: string): string[] {
  return names.split(',').map((name) => name.trim());
}

/** "All only" or "All + <group ids>", keeping the exact identifiers. */
export function executionGroupMembershipSummary(groups: readonly string[]): string {
  return groups.length === 0 ? 'All only' : `All + ${[...groups].sort().join(', ')}`;
}

function fieldId(instanceId: string, field: string): string {
  return `policy-${instanceId}-${field}`;
}

function FieldError({
  id,
  message,
}: {
  id: string;
  message: string | undefined;
}) {
  if (!message) return null;
  return <span id={id} className="sr-only" role="alert">{message}</span>;
}

function selectOptions<T extends string>(options: Array<[T, string]>) {
  return options.map(([value, label]) => <option key={value} value={value}>{label}</option>);
}

interface ProgramPolicyEditorProps {
  node: ProgramTreeNode;
  inheritedGroups?: string[];
  availableGroups?: string[];
  onCreateGroup?: () => void;
  busy: boolean;
  onUpdate: (
    instanceId: string,
    recoveryPolicy: ProgramNodeRecoveryPolicy,
    operatorStopPolicy: ProgramOperatorStopPolicy,
    executionGroups: NonNullable<ProgramTreeNode['execution_groups']>,
  ) => void;
  onInvalidChange?: (message: string | null) => void;
}

/** The exact authored values one commit would send. */
function authoredPolicy(form: ProgramPolicyForm) {
  return {
    recovery: programRecoveryPolicy(form),
    operatorStop: form.operator_stop_policy,
    groups: form.execution_group_mode === 'named'
      ? splitGroupNames(form.execution_group_names).sort()
      : form.execution_group_mode,
  };
}

export function ProgramPolicyEditor({
  node,
  inheritedGroups = [],
  availableGroups = [],
  onCreateGroup,
  busy,
  onUpdate,
  onInvalidChange,
}: ProgramPolicyEditorProps) {
  const [form, setForm] = useState<ProgramPolicyForm>(() => effectiveProgramPolicy(node));

  useEffect(() => {
    setForm(effectiveProgramPolicy(node));
  }, [node]);

  const errors = useMemo(() => validateProgramPolicy(form), [form]);
  const original = useMemo(() => effectiveProgramPolicy(node), [node]);
  const firstError = Object.values(errors)[0] ?? null;
  const invalidChange = useRef(onInvalidChange);
  useEffect(() => {
    invalidChange.current = onInvalidChange;
  }, [onInvalidChange]);
  useEffect(() => {
    invalidChange.current?.(firstError);
  }, [firstError]);
  useEffect(() => () => invalidChange.current?.(null), []);
  // The policy last handed to onUpdate for this node: a later blur (or a
  // repeated change) must not submit it again while the Draft round trip is
  // pending.
  const submitted = useRef<string | null>(null);
  useEffect(() => {
    submitted.current = null;
  }, [node]);
  const commit = (next: ProgramPolicyForm = form) => {
    if (busy || Object.keys(validateProgramPolicy(next)).length > 0) return;
    const authored = authoredPolicy(next);
    const key = JSON.stringify(authored);
    if (key === JSON.stringify(authoredPolicy(original)) || key === submitted.current) return;
    submitted.current = key;
    onUpdate(node.instance_id, authored.recovery, authored.operatorStop, authored.groups);
  };
  // Membership checkboxes are discrete choices: they commit on change.
  const changeGroups = (groups: string[] | 'inherit') => {
    const next: ProgramPolicyForm = {
      ...form,
      execution_group_mode: groups === 'inherit' ? 'inherit' : groups.length > 0 ? 'named' : 'all',
      execution_group_names: groups === 'inherit' ? '' : [...groups].sort().join(', '),
    };
    setForm(next);
    commit(next);
  };
  const id = node.instance_id;
  const resolvedGroups = form.execution_group_mode === 'inherit' ? inheritedGroups
    : form.execution_group_mode === 'all' ? []
      : splitGroupNames(form.execution_group_names).filter(Boolean);
  // Memberships whose group is not declared stay visible so they can be removed.
  const missingGroups = resolvedGroups.filter((groupId) => !availableGroups.includes(groupId));
  const branch = node.children.length > 0;
  const errorId = (field: ProgramPolicyField) => fieldId(id, `${field}-error`);
  const describedBy = (field: ProgramPolicyField) =>
    errors[field] ? errorId(field) : undefined;
  const retryError = errors['recovery_policy.max_retries'];

  return (
    <section className="program-policy-editor dcard" aria-labelledby={fieldId(id, 'title')}>
      <h4 id={fieldId(id, 'title')}>Node policy</h4>
      <form
        className="program-policy-form"
        onBlur={() => commit()}
        onSubmit={(event) => {
          event.preventDefault();
          commit();
        }}
      >
        <p className="program-policy-note hint">
          Effective defaults are shown when fields are omitted; defaults are saved
          by omission. Changes edit only this Draft. Save persists independently;
          Apply makes the Draft the live Program. A Run captures this policy when
          it starts, so a running Run keeps its captured policy.
        </p>

      <fieldset className="program-policy-fieldset">
        <legend>Failure recovery</legend>
        <label className="program-policy-checkbox" htmlFor={fieldId(id, 'allow_intervention')}>
          <input
            id={fieldId(id, 'allow_intervention')}
            type="checkbox"
            checked={form.allow_intervention}
            disabled={busy}
            onChange={(event) => {
              const next = { ...form, allow_intervention: event.target.checked };
              setForm(next);
              commit(next);
            }}
          />
          Allow intervention
        </label>
        <p className="program-policy-help">
          When this node fails, the run pauses and anyone (operator, assistant or
          API) may correct it and Retry. Otherwise the failure ends the run. The
          same rule applies to every caller.
        </p>
        {form.allow_intervention && (
          <>
            <label className="program-policy-field" htmlFor={fieldId(id, 'intervention_stop_scope')}>
              Groups that stop
              <select
                id={fieldId(id, 'intervention_stop_scope')}
                value={form.intervention_stop_scope}
                disabled={busy}
                onChange={(event) => {
                  const next: ProgramPolicyForm = {
                    ...form,
                    intervention_stop_scope: event.target.value as ProgramInterventionStopScope,
                  };
                  setForm(next);
                  commit(next);
                }}
              >
                {selectOptions([
                  ['all', 'All groups'],
                  ['own_groups', "Only this node's groups"],
                ])}
              </select>
            </label>
            <p className="program-policy-help">
              Which motion groups stop while the run waits for the operator.
            </p>
          </>
        )}
        <label className="program-policy-field" htmlFor={fieldId(id, 'retry_limit_mode')}>
          Retry limit
          <select
            id={fieldId(id, 'retry_limit_mode')}
            value={form.retry_limit_mode}
            disabled={busy}
            onChange={(event) => {
              const next: ProgramPolicyForm = {
                ...form,
                retry_limit_mode: event.target.value as ProgramPolicyForm['retry_limit_mode'],
              };
              setForm(next);
              commit(next);
            }}
          >
            {selectOptions([
              ['unlimited', 'Unlimited'],
              ['limited', 'Limit to'],
            ])}
          </select>
        </label>
        {form.retry_limit_mode === 'limited' && (
          <label className="program-policy-field" htmlFor={fieldId(id, 'max_retries')}>
            Retries per run
            <input
              id={fieldId(id, 'max_retries')}
              type="number"
              min={1}
              max={MAX_RETRY_LIMIT}
              step={1}
              value={Number.isNaN(form.retry_limit) ? '' : form.retry_limit}
              disabled={busy}
              aria-invalid={retryError !== undefined}
              aria-describedby={describedBy('recovery_policy.max_retries')}
              title={retryError}
              onChange={(event) => setForm((current) => ({
                ...current,
                retry_limit: event.target.value.trim() === ''
                  ? Number.NaN
                  : Number(event.target.value),
              }))}
            />
            <FieldError
              id={errorId('recovery_policy.max_retries')}
              message={retryError}
            />
          </label>
        )}
        <p className="program-policy-help">
          Counts explicit Retries of this node within one run. Unlimited never
          retries automatically, and the limit applies only while intervention
          is allowed. Resuming an operator Stop and Reset are not retries.
        </p>
      </fieldset>

      <fieldset className="program-policy-fieldset">
        <legend>Control groups</legend>
        <label className="program-policy-field" htmlFor={fieldId(id, 'execution_groups')}>
          <input
            type="checkbox"
            id={fieldId(id, 'execution_groups')}
            checked={form.execution_group_mode === 'inherit'}
            disabled={busy}
            // Turning Inherit off keeps the inherited named groups as the
            // explicit choice, so this node stays covered by them.
            onChange={(event) => changeGroups(event.target.checked ? 'inherit' : [...inheritedGroups])}
          />
          Inherit groups from parent
        </label>
        {[...availableGroups, ...missingGroups].map((groupId) => {
          const missing = !availableGroups.includes(groupId);
          return (
            <label className="toggle-control" key={groupId}
              title={missing ? 'This group is not declared in Program settings. Uncheck to remove it.' : undefined}>
              <input
                type="checkbox"
                checked={resolvedGroups.includes(groupId)}
                disabled={busy || form.execution_group_mode === 'inherit'}
                onChange={(event) => changeGroups(event.target.checked
                  ? [...resolvedGroups, groupId]
                  : resolvedGroups.filter((name) => name !== groupId))}
              />
              {missing ? `${groupId} · missing` : groupId}
            </label>
          );
        })}
        {onCreateGroup !== undefined && (
          <button type="button" disabled={busy} onClick={onCreateGroup}>Create group…</button>
        )}
        <FieldError id={errorId('execution_groups')} message={errors.execution_groups} />
        <p className="program-policy-help">
          Draft memberships: {executionGroupMembershipSummary(resolvedGroups)}
          {form.execution_group_mode === 'inherit' ? ' (inherited)' : ''}.
          {' '}All always includes this node. Named groups replace inherited names
          and may overlap. With Inherit off and no groups selected, only All controls this node.
          {branch && ' Child nodes that inherit follow later changes here.'}
          {' '}Saved for the next Run; running membership cannot change.
        </p>
      </fieldset>

      <fieldset className="program-policy-fieldset">
        <legend>Operator Stop policy</legend>
        <label className="program-policy-field" htmlFor={fieldId(id, 'operator_stop_policy')}>
          Stop behavior
          <select
            id={fieldId(id, 'operator_stop_policy')}
            value={form.operator_stop_policy}
            disabled={busy}
            onChange={(event) => setForm((current) => ({
              ...current,
              operator_stop_policy: event.target.value as ProgramOperatorStopPolicy,
            }))}
          >
            {selectOptions([
              ['inherit', 'Inherit'],
              ['hold', 'Hold'],
              ['continue', 'Continue'],
            ])}
          </select>
        </label>
        <p className="program-policy-help">
          Captured by the next Run. Hold pauses this subtree after an owner-issued
          motion Stop; Continue keeps supervisory work ticking. Inherit follows
          the parent for composite/decorator nodes; leaves default to Hold and
          the root to Continue. It never changes a running Run.
        </p>
      </fieldset>

      </form>
    </section>
  );
}
