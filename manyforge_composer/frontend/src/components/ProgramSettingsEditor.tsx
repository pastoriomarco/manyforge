import { useEffect, useMemo, useState } from 'react';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { JsonValue, ProgramDocument } from '../types/api';

const LANES = ['manyforge-only', 'isaac-sim', 'real-robot'] as const;
const PORTABLE_ID = /^[a-z][a-z0-9_.-]*$/;

interface ProgramSettingsEditorProps {
  program: ProgramDocument;
  busy: boolean;
  runtime: ProgramRuntimeController;
  onMutate: (mutation: string, args: Record<string, JsonValue>) => void;
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

function initialCompatibility(program: ProgramDocument) {
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
  busy,
  runtime,
  onMutate,
}: ProgramSettingsEditorProps) {
  const [name, setName] = useState(program.name);
  const [description, setDescription] = useState(program.description ?? '');
  const [compatibility, setCompatibility] = useState(() => initialCompatibility(program));
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    setName(program.name);
    setDescription(program.description ?? '');
    setCompatibility(initialCompatibility(program));
    setError(null);
  }, [program]);

  const currentScenario = runtime.state.currentScenario?.currentScenario ?? null;
  const appliedBinding = runtime.state.applied?.programBinding ?? null;
  const exactBinding = currentScenario !== null
    && appliedBinding !== null
    && currentScenario.programRevision === program.program_revision
    && currentScenario.programBindingId === appliedBinding.program_binding_id
    && appliedBinding.program_revision === program.program_revision
    ? appliedBinding
    : null;
  const original = useMemo(() => initialCompatibility(program), [program]);
  const dirty = name.trim() !== program.name
    || description.trim() !== (program.description ?? '')
    || JSON.stringify([...compatibility.lanes].sort()) !== JSON.stringify([...original.lanes].sort())
    || compatibility.assistant !== original.assistant
    || compatibility.requires !== original.requires
    || compatibility.tags !== original.tags;

  const submit = () => {
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
      setError(caught instanceof Error ? caught.message : 'Program settings are invalid.');
    }
  };

  return (
    <form className="program-settings-editor" onSubmit={(event) => {
      event.preventDefault();
      submit();
    }}>
      <section className="program-settings-section">
        <h3>Identity and profile</h3>
        <dl className="program-settings-facts">
          <div><dt>Program ID</dt><dd><code>{program.program_id}</code></dd></div>
          <div><dt>Profile</dt><dd><code>{program.program_profile}</code></dd></div>
          <div><dt>Catalog</dt><dd><code>{program.catalog.content_version}</code></dd></div>
        </dl>
        <label>
          Program name
          <input value={name} disabled={busy} onChange={(event) => setName(event.target.value)} />
        </label>
        <label>
          Description
          <textarea rows={3} value={description} disabled={busy} onChange={(event) => setDescription(event.target.value)} />
        </label>
      </section>

      <section className="program-settings-section">
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
            value={compatibility.requires}
            disabled={busy}
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
            value={compatibility.tags}
            disabled={busy}
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

      {error && <p className="program-form-error" role="alert">{error}</p>}
      <button type="submit" className="primary-action" disabled={busy || !dirty}>
        Save Program settings to Draft
      </button>
    </form>
  );
}
