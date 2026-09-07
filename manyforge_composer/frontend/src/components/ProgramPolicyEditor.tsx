import { useEffect, useMemo, useRef, useState } from 'react';

import type {
  ProgramModificationPolicyValue,
  ProgramInterventionDelegate,
  ProgramNodeModificationPolicy,
  ProgramNodeRecoveryPolicy,
  ProgramOperatorStopPolicy,
  ProgramPersistenceCeiling,
  ProgramRecoveryAction,
  ProgramTreeNode,
} from '../types/api';

export const ALLOWED_SKILLS_UNAVAILABLE_MESSAGE =
  'Allowed recovery skills are unavailable: each skill requires an exact revision-qualified retained referent. Leave Allowed skills empty.';

const DEFAULT_MODIFICATION_POLICY: ProgramNodeModificationPolicy = {
  runtime_override: 'forbidden',
  persistent_edit: 'forbidden',
  intervention_delegate: 'forbidden',
};
const DEFAULT_RECOVERY_POLICY: ProgramNodeRecoveryPolicy = {
  allow_intervention: false,
  allowed_recovery_actions: [],
  max_retries: 0,
  allowed_skills: [],
  persistence_ceiling: 'session_only',
};
const DEFAULT_OPERATOR_STOP_POLICY: ProgramOperatorStopPolicy = 'inherit';
const RECOVERY_ACTIONS: ProgramRecoveryAction[] = [
  'intervention',
  'runtime_override',
];

export interface ProgramPolicyForm {
  modification_policy: ProgramNodeModificationPolicy;
  recovery_policy: ProgramNodeRecoveryPolicy;
  operator_stop_policy: ProgramOperatorStopPolicy;
  execution_group_mode: 'inherit' | 'all' | 'named';
  execution_group_names: string;
}

export type ProgramPolicyField =
  | 'modification_policy.runtime_override'
  | 'modification_policy.persistent_edit'
  | 'modification_policy.intervention_delegate'
  | 'recovery_policy.allow_intervention'
  | 'recovery_policy.allowed_recovery_actions'
  | 'recovery_policy.max_retries'
  | 'recovery_policy.allowed_skills'
  | 'recovery_policy.persistence_ceiling'
  | 'operator_stop_policy'
  | 'execution_groups';

export function effectiveProgramPolicy(node: ProgramTreeNode): ProgramPolicyForm {
  return {
    modification_policy: {
      ...DEFAULT_MODIFICATION_POLICY,
      ...node.modification_policy,
    },
    recovery_policy: {
      ...DEFAULT_RECOVERY_POLICY,
      ...node.recovery_policy,
      allowed_recovery_actions: [
        ...(node.recovery_policy?.allowed_recovery_actions ?? []),
      ],
      allowed_skills: [...(node.recovery_policy?.allowed_skills ?? [])],
    },
    operator_stop_policy: node.operator_stop_policy ?? DEFAULT_OPERATOR_STOP_POLICY,
    execution_group_mode: Array.isArray(node.execution_groups)
      ? 'named' : node.execution_groups ?? 'inherit',
    execution_group_names: Array.isArray(node.execution_groups)
      ? node.execution_groups.join(', ') : '',
  };
}

function setError(
  errors: Partial<Record<ProgramPolicyField, string>>,
  field: ProgramPolicyField,
  message: string,
) {
  errors[field] = errors[field] ? `${errors[field]} ${message}` : message;
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
      setError(errors, 'execution_groups',
        'Use unique lower-case group names separated by commas (letters, digits, _, . or -; start with a letter; at most 128 characters). All and inherit are reserved.');
    }
  }
  const { modification_policy: modification, recovery_policy: recovery } = form;
  const hasIntervention = recovery.allowed_recovery_actions.includes('intervention');
  const hasRuntimeOverride = recovery.allowed_recovery_actions.includes('runtime_override');

  if (recovery.allow_intervention !== hasIntervention) {
    const message = 'Intervention permission must match the Intervention recovery action.';
    setError(errors, 'recovery_policy.allow_intervention', message);
    setError(errors, 'recovery_policy.allowed_recovery_actions', message);
  }
  if (recovery.allowed_skills.length > 0 && !hasIntervention) {
    setError(
      errors,
      'recovery_policy.allowed_skills',
      'Allowed skills require the Intervention recovery action.',
    );
  }
  if (
    (hasRuntimeOverride || recovery.persistence_ceiling === 'runtime_override')
    && modification.runtime_override === 'forbidden'
  ) {
    const message = 'Runtime recovery action or persistence requires non-forbidden runtime override permission.';
    setError(errors, 'modification_policy.runtime_override', message);
    if (hasRuntimeOverride) {
      setError(errors, 'recovery_policy.allowed_recovery_actions', message);
    }
    if (recovery.persistence_ceiling === 'runtime_override') {
      setError(errors, 'recovery_policy.persistence_ceiling', message);
    }
  }
  if (
    recovery.persistence_ceiling === 'draft_patch'
    && modification.persistent_edit === 'forbidden'
  ) {
    const message = 'Draft-patch persistence requires non-forbidden persistent edit permission.';
    setError(errors, 'modification_policy.persistent_edit', message);
    setError(errors, 'recovery_policy.persistence_ceiling', message);
  }
  const nondefaultRecovery = recovery.allow_intervention
    || recovery.allowed_recovery_actions.length > 0
    || recovery.max_retries !== 0
    || recovery.allowed_skills.length > 0
    || recovery.persistence_ceiling !== 'session_only';
  if (nondefaultRecovery && recovery.allowed_recovery_actions.length === 0) {
    const message = 'A non-default recovery policy requires at least one recovery action.';
    setError(errors, 'recovery_policy.allowed_recovery_actions', message);
    if (recovery.allow_intervention) {
      setError(errors, 'recovery_policy.allow_intervention', message);
    }
    if (recovery.max_retries !== 0) {
      setError(errors, 'recovery_policy.max_retries', message);
    }
    if (recovery.persistence_ceiling !== 'session_only') {
      setError(errors, 'recovery_policy.persistence_ceiling', message);
    }
    if (recovery.allowed_skills.length > 0) {
      setError(errors, 'recovery_policy.allowed_skills', message);
    }
  }
  if (!Number.isInteger(recovery.max_retries) || recovery.max_retries < 0 || recovery.max_retries > 255) {
    setError(errors, 'recovery_policy.max_retries', 'Max retries must be an integer from 0 to 255.');
  }
  if (recovery.allowed_skills.length > 0) {
    const message = hasIntervention
      ? ALLOWED_SKILLS_UNAVAILABLE_MESSAGE
      : `Allowed skills require the Intervention recovery action. ${ALLOWED_SKILLS_UNAVAILABLE_MESSAGE}`;
    setError(errors, 'recovery_policy.allowed_skills', message);
    // Imported skills have no editable C1 control. Mark their nearest visible
    // recovery control so the invalid retained value cannot be visually hidden.
    setError(errors, 'recovery_policy.allowed_recovery_actions', message);
  }
  return errors;
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
  busy: boolean;
  onUpdate: (
    instanceId: string,
    modificationPolicy: ProgramNodeModificationPolicy,
    recoveryPolicy: ProgramNodeRecoveryPolicy,
    operatorStopPolicy: ProgramOperatorStopPolicy,
    executionGroups: NonNullable<ProgramTreeNode['execution_groups']>,
  ) => void;
  onInvalidChange?: (message: string | null) => void;
}

export function ProgramPolicyEditor({
  node,
  inheritedGroups = [],
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
  const dirty = JSON.stringify(form) !== JSON.stringify(original);
  const firstError = Object.values(errors)[0] ?? null;
  const invalidChange = useRef(onInvalidChange);
  useEffect(() => {
    invalidChange.current = onInvalidChange;
  }, [onInvalidChange]);
  useEffect(() => {
    invalidChange.current?.(firstError);
  }, [firstError]);
  useEffect(() => () => invalidChange.current?.(null), []);
  const commit = () => {
    if (busy || !dirty || Object.keys(errors).length > 0) return;
    onUpdate(
      node.instance_id,
      { ...form.modification_policy },
      {
        ...form.recovery_policy,
        allowed_recovery_actions: [...form.recovery_policy.allowed_recovery_actions],
        allowed_skills: [...form.recovery_policy.allowed_skills],
      },
      form.operator_stop_policy,
      form.execution_group_mode === 'named'
        ? form.execution_group_names.split(',').map((name) => name.trim()).sort()
        : form.execution_group_mode,
    );
  };
  const id = node.instance_id;
  const resolvedGroups = form.execution_group_mode === 'inherit' ? inheritedGroups
    : form.execution_group_mode === 'all' ? []
      : form.execution_group_names.split(',').map((name) => name.trim()).filter(Boolean);
  const updateModification = (
    field: keyof ProgramNodeModificationPolicy,
    value: ProgramModificationPolicyValue | ProgramInterventionDelegate,
  ) => {
    setForm((current) => ({
      ...current,
      modification_policy: {
        ...current.modification_policy,
        [field]: value,
      },
    }));
  };
  const updateRecovery = <K extends keyof ProgramNodeRecoveryPolicy>(
    field: K,
    value: ProgramNodeRecoveryPolicy[K],
  ) => {
    setForm((current) => ({
      ...current,
      recovery_policy: {
        ...current.recovery_policy,
        [field]: value,
      },
    }));
  };
  const errorId = (field: ProgramPolicyField) => fieldId(id, `${field}-error`);
  const describedBy = (field: ProgramPolicyField) =>
    errors[field] ? errorId(field) : undefined;

  return (
    <section className="program-policy-editor" aria-labelledby={fieldId(id, 'title')}>
      <form
        className="program-policy-form"
        onBlur={commit}
        onSubmit={(event) => {
          event.preventDefault();
          commit();
        }}
      >
        <div className="program-policy-heading">
          <div>
            <p className="eyebrow">Authored policy</p>
            <h4 id={fieldId(id, 'title')}>Node policy</h4>
          </div>
        </div>
        <p className="program-policy-note">
          Effective canonical defaults are shown when fields are omitted. Defaults
          are saved by omission. These values are authored for future Run behavior;
          they are not currently applied to the runtime. Saving policy changes only
          this Draft. Save persists independently; global Apply makes the validated
          Draft the live runtime Program. A Run already started keeps its captured policy.
        </p>

      <fieldset className="program-policy-fieldset">
        <legend>Modification controls</legend>
        <label className="program-policy-field" htmlFor={fieldId(id, 'runtime_override')}>
          Runtime override
          <select
            id={fieldId(id, 'runtime_override')}
            value={form.modification_policy.runtime_override}
            disabled={busy}
            aria-invalid={errors['modification_policy.runtime_override'] !== undefined}
            aria-describedby={describedBy('modification_policy.runtime_override')}
            title={errors['modification_policy.runtime_override']}
            onChange={(event) => updateModification(
              'runtime_override',
              event.target.value as ProgramModificationPolicyValue,
            )}
          >
            {selectOptions([
              ['forbidden', 'Forbidden'],
              ['human_approval', 'Human approval'],
              ['llm_allowed', 'LLM allowed'],
            ])}
          </select>
          <FieldError
            id={errorId('modification_policy.runtime_override')}
            message={errors['modification_policy.runtime_override']}
          />
        </label>
        <label className="program-policy-field" htmlFor={fieldId(id, 'persistent_edit')}>
          Persistent edit
          <select
            id={fieldId(id, 'persistent_edit')}
            value={form.modification_policy.persistent_edit}
            disabled={busy}
            aria-invalid={errors['modification_policy.persistent_edit'] !== undefined}
            aria-describedby={describedBy('modification_policy.persistent_edit')}
            title={errors['modification_policy.persistent_edit']}
            onChange={(event) => updateModification(
              'persistent_edit',
              event.target.value as ProgramModificationPolicyValue,
            )}
          >
            {selectOptions([
              ['forbidden', 'Forbidden'],
              ['human_approval', 'Human approval'],
              ['llm_allowed', 'LLM allowed'],
            ])}
          </select>
          <FieldError
            id={errorId('modification_policy.persistent_edit')}
            message={errors['modification_policy.persistent_edit']}
          />
        </label>
        <label className="program-policy-field" htmlFor={fieldId(id, 'intervention_delegate')}>
          Intervention delegate
          <select
            id={fieldId(id, 'intervention_delegate')}
            value={form.modification_policy.intervention_delegate}
            disabled={busy}
            onChange={(event) => updateModification(
              'intervention_delegate',
              event.target.value as ProgramInterventionDelegate,
            )}
          >
            {selectOptions([
              ['forbidden', 'Forbidden'],
              ['human_only', 'Human only'],
              ['llm_allowed', 'LLM allowed'],
              ['program_only', 'Program only'],
            ])}
          </select>
        </label>
      </fieldset>

      <fieldset className="program-policy-fieldset">
        <legend>Recovery</legend>
        <label className="program-policy-checkbox" htmlFor={fieldId(id, 'allow_intervention')}>
          <input
            id={fieldId(id, 'allow_intervention')}
            type="checkbox"
            checked={form.recovery_policy.allow_intervention}
            disabled={busy}
            aria-invalid={errors['recovery_policy.allow_intervention'] !== undefined}
            aria-describedby={describedBy('recovery_policy.allow_intervention')}
            title={errors['recovery_policy.allow_intervention']}
            onChange={(event) => updateRecovery('allow_intervention', event.target.checked)}
          />
          Allow intervention
        </label>
        <FieldError
          id={errorId('recovery_policy.allow_intervention')}
          message={errors['recovery_policy.allow_intervention']}
        />

        <fieldset className="program-policy-actions">
          <legend>Allowed recovery actions</legend>
          {RECOVERY_ACTIONS.map((action) => {
            const actionId = fieldId(id, `action-${action}`);
            return (
              <label className="program-policy-checkbox" htmlFor={actionId} key={action}>
                <input
                  id={actionId}
                  type="checkbox"
                  checked={form.recovery_policy.allowed_recovery_actions.includes(action)}
                  disabled={busy}
                  aria-invalid={errors['recovery_policy.allowed_recovery_actions'] !== undefined}
                  aria-describedby={describedBy('recovery_policy.allowed_recovery_actions')}
                  title={errors['recovery_policy.allowed_recovery_actions']}
                  onChange={(event) => {
                    const actions = new Set(form.recovery_policy.allowed_recovery_actions);
                    if (event.target.checked) actions.add(action);
                    else actions.delete(action);
                    updateRecovery(
                      'allowed_recovery_actions',
                      RECOVERY_ACTIONS.filter((candidate) => actions.has(candidate)),
                    );
                  }}
                />
                {action === 'runtime_override' ? 'Runtime override' : 'Intervention'}
              </label>
            );
          })}
          <FieldError
            id={errorId('recovery_policy.allowed_recovery_actions')}
            message={errors['recovery_policy.allowed_recovery_actions']}
          />
        </fieldset>

        <label className="program-policy-field" htmlFor={fieldId(id, 'max_retries')}>
          Max retries
          <input
            id={fieldId(id, 'max_retries')}
            type="number"
            min={0}
            max={255}
            step={1}
            value={Number.isNaN(form.recovery_policy.max_retries) ? '' : form.recovery_policy.max_retries}
            disabled={busy}
            aria-invalid={errors['recovery_policy.max_retries'] !== undefined}
            aria-describedby={describedBy('recovery_policy.max_retries')}
            title={errors['recovery_policy.max_retries']}
            onChange={(event) => updateRecovery(
              'max_retries',
              event.target.value.trim() === '' ? Number.NaN : Number(event.target.value),
            )}
          />
          <FieldError
            id={errorId('recovery_policy.max_retries')}
            message={errors['recovery_policy.max_retries']}
          />
        </label>
        <label className="program-policy-field" htmlFor={fieldId(id, 'persistence_ceiling')}>
          Persistence ceiling
          <select
            id={fieldId(id, 'persistence_ceiling')}
            value={form.recovery_policy.persistence_ceiling}
            disabled={busy}
            aria-invalid={errors['recovery_policy.persistence_ceiling'] !== undefined}
            aria-describedby={describedBy('recovery_policy.persistence_ceiling')}
            title={errors['recovery_policy.persistence_ceiling']}
            onChange={(event) => updateRecovery(
              'persistence_ceiling',
              event.target.value as ProgramPersistenceCeiling,
            )}
          >
            {selectOptions([
              ['session_only', 'Session only'],
              ['runtime_override', 'Runtime override'],
              ['draft_patch', 'Draft patch'],
            ])}
          </select>
          <FieldError
            id={errorId('recovery_policy.persistence_ceiling')}
            message={errors['recovery_policy.persistence_ceiling']}
          />
        </label>

        <p id={fieldId(id, 'skills-help')} className="program-policy-help">
          {ALLOWED_SKILLS_UNAVAILABLE_MESSAGE}
        </p>
        {form.recovery_policy.allowed_skills.length > 0 && (
          <p className="program-form-error" role="alert">
            Imported authored Allowed skills ({form.recovery_policy.allowed_skills.join(', ')})
            are a blocking applicability error in this editor. Refresh or retry after
            the retained referent contract is available.
          </p>
        )}
      </fieldset>

      <fieldset className="program-policy-fieldset">
        <legend>Operator Stop policy</legend>
        <label className="program-policy-field" htmlFor={fieldId(id, 'execution_groups')}>
          Execution groups
          <select
            id={fieldId(id, 'execution_groups')}
            value={form.execution_group_mode}
            disabled={busy}
            onChange={(event) => setForm((current) => ({
              ...current,
              execution_group_mode: event.target.value as ProgramPolicyForm['execution_group_mode'],
            }))}
          >
            {selectOptions([
              ['inherit', 'Inherit'],
              ['all', 'All only'],
              ['named', 'Named groups'],
            ])}
          </select>
        </label>
        {form.execution_group_mode === 'named' && (
          <label className="program-policy-field" htmlFor={fieldId(id, 'execution_group_names')}>
            Group names
            <input
              id={fieldId(id, 'execution_group_names')}
              value={form.execution_group_names}
              placeholder="robot_a, all_grippers"
              disabled={busy}
              aria-invalid={errors.execution_groups !== undefined}
              aria-describedby={describedBy('execution_groups')}
              title={errors.execution_groups}
              onChange={(event) => setForm((current) => ({
                ...current, execution_group_names: event.target.value,
              }))}
            />
            <FieldError id={errorId('execution_groups')} message={errors.execution_groups} />
          </label>
        )}
        <p className="program-policy-help">
          Draft memberships: {['all', ...resolvedGroups].join(', ')}.
          {' '}All always includes this node. Named groups replace inherited names
          and may overlap. Saved for the next Run; running membership cannot change.
        </p>
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
