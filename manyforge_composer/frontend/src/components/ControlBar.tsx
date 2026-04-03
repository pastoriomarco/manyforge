import { useCycleStore } from '../store/cycleStore';
import { useComposerStore } from '../store/composerStore';

export function ControlBar() {
  const { state, velocityScale, error, run, stop, reset, setVelocityScale } = useCycleStore();
  const { mode } = useComposerStore();

  const stateBadge = {
    idle: { label: 'IDLE', color: 'bg-gray-500' },
    running: { label: 'RUNNING', color: 'bg-green-500' },
    stopped: { label: 'STOPPED', color: 'bg-yellow-500' },
  }[state];

  const modeBadge = {
    compose: { label: 'COMPOSE', color: 'bg-blue-600' },
    run: state === 'running' ? { label: 'RUN', color: 'bg-green-600' } : { label: 'RUN', color: 'bg-yellow-600' },
  }[mode];

  return (
    <div className="h-16 bg-gray-800 border-t border-gray-700 flex items-center px-4 gap-4">
      {/* State badge */}
      <span className={`px-3 py-1 rounded text-sm font-mono ${stateBadge.color}`}>
        {stateBadge.label}
      </span>

      {/* Mode badge */}
      <span className={`px-3 py-1 rounded text-sm font-mono ${modeBadge.color}`}>
        {modeBadge.label}
      </span>

      {/* Buttons */}
      <button
        onClick={run}
        disabled={state === 'running'}
        className="px-4 py-1.5 bg-green-600 hover:bg-green-500 disabled:bg-gray-600 disabled:cursor-not-allowed rounded text-sm font-medium"
      >
        Run
      </button>
      <button
        onClick={stop}
        disabled={state !== 'running'}
        className="px-4 py-1.5 bg-red-600 hover:bg-red-500 disabled:bg-gray-600 disabled:cursor-not-allowed rounded text-sm font-medium"
      >
        Stop
      </button>
      <button
        onClick={reset}
        disabled={state === 'idle'}
        className="px-4 py-1.5 bg-blue-600 hover:bg-blue-500 disabled:bg-gray-600 disabled:cursor-not-allowed rounded text-sm font-medium"
      >
        Reset
      </button>

      {/* Velocity slider */}
      <div className="flex items-center gap-2 ml-4">
        <span className="text-sm text-gray-400">Velocity</span>
        <input
          type="range"
          min={0}
          max={100}
          value={Math.round(velocityScale * 100)}
          onChange={(e) => setVelocityScale(Number(e.target.value) / 100)}
          className="w-32"
        />
        <span className="text-sm font-mono w-10">{Math.round(velocityScale * 100)}%</span>
      </div>

      {/* Error display */}
      {error && (
        <span className="text-red-400 text-sm ml-auto">{error}</span>
      )}
    </div>
  );
}
