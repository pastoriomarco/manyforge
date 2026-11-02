import { EDGE_STYLE_OPTIONS, type EdgeStyle, type LayoutDirection } from '../store/composerStore';

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
        {/* Drawn icons: text glyphs sit on font metrics and are not centered. */}
        <svg className="canvas-tool-icon" viewBox="0 0 16 16" aria-hidden="true">
          {layoutDirection === 'vertical'
            ? <path d="M2 8h12M4.5 5.5 2 8l2.5 2.5M11.5 5.5 14 8l-2.5 2.5" />
            : <path d="M8 2v12M5.5 4.5 8 2l2.5 2.5M5.5 11.5 8 14l2.5-2.5" />}
        </svg>
        <span className="sr-only">Switch layout direction</span>
      </button>
      <button
        type="button"
        onClick={onAutoLayout}
        disabled={disabled}
        title="Auto-align nodes"
      >
        <svg className="canvas-tool-icon" viewBox="0 0 16 16" aria-hidden="true">
          <rect x="2.5" y="2.5" width="11" height="11" rx="1.2" />
          <path d="M6.17 2.5v11M9.83 2.5v11M2.5 6.17h11M2.5 9.83h11" />
        </svg>
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
            className="canvas-tool-icon"
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
