import { useComposerStore } from '../store/composerStore';
import { useProgramStore } from '../store/programStore';
import { useEditLocked } from '../store/cycleStore';
import type { EdgeStyle } from '../store/composerStore';

const EDGE_STYLE_OPTIONS: { value: EdgeStyle; label: string }[] = [
  { value: 'smoothstep', label: 'Orthogonal' },
  { value: 'simplebezier', label: 'Smooth' },
  { value: 'default', label: 'Bezier' },
  { value: 'straight', label: 'Straight' },
  { value: 'step', label: 'Step' },
];

/**
 * CanvasViewControls — graph-view options (orientation, auto-align, edge
 * style) as a compact pill overlaying the tree canvas, next to the thing
 * they affect. These are view preferences, not document actions, so they
 * are deliberately quiet: icon buttons, neutral chrome, tooltips.
 */
export function CanvasViewControls() {
  const { edgeStyle, setEdgeStyle } = useComposerStore();
  const nodes = useProgramStore((state) => state.nodes);
  const layoutDirection = useProgramStore((state) => state.layoutDirection);
  const setLayoutDirection = useProgramStore((state) => state.setLayoutDirection);
  const autoLayout = useProgramStore((state) => state.autoLayout);
  const editLocked = useEditLocked();
  const disabled = editLocked || nodes.length === 0;

  return (
    <div className="pointer-events-none absolute inset-x-0 bottom-3 z-20 flex justify-center">
      <div className="pointer-events-auto flex items-center gap-1 rounded-md border border-gray-600/70 bg-gray-900/85 px-1.5 py-1 shadow-lg shadow-black/40 backdrop-blur-sm">
        <button
          type="button"
          onClick={() => setLayoutDirection(layoutDirection === 'vertical' ? 'horizontal' : 'vertical')}
          disabled={disabled}
          title={layoutDirection === 'vertical' ? 'Switch to horizontal layout' : 'Switch to vertical layout'}
          className="rounded p-1.5 text-gray-300 transition-colors hover:bg-gray-700 hover:text-white disabled:cursor-not-allowed disabled:text-gray-600"
        >
          {layoutDirection === 'vertical' ? (
            <svg viewBox="0 0 16 16" className="h-4 w-4 fill-none stroke-current" strokeWidth="1.5" aria-hidden>
              <path d="M2 8h9M8 4l4 4-4 4" strokeLinecap="round" strokeLinejoin="round" />
            </svg>
          ) : (
            <svg viewBox="0 0 16 16" className="h-4 w-4 fill-none stroke-current" strokeWidth="1.5" aria-hidden>
              <path d="M8 2v9M4 8l4 4 4-4" strokeLinecap="round" strokeLinejoin="round" />
            </svg>
          )}
        </button>
        <button
          type="button"
          onClick={autoLayout}
          disabled={disabled}
          title="Auto-align nodes"
          className="rounded p-1.5 text-gray-300 transition-colors hover:bg-gray-700 hover:text-white disabled:cursor-not-allowed disabled:text-gray-600"
        >
          <svg viewBox="0 0 16 16" className="h-4 w-4 fill-none stroke-current" strokeWidth="1.5" aria-hidden>
            <rect x="2" y="2" width="4.5" height="4.5" rx="0.5" />
            <rect x="9.5" y="2" width="4.5" height="4.5" rx="0.5" />
            <rect x="2" y="9.5" width="4.5" height="4.5" rx="0.5" />
            <rect x="9.5" y="9.5" width="4.5" height="4.5" rx="0.5" />
          </svg>
        </button>
        <div className="mx-0.5 h-5 w-px bg-gray-600/70" />
        <select
          value={edgeStyle}
          onChange={(e) => setEdgeStyle(e.target.value as EdgeStyle)}
          disabled={editLocked}
          className="rounded border-none bg-transparent py-1 pl-1 pr-5 text-xs text-gray-300 hover:text-white focus:outline-none disabled:cursor-not-allowed disabled:text-gray-600"
          title="Connection line style"
        >
          {EDGE_STYLE_OPTIONS.map((option) => (
            <option key={option.value} value={option.value} className="bg-gray-800 text-gray-200">
              {option.label}
            </option>
          ))}
        </select>
      </div>
    </div>
  );
}
