import { useDeploymentStore } from '../store/deploymentStore';

function FieldInput({
  label,
  value,
  onChange,
  type = 'text',
}: {
  label: string;
  value: string | number;
  onChange: (v: string) => void;
  type?: string;
}) {
  return (
    <label className="block mb-2">
      <span className="text-xs text-gray-400">{label}</span>
      <input
        type={type}
        value={value}
        onChange={(e) => onChange(e.target.value)}
        className="w-full mt-0.5 px-2 py-1 bg-gray-700 border border-gray-600 rounded text-sm text-gray-200 focus:border-blue-500 focus:outline-none"
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
}: {
  label: string;
  value: Record<string, unknown>;
  onChange: (v: Record<string, unknown>) => void;
}) {
  return (
    <label className="block mb-2">
      <span className="text-xs text-gray-400">{label}</span>
      <textarea
        rows={3}
        defaultValue={JSON.stringify(value, null, 2)}
        onBlur={(e) => {
          try {
            onChange(JSON.parse(e.target.value));
          } catch {
            // invalid JSON — keep current value
          }
        }}
        className="w-full mt-0.5 px-2 py-1 bg-gray-700 border border-gray-600 rounded text-xs text-gray-200 font-mono focus:border-blue-500 focus:outline-none"
      />
    </label>
  );
}

export function DeploymentPanel() {
  const { deployment, isDirty, saveDeployment, setField, setNestedField } =
    useDeploymentStore();

  if (!deployment) {
    return (
      <div className="p-4 text-gray-500 text-sm">
        No deployment loaded. Load a program with a deployment path first.
      </div>
    );
  }

  return (
    <div className="flex-1 overflow-y-auto p-3 space-y-4">
      <Section title="General">
        <FieldInput
          label="Name"
          value={deployment.name}
          onChange={(v) => setField('name', v)}
        />
        <FieldInput
          label="Description"
          value={deployment.description}
          onChange={(v) => setField('description', v)}
        />
        <FieldInput
          label="Program Profile"
          value={deployment.programProfile}
          onChange={(v) => setField('programProfile', v)}
        />
      </Section>

      <Section title="Robot Config">
        <FieldInput
          label="Robot ID"
          value={deployment.robotConfig.robotId}
          onChange={(v) => setNestedField('robotConfig.robotId', v)}
        />
        <FieldInput
          label="Group ID"
          value={deployment.robotConfig.groupId}
          onChange={(v) => setNestedField('robotConfig.groupId', v)}
        />
        <FieldInput
          label="URDF Path"
          value={deployment.robotConfig.urdfPath}
          onChange={(v) => setNestedField('robotConfig.urdfPath', v)}
        />
        <FieldInput
          label="Max TCP Speed (m/s)"
          value={deployment.robotConfig.globalMaxTcpLinearSpeedMps}
          onChange={(v) =>
            setNestedField('robotConfig.globalMaxTcpLinearSpeedMps', parseFloat(v) || 0)
          }
          type="number"
        />
      </Section>

      {Object.entries(deployment.adapters).map(([slot, adapter]) => (
        <Section key={slot} title={`Adapter: ${slot}`}>
          <FieldInput
            label="Adapter ID"
            value={adapter.adapterId}
            onChange={(v) => setNestedField(`adapters.${slot}.adapterId`, v)}
          />
          <FieldInput
            label="Version"
            value={adapter.adapterVersion}
            onChange={(v) => setNestedField(`adapters.${slot}.adapterVersion`, v)}
          />
          <ConfigTextarea
            label="Config"
            value={adapter.config}
            onChange={(v) => setNestedField(`adapters.${slot}.config`, v)}
          />
        </Section>
      ))}

      <Section title="Orchestrator">
        <FieldInput
          label="Engine"
          value={deployment.orchestrator.engine}
          onChange={(v) => setNestedField('orchestrator.engine', v)}
        />
        <ConfigTextarea
          label="Config"
          value={deployment.orchestrator.config}
          onChange={(v) => setNestedField('orchestrator.config', v)}
        />
      </Section>

      <Section title="Launch">
        <FieldInput
          label="Runner Module"
          value={deployment.launch.runner.module}
          onChange={(v) => setNestedField('launch.runner.module', v)}
        />
        {deployment.launch.uiBackend && (
          <>
            <FieldInput
              label="UI Backend Module"
              value={deployment.launch.uiBackend.module}
              onChange={(v) => setNestedField('launch.uiBackend.module', v)}
            />
          </>
        )}
      </Section>

      <button
        onClick={() => saveDeployment('deployment.yaml')}
        disabled={!isDirty}
        className="w-full px-3 py-2 bg-amber-600 hover:bg-amber-500 disabled:bg-gray-600 disabled:cursor-not-allowed text-white text-sm font-medium rounded"
      >
        Save Deployment
      </button>
    </div>
  );
}
