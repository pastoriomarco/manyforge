import { useDeploymentStore } from '../store/deploymentStore';
import { useCycleStore } from '../store/cycleStore';

function signature(value: unknown): string {
  return JSON.stringify(value ?? null);
}

function FieldInput({
  label,
  value,
  onChange,
  type = 'text',
  disabled = false,
}: {
  label: string;
  value: string | number;
  onChange: (v: string) => void;
  type?: string;
  disabled?: boolean;
}) {
  return (
    <label className="block mb-2">
      <span className="text-xs text-gray-400">{label}</span>
      <input
        type={type}
        value={value}
        onChange={(e) => onChange(e.target.value)}
        disabled={disabled}
        className="w-full mt-0.5 px-2 py-1 bg-gray-700 border border-gray-600 rounded text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:opacity-50 disabled:cursor-not-allowed"
      />
    </label>
  );
}

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div>
      <h3 className="text-xs font-semibold text-gray-400 uppercase tracking-wide mb-1">
        {title}
      </h3>
      <div className="space-y-1">{children}</div>
    </div>
  );
}

function ConfigTextarea({
  label,
  value,
  onChange,
  disabled = false,
}: {
  label: string;
  value: Record<string, unknown>;
  onChange: (v: Record<string, unknown>) => void;
  disabled?: boolean;
}) {
  return (
    <label className="block mb-2">
      <span className="text-xs text-gray-400">{label}</span>
      <textarea
        rows={3}
        defaultValue={JSON.stringify(value, null, 2)}
        disabled={disabled}
        onBlur={(e) => {
          try {
            onChange(JSON.parse(e.target.value));
          } catch {
            // invalid JSON — keep current value
          }
        }}
        className="w-full mt-0.5 px-2 py-1 bg-gray-700 border border-gray-600 rounded text-xs text-gray-200 font-mono focus:border-blue-500 focus:outline-none disabled:opacity-50 disabled:cursor-not-allowed"
      />
    </label>
  );
}

export function DeploymentPanel() {
  const {
    deployment,
    appliedDeployment,
    savedDeployment,
    sourcePath,
    saveDeployment,
    setField,
    setNestedField,
  } =
    useDeploymentStore();
  const { state } = useCycleStore();
  const editLocked = state === 'running';
  const runtimeApplied = signature(deployment) === signature(appliedDeployment);
  const diskSaved = signature(deployment) === signature(savedDeployment);

  if (!deployment) {
    return (
      <div className="p-4 text-gray-500 text-sm">
        No deployment artifact loaded into Composer.
      </div>
    );
  }

  return (
    <div className="flex-1 overflow-y-auto p-3 space-y-4">
      <div className="rounded border border-gray-700 bg-gray-900/60 p-3 text-xs text-gray-300 space-y-1">
        <div>
          <span className="text-gray-500">Loaded from:</span>{' '}
          <span className="font-mono">{sourcePath || '<unspecified>'}</span>
        </div>
        <div className="flex gap-2 pt-1">
          <span className={`rounded px-2 py-1 font-mono ${runtimeApplied ? 'bg-emerald-700 text-white' : 'bg-orange-700 text-white'}`}>
            {runtimeApplied ? 'APPLIED' : 'UNAPPLIED'}
          </span>
          <span className={`rounded px-2 py-1 font-mono ${diskSaved ? 'bg-slate-700 text-white' : 'bg-amber-700 text-white'}`}>
            {diskSaved ? 'SAVED' : 'UNSAVED'}
          </span>
          {editLocked && (
            <span className="rounded bg-yellow-800 px-2 py-1 font-mono text-yellow-100">
              READ ONLY WHILE RUNNING
            </span>
          )}
        </div>
      </div>

      <Section title="General">
        <FieldInput
          label="Name"
          value={deployment.name}
          onChange={(v) => setField('name', v)}
          disabled={editLocked}
        />
        <FieldInput
          label="Description"
          value={deployment.description}
          onChange={(v) => setField('description', v)}
          disabled={editLocked}
        />
        <FieldInput
          label="Program Profile"
          value={deployment.programProfile}
          onChange={(v) => setField('programProfile', v)}
          disabled={editLocked}
        />
      </Section>

      <Section title="Robot Config">
        <FieldInput
          label="Robot ID"
          value={deployment.robotConfig.robotId}
          onChange={(v) => setNestedField('robotConfig.robotId', v)}
          disabled={editLocked}
        />
        <FieldInput
          label="Group ID"
          value={deployment.robotConfig.groupId}
          onChange={(v) => setNestedField('robotConfig.groupId', v)}
          disabled={editLocked}
        />
        <FieldInput
          label="URDF Path"
          value={deployment.robotConfig.urdfPath}
          onChange={(v) => setNestedField('robotConfig.urdfPath', v)}
          disabled={editLocked}
        />
        <FieldInput
          label="Max TCP Speed (m/s)"
          value={deployment.robotConfig.globalMaxTcpLinearSpeedMps}
          onChange={(v) =>
            setNestedField('robotConfig.globalMaxTcpLinearSpeedMps', parseFloat(v) || 0)
          }
          type="number"
          disabled={editLocked}
        />
      </Section>

      {Object.entries(deployment.adapters).map(([slot, adapter]) => (
        <Section key={slot} title={`Adapter: ${slot}`}>
          <FieldInput
            label="Adapter ID"
            value={adapter.adapterId}
            onChange={(v) => setNestedField(`adapters.${slot}.adapterId`, v)}
            disabled={editLocked}
          />
          <FieldInput
            label="Version"
            value={adapter.adapterVersion}
            onChange={(v) => setNestedField(`adapters.${slot}.adapterVersion`, v)}
            disabled={editLocked}
          />
          <ConfigTextarea
            label="Config"
            value={adapter.config}
            onChange={(v) => setNestedField(`adapters.${slot}.config`, v)}
            disabled={editLocked}
          />
        </Section>
      ))}

      <Section title="Orchestrator">
        <FieldInput
          label="Engine"
          value={deployment.orchestrator.engine}
          onChange={(v) => setNestedField('orchestrator.engine', v)}
          disabled={editLocked}
        />
        <ConfigTextarea
          label="Config"
          value={deployment.orchestrator.config}
          onChange={(v) => setNestedField('orchestrator.config', v)}
          disabled={editLocked}
        />
      </Section>

      <Section title="Launch">
        <FieldInput
          label="Runner Module"
          value={deployment.launch.runner.module}
          onChange={(v) => setNestedField('launch.runner.module', v)}
          disabled={editLocked}
        />
        {deployment.launch.uiBackend && (
          <>
            <FieldInput
              label="UI Backend Module"
              value={deployment.launch.uiBackend.module}
              onChange={(v) => setNestedField('launch.uiBackend.module', v)}
              disabled={editLocked}
            />
          </>
        )}
      </Section>

      <button
        onClick={() => sourcePath && saveDeployment(sourcePath)}
        disabled={diskSaved || editLocked || !sourcePath}
        className="w-full px-3 py-2 bg-amber-600 hover:bg-amber-500 disabled:bg-gray-600 disabled:cursor-not-allowed text-white text-sm font-medium rounded"
      >
        Save Deployment Artifact
      </button>
    </div>
  );
}
