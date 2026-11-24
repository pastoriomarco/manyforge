import { useEffect, useState } from 'react';

import type { JsonValue } from '../types/api';
import {
  COLLISION_RULE_LEGS,
  COLLISION_RULE_SELECTION_LABELS,
  DEFAULT_COLLISION_CLEARANCE_M,
  DEFAULT_CONTACT_END_FROM_FRACTION,
  MAX_COLLISION_CLEARANCE_M,
  MAX_COLLISION_RULE_SELECTIONS,
  MAX_COLLISION_RULES,
  SIGNED_RELEASE_DEFAULTS,
  collisionRuleRecord,
  collisionRuleRows,
  collisionRuleSelectionKinds,
  collisionRuleValueKind,
  collisionRulesProblem,
  defaultCollisionRuleSelection,
  everythingClearance,
  everythingRowIndex,
  isEverythingRow,
  isSpringSliderRow,
  newCollisionRule,
  orderedCollisionRule,
  releaseJson,
  releaseValueOf,
  withEverythingClearance,
  withSpringSlider,
  type CollisionReleaseValue,
  type CollisionRuleChoices,
  type CollisionRuleLeg,
  type CollisionRuleScope,
  type CollisionRuleSelectionKind,
  type CollisionRuleValueKind,
  type ReleaseEvidenceMode,
} from '../utils/collisionRules';
import { DraftTextInput } from './common/DraftInputs';

type JsonRecord = Record<string, JsonValue>;

const VALUE_LABELS: Record<CollisionRuleValueKind, string> = {
  clearance_m: 'Clearance',
  contact: 'May touch',
  release: 'Release (starts in contact, must separate)',
};

/** The value a move rule picks to switch a generated tool's spring slider on (OD-371). */
const SPRING_SLIDER_VALUE = 'spring_slider';
const SPRING_SLIDER_LABEL = 'Spring slider pressed in (stroke and step from the slider)';

const LEG_LABELS: Record<CollisionRuleLeg, string> = {
  approach: 'Approach',
  descend: 'Descend',
  retreat: 'Retreat',
};

const RELEASE_METRIC_FIELDS = [
  ['maxStartPenetrationM', 'Maximum starting penetration'],
  ['releaseToleranceM', 'Release tolerance'],
  ['minReleaseProgressM', 'Minimum release progress'],
] as const;

function numberText(value: JsonValue | undefined): string {
  return typeof value === 'number' && Number.isFinite(value) ? String(value) : '';
}

/** An id chosen from a known list, or typed while the list is unknown or empty. */
function IdField({ label, value, options, disabled, onCommit }: {
  label: string;
  value: string;
  options: readonly string[] | null;
  disabled: boolean;
  onCommit: (value: string) => void;
}) {
  if (options === null || options.length === 0) {
    return (
      <DraftTextInput aria-label={label} placeholder={label} value={value} disabled={disabled}
        validate={(text) => (text.trim() === '' ? 'Enter an id.' : null)}
        onCommit={(text) => onCommit(text.trim())} />
    );
  }
  return (
    <select aria-label={label} value={value} disabled={disabled}
      onChange={(event) => onCommit(event.target.value)}>
      {!options.includes(value) && (
        <option value={value}>{value === '' ? 'Select' : `${value} (not listed)`}</option>
      )}
      {options.map((option) => <option key={option} value={option}>{option}</option>)}
    </select>
  );
}

/** A number in meters or a fraction; empty clears it when `optional`. */
function RuleNumber({ label, ariaLabel, value, unit, min = 0, max, optional = false, placeholder, disabled, onCommit }: {
  label: string;
  ariaLabel: string;
  value: JsonValue | undefined;
  unit?: string;
  min?: number;
  max?: number;
  optional?: boolean;
  placeholder?: string;
  disabled: boolean;
  onCommit: (value: number | null) => void;
}) {
  const range = max === undefined ? `${min} or more` : `from ${min} to ${max}`;
  return (
    <label className="deployment-focused-field">
      <span>{label}</span>
      <span className="deployment-number-with-unit">
        <DraftTextInput type="number" step="any" min={min} max={max} aria-label={ariaLabel}
          placeholder={placeholder} value={numberText(value)} disabled={disabled}
          validate={(text) => {
            if (text.trim() === '') return optional ? null : `Enter a number ${range}.`;
            const parsed = Number(text);
            return Number.isFinite(parsed) && parsed >= min && (max === undefined || parsed <= max)
              ? null : `Enter a number ${range}.`;
          }}
          onCommit={(text) => onCommit(text.trim() === '' ? null : Number(text))} />
        {unit !== undefined && <small>{unit}</small>}
      </span>
    </label>
  );
}

function RuleCheck({ label, ariaLabel, checked, disabled, onChange }: {
  label: string;
  ariaLabel: string;
  checked: boolean;
  disabled: boolean;
  onChange: (checked: boolean) => void;
}) {
  return (
    <label className="collision-rule-check">
      <input type="checkbox" aria-label={ariaLabel} checked={checked} disabled={disabled}
        onChange={(event) => onChange(event.target.checked)} />
      <span>{label}</span>
    </label>
  );
}

function SelectionFields({ label, selection, choices, disabled, onChange }: {
  label: string;
  selection: JsonRecord;
  choices: CollisionRuleChoices;
  disabled: boolean;
  onChange: (next: JsonRecord) => void;
}) {
  const text = (key: string) => (typeof selection[key] === 'string' ? selection[key] as string : '');
  const component = (
    <IdField label={`${label} component`} value={text('component_id')} options={choices.componentIds}
      disabled={disabled} onCommit={(value) => onChange({ ...selection, component_id: value })} />
  );
  switch (selection.kind) {
    case 'component':
      return component;
    case 'attached_object': {
      // Every object the component carries, or only those one of its bodies (a spring slider) carries.
      const componentId = text('component_id');
      const role = text('collision_body_role_id');
      const known = (choices.bodies ?? []).filter((body) => body.component_id === componentId)
        .map((body) => body.collision_body_id);
      const withRole = (next: string): JsonRecord => {
        const rest: JsonRecord = { ...selection };
        delete rest.collision_body_role_id;
        return next === '' ? rest : { ...rest, collision_body_role_id: next };
      };
      return (
        <>
          <IdField label={`${label} component`} value={componentId} options={choices.componentIds}
            disabled={disabled}
            onCommit={(value) => onChange({ ...withRole(''), component_id: value })} />
          <select aria-label={`${label} carried by body`} value={role} disabled={disabled}
            onChange={(event) => onChange(withRole(event.target.value))}>
            <option value="">Any body</option>
            {role !== '' && !known.includes(role) && <option value={role}>{`${role} (not listed)`}</option>}
            {known.map((body) => <option key={body} value={body}>{body}</option>)}
          </select>
        </>
      );
    }
    case 'collision_body': {
      const componentId = text('component_id');
      const known = choices.bodies === null ? null : choices.bodies
        .filter((body) => body.component_id === componentId)
        .map((body) => body.collision_body_id);
      return (
        <>
          <IdField label={`${label} component`} value={componentId} options={choices.componentIds}
            disabled={disabled}
            onCommit={(value) => onChange({
              ...selection,
              component_id: value,
              // A body belongs to one component: the first body of the new one, if known.
              collision_body_id: choices.bodies?.find((body) => body.component_id === value)
                ?.collision_body_id ?? '',
            })} />
          <IdField label={`${label} collision body`} value={text('collision_body_id')} options={known}
            disabled={disabled} onCommit={(value) => onChange({ ...selection, collision_body_id: value })} />
        </>
      );
    }
    case 'link':
    case 'model_group': {
      // Picked from the cell model's names of the chosen component; typed only
      // while the model does not list the component.
      const key = selection.kind === 'link' ? 'link' : 'group';
      const rows = selection.kind === 'link'
        ? (choices.links ?? []).map((row) => ({ component_id: row.component_id, name: row.link }))
        : (choices.modelGroups ?? []).map((row) => ({ component_id: row.component_id, name: row.group }));
      const namesOf = (componentId: string) => rows
        .filter((row) => row.component_id === componentId).map((row) => row.name);
      const componentId = text('component_id');
      const known = namesOf(componentId);
      const fieldLabel = selection.kind === 'link' ? 'link' : 'model group';
      return (
        <>
          <IdField label={`${label} component`} value={componentId} options={choices.componentIds}
            disabled={disabled}
            onCommit={(value) => onChange({ ...selection, component_id: value, [key]: namesOf(value)[0] ?? '' })} />
          {known.length > 0 ? (
            <IdField label={`${label} ${fieldLabel}`} value={text(key)} options={known}
              disabled={disabled} onCommit={(value) => onChange({ ...selection, [key]: value })} />
          ) : (
            <DraftTextInput aria-label={`${label} ${fieldLabel}`}
              placeholder={selection.kind === 'link' ? 'link name in the cell model' : 'SRDF group name'}
              value={text(key)} disabled={disabled}
              validate={(value) => (value.trim() === '' ? `Enter the ${fieldLabel} name.` : null)}
              onCommit={(value) => onChange({ ...selection, [key]: value.trim() })} />
          )}
        </>
      );
    }
    case 'scene_object': {
      // One Scene object, a key of type scene_resource, or a scene_resource field of a structured key.
      const encode = (parts: readonly string[]) => JSON.stringify(parts);
      const options = [
        ...(choices.sceneObjectIds ?? []).map((id) => ({ value: encode(['id', id]), label: id })),
        ...(choices.sceneResourceKeys ?? []).map((key) => ({ value: encode(['key', key]), label: `Key ${key}` })),
        ...(choices.sceneResourceKeyFields ?? []).map((row) => ({
          value: encode(['field', row.key, row.field]), label: `Key ${row.key} · ${row.field}`,
        })),
      ];
      const key = typeof selection.blackboard_key === 'string' ? selection.blackboard_key : null;
      const field = typeof selection.field === 'string' ? selection.field : null;
      const current = key === null ? encode(['id', text('scene_resource_id')])
        : field === null ? encode(['key', key]) : encode(['field', key, field]);
      const currentLabel = key === null ? text('scene_resource_id')
        : `Key ${key}${field === null ? '' : ` · ${field}`}`;
      return (
        <select aria-label={`${label} Scene object`} value={current} disabled={disabled}
          onChange={(event) => {
            const [kind, first, second] = JSON.parse(event.target.value) as string[];
            onChange(kind === 'id' ? { kind: 'scene_object', scene_resource_id: first }
              : kind === 'key' ? { kind: 'scene_object', blackboard_key: first }
                : { kind: 'scene_object', blackboard_key: first, field: second });
          }}>
          {!options.some((option) => option.value === current) && (
            <option value={current}>{currentLabel === '' ? 'Select' : `${currentLabel} (not listed)`}</option>
          )}
          {options.map((option) => <option key={option.value} value={option.value}>{option.label}</option>)}
        </select>
      );
    }
    default:
      return null;
  }
}

/** The tool and the spring slider a rule switches on; the slider is one of the tool's collision bodies. */
function SpringSliderFields({ ruleId, slider, choices, disabled, onChange }: {
  ruleId: string;
  slider: JsonRecord;
  choices: CollisionRuleChoices;
  disabled: boolean;
  onChange: (next: JsonRecord) => void;
}) {
  const componentId = typeof slider.component_id === 'string' ? slider.component_id : '';
  const elementId = typeof slider.element_id === 'string' ? slider.element_id : '';
  const bodiesOf = (component: string) => (choices.bodies === null ? null : choices.bodies
    .filter((body) => body.component_id === component).map((body) => body.collision_body_id));
  return (
    <fieldset className="deployment-focused-field" aria-label={`${ruleId} spring slider`}>
      <legend>Spring slider (with the objects it carries)</legend>
      <div className="collision-rule-selection">
        <IdField label={`${ruleId} spring slider tool`} value={componentId} options={choices.componentIds}
          disabled={disabled}
          onCommit={(value) => onChange({ component_id: value, element_id: bodiesOf(value)?.[0] ?? '' })} />
        <IdField label={`${ruleId} spring slider`} value={elementId} options={bodiesOf(componentId)}
          disabled={disabled} onCommit={(value) => onChange({ component_id: componentId, element_id: value })} />
      </div>
    </fieldset>
  );
}

function SelectionList({ ruleId, side, selections, scope, choices, disabled, onChange }: {
  ruleId: string;
  side: 'a' | 'b';
  selections: JsonValue[];
  scope: CollisionRuleScope;
  choices: CollisionRuleChoices;
  disabled: boolean;
  onChange: (next: JsonValue[]) => void;
}) {
  const sideLabel = `Selection ${side.toUpperCase()}`;
  const kinds = collisionRuleSelectionKinds(scope);
  return (
    <fieldset className="deployment-focused-field" aria-label={`${ruleId} ${sideLabel}`}>
      <legend>{sideLabel}</legend>
      {selections.map((value, index) => {
        const selection = collisionRuleRecord(value) ?? {};
        const kind = typeof selection.kind === 'string' ? selection.kind : '';
        const label = `${ruleId} ${side.toUpperCase()}${index + 1}`;
        const replace = (next: JsonRecord) => onChange(
          selections.map((item, at) => (at === index ? next : item)),
        );
        return (
          <div className="collision-rule-selection" key={index}>
            <select aria-label={`${label} kind`} value={kind} disabled={disabled}
              onChange={(event) => replace(defaultCollisionRuleSelection(
                event.target.value as CollisionRuleSelectionKind, choices,
              ))}>
              {!(kinds as readonly string[]).includes(kind) && (
                <option value={kind}>{kind === '' ? 'Select' : `${kind} (not available here)`}</option>
              )}
              {kinds.map((item) => (
                <option key={item} value={item}>{COLLISION_RULE_SELECTION_LABELS[item]}</option>
              ))}
            </select>
            <SelectionFields label={label} selection={selection} choices={choices} disabled={disabled}
              onChange={replace} />
            {selections.length > 1 && (
              <button type="button" className="text-button" disabled={disabled}
                aria-label={`Remove ${label}`}
                onClick={() => onChange(selections.filter((_, at) => at !== index))}>Remove</button>
            )}
          </div>
        );
      })}
      <button type="button" className="text-button"
        disabled={disabled || selections.length >= MAX_COLLISION_RULE_SELECTIONS}
        aria-label={`Add to ${ruleId} ${sideLabel}`}
        onClick={() => onChange([...selections, { kind: 'scene_objects' }])}>Add selection</button>
    </fieldset>
  );
}

function ReleaseFields({ ruleId, release, disabled, onChange }: {
  ruleId: string;
  release: CollisionReleaseValue;
  disabled: boolean;
  onChange: (next: CollisionReleaseValue) => void;
}) {
  const signed = release.evidence === 'signed_penetration';
  return (
    <>
      <label className="deployment-focused-field">
        <span>Validation evidence</span>
        <select aria-label={`${ruleId} validation evidence`} value={release.evidence} disabled={disabled}
          onChange={(event) => {
            const evidence = event.target.value as ReleaseEvidenceMode;
            // A precise release starts from its defaults; a lightweight one has no penetration metrics.
            onChange(evidence === 'signed_penetration' ? {
              ...SIGNED_RELEASE_DEFAULTS,
              requireSeparationByEnd: release.requireSeparationByEnd,
              allowRecontact: release.allowRecontact,
            } : { ...release, evidence });
          }}>
          <option value="signed_penetration">Precise release</option>
          <option value="binary_contact">Lightweight release</option>
        </select>
      </label>
      {signed && (
        <>
          <RuleCheck label="Limit starting penetration" ariaLabel={`${ruleId} limit starting penetration`}
            checked={release.enforceMaxStartPenetration} disabled={disabled}
            onChange={(checked) => onChange({ ...release, enforceMaxStartPenetration: checked })} />
          {RELEASE_METRIC_FIELDS
            .filter(([field]) => field !== 'maxStartPenetrationM' || release.enforceMaxStartPenetration)
            .map(([field, label]) => (
              <RuleNumber key={field} label={label} ariaLabel={`${ruleId} ${label.toLowerCase()}`} unit="m"
                value={release[field]} disabled={disabled}
                onCommit={(value) => { if (value !== null) onChange({ ...release, [field]: value }); }} />
            ))}
        </>
      )}
      <div className="collision-rule-checks">
        <RuleCheck label="Require separation by end" ariaLabel={`${ruleId} require separation by end`}
          checked={release.requireSeparationByEnd} disabled={disabled}
          onChange={(checked) => onChange({ ...release, requireSeparationByEnd: checked })} />
        {signed && (
          <RuleCheck label="Require decreasing penetration"
            ariaLabel={`${ruleId} require decreasing penetration`}
            checked={release.requireStrictlyDecreasingPenetration} disabled={disabled}
            onChange={(checked) => onChange({ ...release, requireStrictlyDecreasingPenetration: checked })} />
        )}
        <RuleCheck label="Allow re-contact" ariaLabel={`${ruleId} allow re-contact`}
          checked={release.allowRecontact} disabled={disabled}
          onChange={(checked) => onChange({ ...release, allowRecontact: checked })} />
      </div>
    </>
  );
}

function ValueFields({ ruleId, rule, scope, defaultClearanceM, disabled, onChange, onSpringSlider }: {
  ruleId: string;
  rule: JsonRecord;
  scope: CollisionRuleScope;
  defaultClearanceM: number;
  disabled: boolean;
  /** Replaces the rule's value key. */
  onChange: (kind: CollisionRuleValueKind, value: JsonValue) => void;
  /** Turns the rule into a spring slider rule, or back. */
  onSpringSlider: (spring: boolean) => void;
}) {
  const spring = isSpringSliderRow(rule);
  const kind = spring ? 'contact' : collisionRuleValueKind(rule);
  const contact = collisionRuleRecord(rule.contact) ?? {};
  // A contact value without a fraction may touch from the default fraction on; 0 is the whole move.
  const endFromFraction: JsonValue = contact.end_from_fraction ?? DEFAULT_CONTACT_END_FROM_FRACTION;
  const wholeMove = endFromFraction === 0;
  const release = releaseValueOf(rule.release);
  const setContact = (patch: Record<string, number | null>) => {
    const next: JsonRecord = { ...contact };
    for (const [key, value] of Object.entries(patch)) {
      if (value === null) delete next[key];
      else next[key] = value;
    }
    onChange('contact', next);
  };
  return (
    <>
      <label className="deployment-focused-field">
        <span>Value</span>
        <select aria-label={`${ruleId} value`} value={spring ? SPRING_SLIDER_VALUE : kind ?? ''}
          disabled={disabled}
          onChange={(event) => {
            const next = event.target.value;
            if (next === SPRING_SLIDER_VALUE) onSpringSlider(true);
            else if (next === 'clearance_m') onChange(next, defaultClearanceM);
            else if (next === 'contact') {
              if (spring) onSpringSlider(false);
              else onChange(next, {});
            } else onChange('release', releaseJson(SIGNED_RELEASE_DEFAULTS));
          }}>
          {kind === null && <option value="">Select</option>}
          {(Object.keys(VALUE_LABELS) as CollisionRuleValueKind[]).map((item) => (
            <option key={item} value={item}>{VALUE_LABELS[item]}</option>
          ))}
          {scope !== 'deployment' && <option value={SPRING_SLIDER_VALUE}>{SPRING_SLIDER_LABEL}</option>}
        </select>
      </label>
      {kind === 'clearance_m' && (
        <RuleNumber label="Clearance" ariaLabel={`${ruleId} clearance`} unit="m"
          max={MAX_COLLISION_CLEARANCE_M} value={rule.clearance_m} disabled={disabled}
          onCommit={(value) => { if (value !== null) onChange('clearance_m', value); }} />
      )}
      {kind === 'contact' && (
        <>
          {!spring && (
            <>
              <RuleNumber label="Deepest penetration" ariaLabel={`${ruleId} deepest penetration`} unit="m"
                optional placeholder="not checked" value={contact.max_penetration_m} disabled={disabled}
                onCommit={(value) => setContact(value === null
                  // Without the stroke there is no spring: the step limit goes with it.
                  ? { max_penetration_m: null, max_compression_step_m: null }
                  : { max_penetration_m: value })} />
              {contact.max_penetration_m !== undefined && (
                // A spring (OD-356): compression may not jump by more than this between samples.
                <RuleNumber label="Spring: largest compression step" ariaLabel={`${ruleId} spring compression step`}
                  unit="m" optional placeholder="not a spring" min={0.000001}
                  value={contact.max_compression_step_m} disabled={disabled}
                  onCommit={(value) => setContact({ max_compression_step_m: value })} />
              )}
            </>
          )}
          <RuleCheck label="Only at the end of the move" ariaLabel={`${ruleId} only at the end of the move`}
            checked={!wholeMove} disabled={disabled}
            onChange={(checked) => setContact({
              end_from_fraction: checked ? DEFAULT_CONTACT_END_FROM_FRACTION : 0,
            })} />
          {!wholeMove && (
            <RuleNumber label="From fraction of the move" ariaLabel={`${ruleId} end from fraction`}
              max={1} value={endFromFraction} disabled={disabled}
              onCommit={(value) => { if (value !== null) setContact({ end_from_fraction: value }); }} />
          )}
          <p className="field-help">
            {wholeMove
              ? 'The pair may touch during the whole move, from its first sample.'
              : 'The pair may touch only from this fraction of the move on; contact at the start or before it is refused.'}
            {spring
              ? ' The slider and the objects it carries may press in together up to the slider\'s stroke.'
              : contact.max_penetration_m === undefined ? ' The depth is not checked.' : ''}
          </p>
        </>
      )}
      {kind === 'release' && (release === null ? (
        <p className="deployment-field-error inline-message severity-error">
          The release value has a field of the wrong type; choose the value again to replace it.
        </p>
      ) : (
        <ReleaseFields ruleId={ruleId} release={release} disabled={disabled}
          onChange={(next) => onChange('release', releaseJson(next))} />
      ))}
    </>
  );
}

/**
 * The collision rules of a Deployment (cell rules) or of one motion node or
 * pick/place sequence (move rules). The first row, "Everything else", is the
 * rule without selections and holds a clearance; every other row gives one
 * value to the pairs of its two selections. An edit is written when the whole
 * list is valid; until then it stays here with the reason shown.
 */
export function CollisionRulesTable({ value, scope, choices, disabled, onCommit }: {
  value: JsonValue | undefined;
  scope: CollisionRuleScope;
  choices: CollisionRuleChoices;
  disabled: boolean;
  onCommit: (rows: JsonValue[]) => void;
}) {
  const incoming = collisionRuleRows(value);
  const serialized = JSON.stringify(incoming);
  const [rows, setRows] = useState<JsonValue[]>(() => incoming ?? []);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    setRows((JSON.parse(serialized) as JsonValue[] | null) ?? []);
    setError(null);
  }, [serialized]);

  if (incoming === null) {
    return (
      <p className="deployment-field-error inline-message severity-error">
        collision_rules is not a list; it is kept unchanged and cannot be edited here.
      </p>
    );
  }

  const apply = (next: JsonValue[]) => {
    setRows(next);
    const problem = collisionRulesProblem(next, scope);
    setError(problem);
    if (problem === null) onCommit(next);
  };
  const replaceRule = (index: number, rule: JsonRecord) => apply(
    rows.map((row, at) => (at === index ? orderedCollisionRule(rule) : row)),
  );
  const everythingIndex = everythingRowIndex(rows);
  const everything = collisionRuleRecord(rows[everythingIndex]);
  const everythingM = everythingClearance(rows);
  const defaultClearanceM = everythingM ?? DEFAULT_COLLISION_CLEARANCE_M;
  // A sequence has no clearance of its own today: its row is shown only when the list holds one.
  const showEverything = scope !== 'sequence' || everything !== null;

  return (
    <div className="collision-rules" aria-label="Collision rules">
      {showEverything && (
        <div className="deployment-focused-form-grid">
          <RuleNumber label="Everything else: clearance" ariaLabel="Everything else clearance" unit="m"
            max={MAX_COLLISION_CLEARANCE_M} optional={scope !== 'deployment'}
            placeholder={scope === 'deployment' ? undefined : 'Deployment value'}
            value={everythingM ?? undefined} disabled={disabled}
            onCommit={(clearance) => apply(withEverythingClearance(rows, clearance))} />
          {scope === 'sequence' && everything !== null && (
            <label className="deployment-focused-field">
              <span>Leg</span>
              <select aria-label="Everything else leg"
                value={typeof everything.leg === 'string' ? everything.leg : ''} disabled={disabled}
                onChange={(event) => replaceRule(everythingIndex, { ...everything, leg: event.target.value })}>
                {!(COLLISION_RULE_LEGS as readonly string[]).includes(everything.leg as string)
                  && <option value="">Select</option>}
                {COLLISION_RULE_LEGS.map((leg) => <option key={leg} value={leg}>{LEG_LABELS[leg]}</option>)}
              </select>
            </label>
          )}
        </div>
      )}
      <p className="field-help">
        {scope === 'deployment'
          ? 'Everything else is the clearance every robot keeps from the Scene objects (a carried object counts as part of its robot); 0 turns it off. Each rule below gives its own value to the pairs of its two selections.'
          : scope === 'node'
            ? 'Everything else replaces the Deployment\'s clearance for this move; left empty, the move keeps the Deployment value. Each rule below replaces the value of exactly its pairs for this move.'
            : 'Each rule applies to one leg of the sequence and replaces the value of exactly its pairs on that leg.'}
      </p>
      {rows.map((row, index) => {
        if (isEverythingRow(row)) return null;
        const rule = collisionRuleRecord(row) ?? {};
        const ruleId = typeof rule.rule_id === 'string' && rule.rule_id !== ''
          ? rule.rule_id : `rule ${index + 1}`;
        const setValue = (kind: CollisionRuleValueKind, next: JsonValue) => {
          // A clearance or release ends a spring slider rule; its contact keeps it.
          const replaced: JsonRecord = isSpringSliderRow(rule) && kind !== 'contact'
            ? withSpringSlider(rule, false, choices) : { ...rule };
          delete replaced.clearance_m;
          delete replaced.contact;
          delete replaced.release;
          replaced[kind] = next;
          replaceRule(index, replaced);
        };
        return (
          <fieldset className="deployment-gripper-editor" key={index} aria-label={`Collision rule ${ruleId}`}>
            <legend>{ruleId}</legend>
            <div className="deployment-focused-form-grid">
              <label className="deployment-focused-field">
                <span>Rule id</span>
                <DraftTextInput aria-label={`${ruleId} rule id`}
                  value={typeof rule.rule_id === 'string' ? rule.rule_id : ''} disabled={disabled}
                  validate={(text) => (text.trim() === '' ? 'Enter a rule id.' : null)}
                  onCommit={(text) => replaceRule(index, { ...rule, rule_id: text.trim() })} />
              </label>
              {scope === 'sequence' && (
                <label className="deployment-focused-field">
                  <span>Leg</span>
                  <select aria-label={`${ruleId} leg`} value={typeof rule.leg === 'string' ? rule.leg : ''}
                    disabled={disabled}
                    onChange={(event) => replaceRule(index, { ...rule, leg: event.target.value })}>
                    {!(COLLISION_RULE_LEGS as readonly string[]).includes(rule.leg as string)
                      && <option value="">Select</option>}
                    {COLLISION_RULE_LEGS.map((leg) => <option key={leg} value={leg}>{LEG_LABELS[leg]}</option>)}
                  </select>
                </label>
              )}
              {isSpringSliderRow(rule) && (
                <SpringSliderFields ruleId={ruleId} slider={collisionRuleRecord(rule.spring_slider) ?? {}}
                  choices={choices} disabled={disabled}
                  onChange={(next) => replaceRule(index, { ...rule, spring_slider: next })} />
              )}
              {(isSpringSliderRow(rule) ? ['b'] as const : ['a', 'b'] as const).map((side) => (
                <SelectionList key={side} ruleId={ruleId} side={side}
                  selections={Array.isArray(rule[side]) ? rule[side] as JsonValue[] : []}
                  scope={scope} choices={choices} disabled={disabled}
                  onChange={(next) => replaceRule(index, { ...rule, [side]: next })} />
              ))}
              <ValueFields ruleId={ruleId} rule={rule} scope={scope} defaultClearanceM={defaultClearanceM}
                disabled={disabled} onChange={setValue}
                onSpringSlider={(spring) => replaceRule(index, withSpringSlider(rule, spring, choices))} />
            </div>
            <div className="button-row">
              <button type="button" disabled={disabled}
                onClick={() => apply(rows.filter((_, at) => at !== index))}>Remove collision rule</button>
            </div>
          </fieldset>
        );
      })}
      {error !== null && (
        <p className="deployment-field-error inline-message severity-error" role="alert">{error}</p>
      )}
      <div className="button-row">
        <button type="button" disabled={disabled || rows.length >= MAX_COLLISION_RULES}
          onClick={() => apply([...rows, newCollisionRule(rows, scope, choices)])}>Add collision rule</button>
      </div>
    </div>
  );
}
