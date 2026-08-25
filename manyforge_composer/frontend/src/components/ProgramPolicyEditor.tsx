import { useEffect, useMemo, useState } from 'react';

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
  | 'operator_stop_policy';

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
    setError(
      errors,
      'recovery_policy.allowed_skills',
      hasIntervention
        ? ALLOWED_SKILLS_UNAVAILABLE_MESSAGE
        : `Allowed skills require the Intervention recovery action. ${ALLOWED_SKILLS_UNAVAILABLE_MESSAGE}`,
    );
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
  return <p id={id} className="program-form-error" role="alert">{message}</p>;
}

function selectOptions<T extends string>(options: Array<[T, string]>) {
  return options.map(([value, label]) => <option key={value} value={value}>{label}</option>);
}

interface ProgramPolicyEditorProps {
  node: ProgramTreeNode;
  busy: boolean;
  onUpdate: (
    instanceId: string,
    modificationPolicy: ProgramNodeModificationPolicy,
    recoveryPolicy: ProgramNodeRecoveryPolicy,
    operatorStopPolicy: ProgramOperatorStopPolicy,
  ) => void;
}

export function ProgramPolicyEditor({
  node,
  busy,
  onUpdate,
}: ProgramPolicyEditorProps) {
  const [form, setForm] = useState<ProgramPolicyForm>(() => effectiveProgramPolicy(node));

  useEffect(() => {
    setForm(effectiveProgramPolicy(node));
  }, [node]);

  const errors = useMemo(() => validateProgramPolicy(form), [form]);
  const id = node.instance_id;
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
        onSubmit={(event) => {
          event.preventDefault();
          if (Object.keys(errors).length > 0) return;
          onUpdate(
            id,
            { ...form.modification_policy },
            {
              ...form.recovery_policy,
              allowed_recovery_actions: [
                ...form.recovery_policy.allowed_recovery_actions,
              ],
              allowed_skills: [...form.recovery_policy.allowed_skills],
            },
            form.operator_stop_policy,
          );
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
          Stop enforcement remains a later runtime capability; this is authored
          policy for future Run behavior, not a change to a running Run.
        </p>
      </fieldset>

      <div className="program-policy-actions-row">
        <button
          type="submit"
          disabled={busy || Object.keys(errors).length > 0}
        >
          {busy ? 'Saving node policy…' : 'Save node policy to Draft'}
        </button>
        {Object.keys(errors).length > 0 && (
          <span className="program-policy-blocked" role="status">
            Resolve the field guidance before saving this policy.
          </span>
        )}
      </div>
      </form>
    </section>
  );
}
