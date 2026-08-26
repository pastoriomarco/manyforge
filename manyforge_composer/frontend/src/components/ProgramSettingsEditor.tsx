import { useEffect, useMemo, useState, type KeyboardEvent } from 'react';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type {
  JsonValue,
  ProgramAuthoringDiagnostic,
  ProgramAuthoringDocument,
} from '../types/api';

const LANES = ['manyforge-only', 'isaac-sim', 'real-robot'] as const;
const PORTABLE_ID = /^[a-z][a-z0-9_.-]*$/;

interface ProgramSettingsEditorProps {
  program: ProgramAuthoringDocument;
  candidateRevision: string | null;
  busy: boolean;
  runtime: ProgramRuntimeController;
  onMutate: (mutation: string, args: Record<string, JsonValue>) => void;
  onInvalidChange?: (invalid: boolean) => void;
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

export function ProgramSettingsEditor({
  program,
  candidateRevision,
  busy,
  runtime,
  onMutate,
  onInvalidChange,
  diagnostics = [],
}: ProgramSettingsEditorProps) {
  const [name, setName] = useState(program.name);
  const [description, setDescription] = useState(program.description ?? '');
  const [compatibility, setCompatibility] = useState(() => initialCompatibility(program));
  const [error, setError] = useState<string | null>(null);
  const [errorTarget, setErrorTarget] = useState('program-settings-name');

  useEffect(() => {
    setName(program.name);
    setDescription(program.description ?? '');
    setCompatibility(initialCompatibility(program));
    setError(null);
    setErrorTarget('program-settings-name');
  }, [program]);
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
    || compatibility.tags !== original.tags;
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
      const nextCompatibility: Record<string, JsonValue> = {};
      if (lanes.length > 0) nextCompatibility.lanes = lanes;
      if (compatibility.assistant) nextCompatibility.assistant = compatibility.assistant;
      if (requires.length > 0) nextCompatibility.requires = requires;
      if (tags.length > 0) nextCompatibility.tags = tags;
      onMutate('program_draft_update_settings', {
        name: name.trim(),
        description: description.trim(),
        program_profile: program.program_profile,
        compatibility: nextCompatibility,
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
    } catch (caught) {
      const message = caught instanceof Error ? caught.message : 'Program settings are invalid.';
      const target = message.startsWith('Requirements')
        ? 'program-settings-requires'
        : message.startsWith('Tags')
          ? 'program-settings-tags'
          : 'program-settings-name';
      event.preventDefault();
      setError(message);
      setErrorTarget(target);
      document.getElementById(target)?.focus();
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

    </form>
  );
}
