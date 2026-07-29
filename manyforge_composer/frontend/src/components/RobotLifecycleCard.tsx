import type { RobotLifecycleFact } from '../types/api';
import {
  useRobotLifecyclePolling,
  useRobotLifecycleStore,
} from '../store/robotLifecycleStore';

function Fact({
  label,
  value,
}: {
  label: string;
  value: RobotLifecycleFact | undefined;
}) {
  const color =
    value === 'true'
      ? 'bg-emerald-400'
      : value === 'false'
        ? 'bg-gray-500'
        : 'bg-amber-400';
  return (
    <div className="flex items-center justify-between gap-2">
      <span className="text-gray-400">{label}</span>
      <span className="flex items-center gap-1 font-mono text-[10px] text-gray-200">
        <span className={`h-1.5 w-1.5 rounded-full ${color}`} />
        {value ?? 'unknown'}
      </span>
    </div>
  );
}

export function RobotLifecycleCard() {
  useRobotLifecyclePolling();
  const status = useRobotLifecycleStore((state) => state.status);
  const loading = useRobotLifecycleStore((state) => state.loading);
  const pending = useRobotLifecycleStore((state) => state.commandPending);
  const error = useRobotLifecycleStore((state) => state.error);
  const enable = useRobotLifecycleStore((state) => state.enable);
  const disable = useRobotLifecycleStore((state) => state.disable);
  const recovery = useRobotLifecycleStore((state) => state.recovery);
  const reconcile = useRobotLifecycleStore((state) => state.reconcile);

  if (status?.configured === false) return null;

  const capabilities = status?.capabilities ?? [];
  const capability = (actionId: string) =>
    capabilities.find((value) => value.actionId === actionId);
  const clearFault = capability('robot.clear_fault');
  const clearWarning = capability('robot.clear_warning');
  const shutdown = capability('shutdown_controller');
  const prepareForMotion = capability('robot.prepare_for_motion');
  const makeNoncommandable = capability('robot.make_noncommandable');
  const anyControlStageActive =
    status?.robotEnabled === 'true'
    || status?.vendorControlReady === 'true'
    || status?.hardwareComponentActive === 'true'
    || status?.trajectoryControllerActive === 'true';
  const canEnable =
    status?.fresh === true
    && !pending
    && status.motionAdmitted !== true
    && prepareForMotion?.available === true;
  const canDisable =
    status?.fresh === true
    && !pending
    && anyControlStageActive
    && makeNoncommandable?.available === true;
  const tone =
    status?.readiness === 'ready'
      ? 'border-emerald-700/70'
      : status?.readiness === 'fault' || status?.readiness === 'offline'
        ? 'border-red-700/70'
        : 'border-amber-700/60';

  return (
    <div
      data-testid="robot-lifecycle-card"
      className={`shrink-0 border-b bg-gray-950/40 px-2 py-2 ${tone}`}
    >
      <div className="mb-1 flex items-center justify-between gap-2">
        <span className="text-[10px] font-semibold uppercase tracking-wide text-gray-300">
          Robot state
        </span>
        <span className="truncate font-mono text-[10px] text-gray-400">
          {status?.robotId ?? (loading ? 'loading…' : 'unavailable')}
        </span>
      </div>
      <div className="grid grid-cols-2 gap-x-3 gap-y-0.5">
        <Fact label="Connection" value={status?.transportConnected} />
        <Fact label="Position" value={status?.positionValid} />
        <Fact label="Drives" value={status?.robotEnabled} />
        <Fact label="Vendor control" value={status?.vendorControlReady} />
        <Fact label="Hardware" value={status?.hardwareComponentActive} />
        <Fact label="Trajectory" value={status?.trajectoryControllerActive} />
      </div>
      <div className="mt-1 rounded bg-gray-900/70 px-1.5 py-1 text-[10px] text-gray-300">
        {status?.commandInFlight
          ? `Working: ${status.commandInFlight.stage}`
          : status?.userMessage ?? 'Waiting for robot lifecycle status.'}
      </div>
      {(status?.faultPresent === 'true'
        || status?.warningPresent === 'true'
        || status?.reconciliationRequired === true) && (
        <div className="mt-1 rounded border border-red-800/70 bg-red-950/30 px-1.5 py-1 text-[10px] text-red-300">
          Error {status.adapterStatus?.error_code ?? '—'} · warning{' '}
          {status.adapterStatus?.warning_code ?? '—'}
          {status.reconciliationRequired ? ' · outcome must be reconciled' : ''}
        </div>
      )}
      {status?.reconciliationRequired === true && (
        <button
          type="button"
          disabled={pending || status.fresh !== true}
          onClick={() => {
            if (
              window.confirm(
                'Re-read the observed robot state and reconcile the uncertain '
                + 'command outcome against its required postcondition? This '
                + 'does not repeat the command.',
              )
            ) {
              void reconcile();
            }
          }}
          className="mt-1 w-full rounded border border-red-700 bg-red-950/50 px-2 py-1 text-[10px] text-red-200 hover:bg-red-900/60 disabled:opacity-40"
        >
          Reconcile observed outcome
        </button>
      )}
      {error !== null && (
        <div className="mt-1 rounded border border-red-800/70 bg-red-950/30 px-1.5 py-1 text-[10px] text-red-300">
          {error}
        </div>
      )}
      <div className="mt-1.5 flex gap-1">
        <button
          type="button"
          disabled={!canEnable}
          onClick={() => {
            if (
              window.confirm(
                'Prepare this robot for motion using its configured adapter? '
                + 'This may enable drives and external control, but it does '
                + 'not dispatch a trajectory or clear a fault.',
              )
            ) {
              void enable();
            }
          }}
          className="flex-1 rounded bg-emerald-700 px-2 py-1 text-[11px] text-white hover:bg-emerald-600 disabled:cursor-not-allowed disabled:opacity-40"
        >
          {status?.commandInFlight?.operation === 'enable'
            ? 'Enabling…'
            : 'Enable robot'}
        </button>
        <button
          type="button"
          disabled={!canDisable}
          onClick={() => {
            if (
              window.confirm(
                'Full-stop the behavior cycle, prove that no motion remains, '
                + 'then make this robot non-commandable using its configured '
                + 'adapter?',
              )
            ) {
              void disable();
            }
          }}
          className="flex-1 rounded bg-amber-700 px-2 py-1 text-[11px] text-white hover:bg-amber-600 disabled:cursor-not-allowed disabled:opacity-40"
        >
          {status?.commandInFlight?.operation === 'disable'
            ? 'Disabling…'
            : 'Disable robot'}
        </button>
      </div>
      {(clearFault?.available || clearWarning?.available) && (
        <div className="mt-1 flex gap-1">
          {clearFault?.available && (
            <button
              type="button"
              disabled={pending}
              onClick={() => {
                if (
                  window.confirm(
                    'Clear the current robot fault while drives remain disabled?',
                  )
                ) {
                  void recovery('clear-fault');
                }
              }}
              className="flex-1 rounded bg-red-800 px-2 py-1 text-[10px] text-white hover:bg-red-700 disabled:opacity-40"
            >
              Clear fault
            </button>
          )}
          {clearWarning?.available && (
            <button
              type="button"
              disabled={pending}
              onClick={() => {
                if (
                  window.confirm(
                    'Clear the current robot warning while drives remain disabled?',
                  )
                ) {
                  void recovery('clear-warning');
                }
              }}
              className="flex-1 rounded bg-amber-800 px-2 py-1 text-[10px] text-white hover:bg-amber-700 disabled:opacity-40"
            >
              Clear warning
            </button>
          )}
        </div>
      )}
      {shutdown?.available && (
        <details className="mt-1 text-[10px] text-gray-400">
          <summary className="cursor-pointer select-none">Advanced</summary>
          <button
            type="button"
            disabled={pending}
            onClick={() => {
              const phrase = window.prompt(
                'The provider reports every shutdown precondition satisfied. '
                + 'Type SHUTDOWN CONTROLLER to call the physical-controller '
                + 'shutdown service.',
              );
              if (phrase === 'SHUTDOWN CONTROLLER') {
                void recovery('shutdown-controller');
              }
            }}
            className="mt-1 w-full rounded border border-red-800 bg-red-950/50 px-2 py-1 text-red-300 hover:bg-red-900/60 disabled:opacity-40"
          >
            Shut down controller
          </button>
        </details>
      )}
      <div className="mt-1 text-[9px] leading-tight text-gray-500">
        The main Stop control stops motion only; it does not disable the robot.
      </div>
    </div>
  );
}
