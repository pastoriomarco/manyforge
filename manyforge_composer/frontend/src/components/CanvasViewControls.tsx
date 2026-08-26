import type { EdgeStyle, LayoutDirection } from '../store/composerStore';

const EDGE_STYLE_OPTIONS: { value: EdgeStyle; label: string }[] = [
  { value: 'smoothstep', label: 'Orthogonal' },
  { value: 'simplebezier', label: 'Smooth' },
  { value: 'default', label: 'Bezier' },
  { value: 'straight', label: 'Straight' },
  { value: 'step', label: 'Step' },
];

interface CanvasViewControlsProps {
  edgeStyle: EdgeStyle;
  layoutDirection: LayoutDirection;
  disabled?: boolean;
  onEdgeStyleChange: (style: EdgeStyle) => void;
  onLayoutDirectionChange: (direction: LayoutDirection) => void;
  onAutoLayout: () => void;
  onFitView?: () => void;
}

/** Prop-driven graph presentation controls, independent of runtime stores. */
export function CanvasViewControls({
  edgeStyle,
  layoutDirection,
  disabled = false,
  onEdgeStyleChange,
  onLayoutDirectionChange,
  onAutoLayout,
  onFitView,
}: CanvasViewControlsProps) {
  const nextDirection = layoutDirection === 'vertical' ? 'horizontal' : 'vertical';
  return (
    <div className="canvas-view-controls" aria-label="Canvas view controls">
      <button
        type="button"
        onClick={() => onLayoutDirectionChange(nextDirection)}
        disabled={disabled}
        title={`Switch to ${nextDirection} layout`}
      >
        <span aria-hidden="true">{layoutDirection === 'vertical' ? '↔' : '↕'}</span>
        <span className="sr-only">Switch layout direction</span>
      </button>
      <button
        type="button"
        onClick={onAutoLayout}
        disabled={disabled}
        title="Auto-align nodes"
      >
        <span aria-hidden="true">▦</span>
        <span className="sr-only">Auto-align nodes</span>
      </button>
      {onFitView && (
        <button
          type="button"
          onClick={onFitView}
          title="Fit complete tree in canvas"
          aria-label="Fit complete tree in canvas"
        >
          <svg
            className="canvas-fit-icon"
            viewBox="0 0 16 16"
            aria-hidden="true"
          >
            <path d="M6 3H3v3M10 3h3v3M3 10v3h3M13 10v3h-3" />
          </svg>
        </button>
      )}
      <select
        value={edgeStyle}
        onChange={(event) => onEdgeStyleChange(event.target.value as EdgeStyle)}
        disabled={disabled}
        title="Connection line style"
        aria-label="Connection line style"
      >
        {EDGE_STYLE_OPTIONS.map((option) => (
          <option key={option.value} value={option.value}>{option.label}</option>
        ))}
      </select>
    </div>
  );
}
