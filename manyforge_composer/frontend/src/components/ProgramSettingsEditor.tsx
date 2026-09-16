import { useEffect, useMemo, useState, type KeyboardEvent } from 'react';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type {
  JsonValue,
  CatalogEntry,
  DraftMutationResponse,
  ProgramAuthoringDiagnostic,
  ProgramAuthoringDocument,
} from '../types/api';
import {
  deriveProgramMotionConfiguration,
  flattenProgramForest,
  PROGRAM_MOTION_FIELDS,
  type ProgramMotionValues,
} from '../utils/programAuthoring';

const LANES = ['manyforge-only', 'isaac-sim', 'real-robot'] as const;
const PORTABLE_ID = /^[a-z][a-z0-9_.-]*$/;

interface ProgramSettingsEditorProps {
  program: ProgramAuthoringDocument;
  catalogEntries: CatalogEntry[];
  candidateRevision: string | null;
  busy: boolean;
  runtime: ProgramRuntimeController;
  onMutate: (
    mutation: string,
    args: Record<string, JsonValue>,
  ) => Promise<DraftMutationResponse | null> | DraftMutationResponse | null | void;
  onInvalidChange?: (invalid: boolean) => void;
  onRoleCreated?: (kind: 'component_role' | 'target_role', id: string) => void;
  diagnostics?: ProgramAuthoringDiagnostic[];
}

function stringList(value: JsonValue | undefined): string[] {
  return Array.isArray(value)
    ? value.filter((item): item is string => typeof item === 'string')
    : [];
}

function parseIdentifierList(value: string, label: string): string[] {
  const entries = [...new Set(value
    .split(/[\s,]+/)
    .map((item) => item.trim())
    .filter(Boolean))].sort();
  const invalid = entries.find((entry) => !PORTABLE_ID.test(entry));
  if (invalid) throw new Error(`${label} contains invalid identifier '${invalid}'.`);
  return entries;
}

function initialCompatibility(program: ProgramAuthoringDocument) {
  const compatibility = program.compatibility ?? {};
  return {
    lanes: stringList(compatibility.lanes),
    assistant: typeof compatibility.assistant === 'string'
      ? compatibility.assistant
      : '',
    requires: stringList(compatibility.requires).join(', '),
    tags: stringList(compatibility.tags).join(', '),
  };
}

function independentExecutionGroups(program: ProgramAuthoringDocument): string {
  return Object.entries(program.execution_groups ?? {})
    .filter(([, definition]) => definition.execution_policy === 'independent')
    .map(([name]) => name)
    .sort()
    .join(', ');
}

type MotionDefaults = ProgramMotionValues;

interface MotionHistory {
  before: JsonValue;
  after: JsonValue;
}

export function ProgramSettingsEditor({
  program,
  catalogEntries,
  candidateRevision,
  busy,
  runtime,
  onMutate,
  onInvalidChange,
  onRoleCreated,
  diagnostics = [],
}: ProgramSettingsEditorProps) {
  const motionConfiguration = useMemo(
    () => deriveProgramMotionConfiguration(program, catalogEntries),
    [catalogEntries, program],
  );
  const appliedMotion = motionConfiguration.available
    ? motionConfiguration.configuration.values
    : null;
  const [name, setName] = useState(program.name);
  const [description, setDescription] = useState(program.description ?? '');
  const [compatibility, setCompatibility] = useState(() => initialCompatibility(program));
  const [executionGroups, setExecutionGroups] = useState(
    () => independentExecutionGroups(program),
  );
  const [error, setError] = useState<string | null>(null);
  const [errorTarget, setErrorTarget] = useState('program-settings-name');
  const [componentRoleId, setComponentRoleId] = useState('');
  const [componentTypeTags, setComponentTypeTags] = useState('');
  const [componentModelRoles, setComponentModelRoles] = useState('');
  const [componentAttachments, setComponentAttachments] = useState('');
  const [targetRoleId, setTargetRoleId] = useState('');
  const [targetComponentRole, setTargetComponentRole] = useState('');
  const [targetCapabilities, setTargetCapabilities] = useState('');
  const [targetFrameRoles, setTargetFrameRoles] = useState('');
  const [targetResourceRoles, setTargetResourceRoles] = useState('');
  const [targetCollisionRoles, setTargetCollisionRoles] = useState('');
  const [targetModelRoles, setTargetModelRoles] = useState('');
  const [motion, setMotion] = useState<MotionDefaults | null>(appliedMotion);
  const [motionHistory, setMotionHistory] = useState<MotionHistory | null>(null);
  const [motionRedo, setMotionRedo] = useState<MotionHistory | null>(null);
  const [motionConflict, setMotionConflict] = useState<{
    values: MotionDefaults;
    count: number;
  } | null>(null);
  const [motionError, setMotionError] = useState<string | null>(null);

  useEffect(() => {
    setName(program.name);
    setDescription(program.description ?? '');
    setCompatibility(initialCompatibility(program));
    setExecutionGroups(independentExecutionGroups(program));
    setError(null);
    setErrorTarget('program-settings-name');
    setComponentRoleId('');
    setComponentTypeTags('');
    setComponentModelRoles('');
    setComponentAttachments('');
    setTargetRoleId('');
    setTargetComponentRole(Object.keys(program.component_roles)[0] ?? '');
    setTargetCapabilities('');
    setTargetFrameRoles('');
    setTargetResourceRoles('');
    setTargetCollisionRoles('');
    setTargetModelRoles('');
    setMotion(appliedMotion);
    setMotionConflict(null);
    setMotionError(null);
  }, [appliedMotion, program]);
  useEffect(() => {
    onInvalidChange?.(error !== null);
    return () => onInvalidChange?.(false);
  }, [error, onInvalidChange]);

  const currentScenario = runtime.state.currentScenario?.currentScenario ?? null;
  const appliedBinding = runtime.state.applied?.programBinding ?? null;
  const exactBinding = currentScenario !== null
    && appliedBinding !== null
    && candidateRevision !== null
    && currentScenario.programRevision === candidateRevision
    && currentScenario.programBindingId === appliedBinding.program_binding_id
    && appliedBinding.program_revision === candidateRevision
    ? appliedBinding
    : null;
  const original = useMemo(() => initialCompatibility(program), [program]);
  const dirty = name.trim() !== program.name
    || description.trim() !== (program.description ?? '')
    || JSON.stringify([...compatibility.lanes].sort()) !== JSON.stringify([...original.lanes].sort())
    || compatibility.assistant !== original.assistant
    || compatibility.requires !== original.requires
    || compatibility.tags !== original.tags
    || executionGroups !== independentExecutionGroups(program);
  const settingsDiagnostics = diagnostics.filter((diagnostic) =>
    diagnostic.path.startsWith('program')
    && !diagnostic.path.startsWith('program.parameters')
    && !diagnostic.path.startsWith('program.blackboard'));
  const compatibilityDiagnostic = settingsDiagnostics.find((diagnostic) =>
    diagnostic.path.startsWith('program.compatibility'));
  const identityDiagnostic = settingsDiagnostics.find((diagnostic) =>
    !diagnostic.path.startsWith('program.compatibility'));

  const submit = () => {
    if (busy || !dirty) return;
    try {
      if (!name.trim()) throw new Error('Program name is required.');
      const lanes = LANES.filter((lane) => compatibility.lanes.includes(lane));
      const requires = parseIdentifierList(compatibility.requires, 'Requirements');
      const tags = parseIdentifierList(compatibility.tags, 'Tags');
      const groups = parseIdentifierList(
        executionGroups,
        'Execution groups',
      );
      const nextCompatibility: Record<string, JsonValue> = {};
      if (lanes.length > 0) nextCompatibility.lanes = lanes;
      if (compatibility.assistant) nextCompatibility.assistant = compatibility.assistant;
      if (requires.length > 0) nextCompatibility.requires = requires;
      if (tags.length > 0) nextCompatibility.tags = tags;
      const preservedDeferredGroups = Object.fromEntries(
        Object.entries(program.execution_groups ?? {}).filter(
          ([, definition]) => definition.execution_policy !== 'independent',
        ),
      );
      onMutate('program_draft_update_settings', {
        name: name.trim(),
        description: description.trim(),
        program_profile: program.program_profile,
        compatibility: nextCompatibility,
        execution_groups: {
          ...preservedDeferredGroups,
          ...Object.fromEntries(groups.map((group) => [
            group,
            { execution_policy: 'independent' },
          ])),
        },
      });
      setError(null);
    } catch (caught) {
      const message = caught instanceof Error ? caught.message : 'Program settings are invalid.';
      setError(message);
      setErrorTarget(
        message.startsWith('Requirements')
          ? 'program-settings-requires'
          : message.startsWith('Tags')
            ? 'program-settings-tags'
            : message.startsWith('Execution groups')
              ? 'program-settings-execution-groups'
            : 'program-settings-name',
      );
    }
  };
  const guardInvalidTab = (event: KeyboardEvent<HTMLFormElement>) => {
    if (event.key !== 'Tab') return;
    try {
      if (!name.trim()) throw new Error('Program name is required.');
      parseIdentifierList(compatibility.requires, 'Requirements');
      parseIdentifierList(compatibility.tags, 'Tags');
      parseIdentifierList(executionGroups, 'Execution groups');
    } catch (caught) {
      const message = caught instanceof Error ? caught.message : 'Program settings are invalid.';
      const target = message.startsWith('Requirements')
        ? 'program-settings-requires'
        : message.startsWith('Tags')
          ? 'program-settings-tags'
          : message.startsWith('Execution groups')
            ? 'program-settings-execution-groups'
          : 'program-settings-name';
      event.preventDefault();
      setError(message);
      setErrorTarget(target);
      document.getElementById(target)?.focus();
    }
  };
  const replaceRoles = (
    componentRoles: Record<string, JsonValue>,
    targetRoles: Record<string, JsonValue>,
  ) => Promise.resolve(onMutate('program_draft_replace_roles', {
    component_roles: componentRoles,
    target_roles: targetRoles,
  }));
  const addComponentRole = () => {
    try {
      const roleId = componentRoleId.trim();
      if (!PORTABLE_ID.test(roleId)) throw new Error('Component role needs a portable ID.');
      if (program.component_roles[roleId] !== undefined) {
        throw new Error(`Component role '${roleId}' already exists.`);
      }
      const typeTags = parseIdentifierList(componentTypeTags, 'Component type tags');
      const modelRoles = parseIdentifierList(componentModelRoles, 'Component model roles');
      const attachments = parseIdentifierList(componentAttachments, 'Attachment interfaces');
      const role: Record<string, JsonValue> = {};
      if (typeTags.length) role.required_component_type_tags = typeTags;
      if (modelRoles.length) role.required_model_roles = modelRoles;
      if (attachments.length) role.required_attachment_interfaces = attachments;
      void replaceRoles(
        { ...program.component_roles, [roleId]: role },
        program.target_roles,
      ).then((response) => {
        if (!response) return;
        onRoleCreated?.('component_role', roleId);
        setComponentRoleId('');
        setComponentTypeTags('');
        setComponentModelRoles('');
        setComponentAttachments('');
        setTargetComponentRole(roleId);
      });
      setError(null);
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : 'Component role is invalid.');
      setErrorTarget('program-component-role-id');
    }
  };
  const addTargetRole = () => {
    try {
      const roleId = targetRoleId.trim();
      if (!PORTABLE_ID.test(roleId)) throw new Error('Target role needs a portable ID.');
      if (program.target_roles[roleId] !== undefined) {
        throw new Error(`Target role '${roleId}' already exists.`);
      }
      if (!targetComponentRole || program.component_roles[targetComponentRole] === undefined) {
        throw new Error('Target role must select a declared component role.');
      }
      const capabilities = parseIdentifierList(targetCapabilities, 'Target capabilities');
      if (capabilities.length === 0) throw new Error('Target role needs at least one capability.');
      const role: Record<string, JsonValue> = {
        component_role: targetComponentRole,
        required_capabilities: capabilities.map((capabilityId) => ({
          capability_id: capabilityId,
          parameters: {},
          disposition: 'required',
        })),
      };
      const optionalLists = {
        required_frame_roles: parseIdentifierList(targetFrameRoles, 'Frame roles'),
        required_resource_roles: parseIdentifierList(targetResourceRoles, 'Resource roles'),
        required_collision_body_roles: parseIdentifierList(targetCollisionRoles, 'Collision-body roles'),
        required_model_roles: parseIdentifierList(targetModelRoles, 'Target model roles'),
      };
      for (const [name, values] of Object.entries(optionalLists)) {
        if (values.length) role[name] = values;
      }
      void replaceRoles(
        program.component_roles,
        { ...program.target_roles, [roleId]: role },
      ).then((response) => {
        if (!response) return;
        onRoleCreated?.('target_role', roleId);
        setTargetRoleId('');
        setTargetCapabilities('');
        setTargetFrameRoles('');
        setTargetResourceRoles('');
        setTargetCollisionRoles('');
        setTargetModelRoles('');
      });
      setError(null);
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : 'Target role is invalid.');
      setErrorTarget('program-target-role-id');
    }
  };
  const motionDirty = motion !== null && appliedMotion !== null
    && PROGRAM_MOTION_FIELDS.some((field) => (
    Math.abs(
      motion[field]
      - appliedMotion[field],
    ) > 1e-12
    ));
  const motionConflictCount = (values: MotionDefaults): number => (
    flattenProgramForest(program.forest).reduce((count, { node }) => (
      count + PROGRAM_MOTION_FIELDS.filter((field) => {
        const value = node.parameters[field];
        return typeof value === 'number'
          && value > values[field];
      }).length
    ), 0)
  );
  const performMotionUpdate = async (
    values: MotionDefaults,
    conflictResolution: 'reject' | 'reduce',
  ) => {
    if (!motionConfiguration.available) {
      setMotionError(motionConfiguration.reason);
      return;
    }
    const response = await onMutate('program_draft_update_motion_defaults', {
      mode: 'update',
      velocity_scale: values.velocity_scale,
      acceleration_scale: values.acceleration_scale,
      max_tcp_linear_speed_mps: values.max_tcp_linear_speed_mps,
      parameter_bindings: motionConfiguration.configuration.bindings,
      conflict_resolution: conflictResolution,
    });
    if (!response) return;
    const before = response.details.previous_config;
    const after = response.details.current_config;
    if (before !== undefined && after !== undefined) {
      setMotionHistory({ before, after });
      setMotionRedo(null);
    }
    setMotionConflict(null);
    setMotionError(null);
  };
  const submitMotion = () => {
    if (busy || !motionDirty || motion === null) return;
    if (
      !Number.isFinite(motion.velocity_scale)
      || motion.velocity_scale <= 0
      || motion.velocity_scale > 1
      || !Number.isFinite(motion.acceleration_scale)
      || motion.acceleration_scale <= 0
      || motion.acceleration_scale > 1
      || !Number.isFinite(motion.max_tcp_linear_speed_mps)
      || motion.max_tcp_linear_speed_mps <= 0
    ) {
      setMotionError('Velocity and acceleration scales must be in (0, 1]; TCP speed must be positive.');
      return;
    }
    const count = motionConflictCount(motion);
    if (count > 0) {
      setMotionConflict({ values: motion, count });
      return;
    }
    void performMotionUpdate(motion, 'reject');
  };
  const restoreMotion = async (history: MotionHistory, direction: 'undo' | 'redo') => {
    const response = await onMutate('program_draft_update_motion_defaults', {
      mode: 'restore',
      config: direction === 'undo' ? history.before : history.after,
    });
    if (!response) return;
    if (direction === 'undo') {
      setMotionHistory(null);
      setMotionRedo(history);
    } else {
      setMotionHistory(history);
      setMotionRedo(null);
    }
  };

  return (
    <form className="program-settings-editor" onSubmit={(event) => {
      event.preventDefault();
      submit();
    }} onKeyDown={guardInvalidTab} onBlur={submit}>
      {error && (
        <div className="program-invalid-edit-strip" role="alert">
          <span>{error}</span>
          <button type="button" onClick={() => document.getElementById(errorTarget)?.focus()}>
            Review
          </button>
          <button
            type="button"
            onClick={() => {
              setName(program.name);
              setDescription(program.description ?? '');
              setCompatibility(initialCompatibility(program));
              setExecutionGroups(independentExecutionGroups(program));
              setComponentRoleId('');
              setComponentTypeTags('');
              setComponentModelRoles('');
              setComponentAttachments('');
              setTargetRoleId('');
              setTargetComponentRole(Object.keys(program.component_roles)[0] ?? '');
              setTargetCapabilities('');
              setTargetFrameRoles('');
              setTargetResourceRoles('');
              setTargetCollisionRoles('');
              setTargetModelRoles('');
              setError(null);
              setErrorTarget('program-settings-name');
            }}
          >
            Discard edit
          </button>
        </div>
      )}
      {settingsDiagnostics.length > 0 && (
        <div className="program-authoring-diagnostics" role="status">
          <strong>Complete Program settings</strong>
          {settingsDiagnostics.slice(0, 6).map((diagnostic) => (
            <span key={`${diagnostic.code}:${diagnostic.path}`}>{diagnostic.message}</span>
          ))}
        </div>
      )}
      <section className={`program-settings-section${identityDiagnostic ? ' backend-invalid' : ''}`} title={identityDiagnostic?.message}>
        <h3>Identity and profile</h3>
        <dl className="program-settings-facts">
          <div><dt>Program ID</dt><dd><code>{program.program_id}</code></dd></div>
          <div><dt>Profile</dt><dd><code>{program.program_profile}</code></dd></div>
          <div><dt>Catalog</dt><dd><code>{program.catalog.content_version}</code></dd></div>
        </dl>
        <label>
          Program name
          <input id="program-settings-name" value={name} disabled={busy} aria-invalid={error !== null && errorTarget === 'program-settings-name'} title={errorTarget === 'program-settings-name' ? error ?? undefined : undefined} onChange={(event) => setName(event.target.value)} />
        </label>
        <label>
          Description
          <textarea rows={3} value={description} disabled={busy} onChange={(event) => setDescription(event.target.value)} />
        </label>
      </section>

      <section className="program-settings-section">
        <h3>Execution timing</h3>
        <label>
          Control groups
          <input
            id="program-settings-execution-groups"
            value={executionGroups}
            disabled={busy}
            placeholder="group_a, group_b"
            title="Named groups for Run, Stop, Resume and velocity scaling. Members plan and execute independently. Synchronized policies are added in F2."
            onChange={(event) => setExecutionGroups(event.target.value)}
          />
        </label>
      </section>

      <section className="program-settings-section program-motion-defaults">
        <h3>Program motion limits</h3>
        <p>
          These are portable Program ceilings. Motion nodes default to them;
          lower per-node values remain valid. The live command-bar Velocity
          control further scales current and subsequent compatible moves for
          the addressed target without changing these Program values.
        </p>
        {motionError && <p className="program-settings-error" role="alert">{motionError}</p>}
        {!motionConfiguration.available || motion === null ? (
          <p className="program-settings-error" role="status">
            Program-wide motion controls are unavailable. {motionConfiguration.available
              ? 'The current values could not be read.'
              : motionConfiguration.reason}
          </p>
        ) : (
          <>
            <dl className="program-settings-facts program-motion-bindings">
              {PROGRAM_MOTION_FIELDS.map((field) => (
                <div key={field}>
                  <dt>{field}</dt>
                  <dd>
                    <code>{motionConfiguration.configuration.bindings[field]}</code>
                    {' · '}{motionConfiguration.configuration.usageCounts[field]}
                    /{motionConfiguration.configuration.applicableCounts[field]} bound nodes
                  </dd>
                </div>
              ))}
            </dl>
            <div className="program-motion-default-grid">
              <label>
                Global velocity scale
                <input
                  aria-label="Program global velocity scale"
                  type="number"
                  min="0.01"
                  max="1"
                  step="0.01"
                  value={motion.velocity_scale}
                  disabled={busy}
                  onChange={(event) => setMotion((current) => current === null ? null : ({
                    ...current,
                    velocity_scale: event.currentTarget.valueAsNumber,
                  }))}
                />
              </label>
              <label>
                Global acceleration scale
                <input
                  aria-label="Program global acceleration scale"
                  type="number"
                  min="0.01"
                  max="1"
                  step="0.01"
                  value={motion.acceleration_scale}
                  disabled={busy}
                  onChange={(event) => setMotion((current) => current === null ? null : ({
                    ...current,
                    acceleration_scale: event.currentTarget.valueAsNumber,
                  }))}
                />
              </label>
              <label>
                Maximum TCP linear speed
                <span className="program-motion-number">
                  <input
                    aria-label="Program maximum TCP linear speed"
                    type="number"
                    min="0.001"
                    step="0.001"
                    value={motion.max_tcp_linear_speed_mps}
                    disabled={busy}
                    onChange={(event) => setMotion((current) => current === null ? null : ({
                      ...current,
                      max_tcp_linear_speed_mps: event.currentTarget.valueAsNumber,
                    }))}
                  />
                  <span>m/s</span>
                </span>
              </label>
            </div>
            <div className="program-motion-actions">
              <button type="button" disabled={busy || !motionDirty} onClick={submitMotion}>
                Update motion limits
              </button>
              <button
                type="button"
                disabled={busy || motionHistory === null}
                onClick={() => motionHistory && void restoreMotion(motionHistory, 'undo')}
              >Undo</button>
              <button
                type="button"
                disabled={busy || motionRedo === null}
                onClick={() => motionRedo && void restoreMotion(motionRedo, 'redo')}
              >Redo</button>
            </div>
          </>
        )}
      </section>

      <section className={`program-settings-section${compatibilityDiagnostic ? ' backend-invalid' : ''}`} title={compatibilityDiagnostic?.message}>
        <h3>Portable compatibility</h3>
        <fieldset className="program-settings-lanes">
          <legend>Supported lanes</legend>
          {LANES.map((lane) => (
            <label key={lane}>
              <input
                type="checkbox"
                checked={compatibility.lanes.includes(lane)}
                disabled={busy}
                onChange={(event) => setCompatibility((current) => ({
                  ...current,
                  lanes: event.target.checked
                    ? [...current.lanes, lane]
                    : current.lanes.filter((item) => item !== lane),
                }))}
              />
              {lane}
            </label>
          ))}
        </fieldset>
        <label>
          Assistant disposition
          <select
            value={compatibility.assistant}
            disabled={busy}
            onChange={(event) => setCompatibility((current) => ({
              ...current,
              assistant: event.target.value,
            }))}
          >
            <option value="">Not specified</option>
            <option value="required">Required</option>
            <option value="optional">Optional</option>
            <option value="forbidden">Forbidden</option>
          </select>
        </label>
        <label>
          Portable semantic requirements
          <input
            id="program-settings-requires"
            value={compatibility.requires}
            disabled={busy}
            aria-invalid={error !== null && errorTarget === 'program-settings-requires'}
            title={errorTarget === 'program-settings-requires' ? error ?? undefined : undefined}
            placeholder="requirement.one, requirement.two"
            onChange={(event) => setCompatibility((current) => ({
              ...current,
              requires: event.target.value,
            }))}
          />
        </label>
        <label>
          Discovery tags
          <input
            id="program-settings-tags"
            value={compatibility.tags}
            disabled={busy}
            aria-invalid={error !== null && errorTarget === 'program-settings-tags'}
            title={errorTarget === 'program-settings-tags' ? error ?? undefined : undefined}
            placeholder="pick, inspection"
            onChange={(event) => setCompatibility((current) => ({
              ...current,
              tags: event.target.value,
            }))}
          />
        </label>
      </section>

      <section className="program-settings-section program-binding-projection">
        <h3>Current owner-issued Binding</h3>
        {exactBinding ? (
          <dl className="program-settings-facts">
            <div><dt>Scenario</dt><dd><code>{currentScenario?.scenarioId}</code></dd></div>
            <div><dt>Binding</dt><dd><code>{exactBinding.program_binding_id}</code></dd></div>
            <div><dt>Component roles</dt><dd>{exactBinding.component_role_bindings.length}</dd></div>
            <div><dt>Target roles</dt><dd>{exactBinding.target_role_bindings.length}</dd></div>
          </dl>
        ) : (
          <p>
            This Draft has no exact current owner-issued Binding projection.
            Program settings do not infer compatibility in the browser.
          </p>
        )}
      </section>

      <section className="program-settings-section program-role-requirements">
        <h3>Portable component and target roles</h3>
        <p>
          Node selectors use these logical requirements. Deployment Binding chooses
          implementations later; this Program does not name live components.
        </p>
        <dl className="program-settings-facts">
          {Object.keys(program.component_roles).map((role) => (
            <div key={`component:${role}`}><dt>Component role</dt><dd><code>{role}</code></dd></div>
          ))}
          {Object.keys(program.target_roles).map((role) => (
            <div key={`target:${role}`}><dt>Target role</dt><dd><code>{role}</code></dd></div>
          ))}
        </dl>
        <details>
          <summary>Create component role</summary>
          <div className="program-role-create-form">
            <label>Role ID<input id="program-component-role-id" value={componentRoleId} disabled={busy} onChange={(event) => setComponentRoleId(event.target.value)} /></label>
            <label>Required component type tags<input value={componentTypeTags} disabled={busy} placeholder="articulated-arm, gripper" onChange={(event) => setComponentTypeTags(event.target.value)} /></label>
            <label>Required model roles<input value={componentModelRoles} disabled={busy} placeholder="arm_model" onChange={(event) => setComponentModelRoles(event.target.value)} /></label>
            <label>Required attachment interfaces<input value={componentAttachments} disabled={busy} onChange={(event) => setComponentAttachments(event.target.value)} /></label>
            <button type="button" disabled={busy || !componentRoleId.trim()} onClick={addComponentRole}>Create component role</button>
          </div>
        </details>
        <details>
          <summary>Create target role</summary>
          <div className="program-role-create-form">
            <label>Role ID<input id="program-target-role-id" value={targetRoleId} disabled={busy} onChange={(event) => setTargetRoleId(event.target.value)} /></label>
            <label>Component role<select value={targetComponentRole} disabled={busy} onChange={(event) => setTargetComponentRole(event.target.value)}><option value="" disabled>Select component role</option>{Object.keys(program.component_roles).map((role) => <option key={role}>{role}</option>)}</select></label>
            <label>Required capabilities<input value={targetCapabilities} disabled={busy} placeholder="planning.motion.pose_goal" onChange={(event) => setTargetCapabilities(event.target.value)} /></label>
            <label>Frame roles<input value={targetFrameRoles} disabled={busy} placeholder="base, tcp" onChange={(event) => setTargetFrameRoles(event.target.value)} /></label>
            <label>Resource roles<input value={targetResourceRoles} disabled={busy} onChange={(event) => setTargetResourceRoles(event.target.value)} /></label>
            <label>Collision-body roles<input value={targetCollisionRoles} disabled={busy} onChange={(event) => setTargetCollisionRoles(event.target.value)} /></label>
            <label>Model roles<input value={targetModelRoles} disabled={busy} onChange={(event) => setTargetModelRoles(event.target.value)} /></label>
            <button type="button" disabled={busy || !targetRoleId.trim()} onClick={addTargetRole}>Create target role</button>
          </div>
        </details>
      </section>

      {motionConflict && (
        <div className="program-motion-conflict-backdrop" role="presentation">
          <section
            className="program-motion-conflict"
            role="dialog"
            aria-modal="true"
            aria-labelledby="program-motion-conflict-title"
          >
            <h3 id="program-motion-conflict-title">Node limits exceed the new Program ceiling</h3>
            <p>
              {motionConflict.count} node {motionConflict.count === 1 ? 'value is' : 'values are'}
              {' '}higher than the proposed global limit.
            </p>
            <div>
              <button
                type="button"
                onClick={() => void performMotionUpdate(motionConflict.values, 'reduce')}
              >Reduce node limits</button>
              <button
                type="button"
                onClick={() => {
                  setMotion(appliedMotion);
                  setMotionConflict(null);
                }}
              >Discard new setting</button>
            </div>
          </section>
        </div>
      )}

    </form>
  );
}
