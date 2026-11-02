import { useEffect, useState } from 'react';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';

const GENERATED_FIELDS = new Set([
  'api_version',
  'deployment_revision',
  'graph_revision',
  'control_domain_revision',
  'control_binding_revision',
  'component_attachment_revision',
  'capability_provider_binding_revision',
]);

function label(value: string): string {
  return value.replaceAll('_', ' ').replace(/^./, (head) => head.toUpperCase());
}

interface ScalarEditorProps {
  field: string;
  path: Array<string | number>;
  value: string | number | boolean | null;
  controller: DeploymentWorkspaceController;
  forceReadOnly?: boolean;
}

function ScalarEditor({
  field,
  path,
  value,
  controller,
  forceReadOnly = false,
}: ScalarEditorProps) {
  const [draft, setDraft] = useState(value === null ? '' : String(value));
  const [invalid, setInvalid] = useState(false);
  const readOnly = forceReadOnly || GENERATED_FIELDS.has(field);
  useEffect(() => {
    setDraft(value === null ? '' : String(value));
    setInvalid(false);
  }, [value]);

  const discard = () => {
    setDraft(value === null ? '' : String(value));
    setInvalid(false);
  };
  const commit = async () => {
    if (readOnly || draft === String(value ?? '')) return;
    let next: JsonValue = draft;
    if (typeof value === 'number') {
      if (draft.trim() === '') {
        setInvalid(true);
        return;
      }
      const parsed = Number(draft);
      if (!Number.isFinite(parsed)) {
        setInvalid(true);
        return;
      }
      next = parsed;
    } else if (typeof value === 'boolean') {
      next = draft === 'true';
    } else if (value === null) {
      next = draft === 'null' ? null : draft;
    }
    const result = await controller.mutate(path, 'set', next);
    setInvalid(result === null);
  };

  return (
    <label className={`deployment-field${invalid ? ' invalid' : ''}`}>
      <span>{label(field)}{readOnly && <small>compiler-owned</small>}</span>
      {typeof value === 'boolean' ? (
        <select
          value={draft}
          disabled={readOnly || !controller.mutationAvailable || controller.busy}
          onChange={(event) => {
            setDraft(event.target.value);
            setInvalid(false);
          }}
          onBlur={() => { void commit(); }}
        >
          <option value="true">Enabled</option>
          <option value="false">Disabled</option>
        </select>
      ) : (
        <input
          type={typeof value === 'number' ? 'number' : 'text'}
          value={draft}
          readOnly={readOnly}
          disabled={!readOnly && (!controller.mutationAvailable || controller.busy)}
          onChange={(event) => {
            setDraft(event.target.value);
            setInvalid(false);
          }}
          onBlur={() => { void commit(); }}
          onKeyDown={(event) => {
            if (event.key === 'Enter') event.currentTarget.blur();
            if (event.key === 'Escape') {
              event.preventDefault();
              discard();
              event.currentTarget.blur();
            }
          }}
        />
      )}
      {invalid && (
        <span className="deployment-field-error inline-message severity-error">
          Invalid value. Correct it or <button type="button" onClick={discard}>discard edit</button>.
        </span>
      )}
    </label>
  );
}

interface ValueEditorProps {
  name: string;
  path: Array<string | number>;
  value: JsonValue;
  controller: DeploymentWorkspaceController;
  depth?: number;
  allowRemove?: boolean;
  readOnly?: boolean;
}

function ValueEditor({
  name,
  path,
  value,
  controller,
  depth = 0,
  allowRemove = false,
  readOnly = false,
}: ValueEditorProps) {
  if (value === null || typeof value !== 'object') {
    return (
      <ScalarEditor
        field={name}
        path={path}
        value={value}
        controller={controller}
        forceReadOnly={readOnly}
      />
    );
  }
  if (Array.isArray(value)) {
    return (
      <details className="deployment-collection" open={depth < 1}>
        <summary>{label(name)} <span>{value.length}</span></summary>
        <div className="deployment-collection-body">
          {value.length === 0 && <p className="muted">No configured entries.</p>}
          {value.map((entry, index) => (
            <div className="deployment-array-row" key={`${path.join('.')}:${index}`}>
              <ValueEditor
                name={`${label(name)} ${index + 1}`}
                path={[...path, index]}
                value={entry}
                controller={controller}
                depth={depth + 1}
                allowRemove
                readOnly={readOnly}
              />
              {!readOnly && <div className="deployment-row-actions">
                <button
                  type="button"
                  disabled={!controller.mutationAvailable || controller.busy}
                  onClick={() => { void controller.mutate(path, 'append', entry); }}
                >
                  Duplicate
                </button>
                <button
                  type="button"
                  className="danger-button"
                  disabled={!controller.mutationAvailable || controller.busy}
                  onClick={() => { void controller.mutate([...path, index], 'remove'); }}
                >
                  Remove
                </button>
              </div>}
            </div>
          ))}
        </div>
      </details>
    );
  }
  return (
    <details className="deployment-object" open={depth < 1}>
      <summary>
        {label(name)}
        {allowRemove && <span>{Object.keys(value).length} fields</span>}
      </summary>
      <div className="deployment-object-body">
        {Object.entries(value).map(([field, entry]) => (
          <ValueEditor
            key={field}
            name={field}
            path={[...path, field]}
            value={entry}
            controller={controller}
            depth={depth + 1}
            readOnly={readOnly}
          />
        ))}
      </div>
    </details>
  );
}

const PREFERRED_SECTIONS = [
  'graph',
  'desired_control_configurations',
  'capability_provider_bindings',
  'catalogs',
  'policies',
  'assistant',
  'external_resources',
  'site_overlay_ref',
] as const;

const IDENTITY_FIELDS = [
  'api_version', 'name', 'description', 'cell_id',
  'deployment_artifact_id', 'deployment_revision', 'compatibility',
];

export function DeploymentDocumentEditor({
  controller,
  readOnly = false,
}: {
  controller: DeploymentWorkspaceController;
  readOnly?: boolean;
}) {
  const document = controller.workspace?.draft.document ?? null;
  const [section, setSection] = useState<string>('identity');
  if (document === null) {
    return <div className="panel-state">Open a Deployment artifact to edit its Draft.</div>;
  }
  const reserved = new Set<string>([
    ...IDENTITY_FIELDS,
    ...PREFERRED_SECTIONS,
  ]);
  const available = [
    'identity',
    ...PREFERRED_SECTIONS.filter((key) => (
      Object.prototype.hasOwnProperty.call(document, key)
    )),
    ...Object.keys(document).filter((key) => !reserved.has(key)).sort(),
  ];
  const identity = Object.fromEntries(
    IDENTITY_FIELDS.flatMap((key) => key in document ? [[key, document[key]]] : []),
  ) as Record<string, JsonValue>;
  const activeSection = available.includes(section) ? section : 'identity';
  const selected = activeSection === 'identity' ? identity : document[activeSection];
  return (
    <section className="deployment-editor" aria-labelledby="deployment-editor-title">
      <header className="scene-authoring-heading">
        <div>
          <p className="eyebrow">Compiler-validated Draft</p>
          <h3 id="deployment-editor-title">Deployment editor</h3>
        </div>
        <select value={activeSection} onChange={(event) => setSection(event.target.value)}>
          {available.map((key) => <option key={key} value={key}>{label(key)}</option>)}
        </select>
      </header>
      {selected !== undefined && (
        <ValueEditor
          name={activeSection}
          path={activeSection === 'identity' ? [] : [activeSection]}
          value={selected}
          controller={controller}
          readOnly={readOnly}
        />
      )}
    </section>
  );
}
