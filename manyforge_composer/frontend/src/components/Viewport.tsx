import { Component, useCallback, useState } from 'react';
import type { ErrorInfo, PointerEvent as ReactPointerEvent, ReactNode } from 'react';
import { TreeCanvas } from './TreeCanvas';
import { PropertyPanel } from './PropertyPanel';
import { SceneViewerPanel } from './SceneViewer';
import { CollapsedRail } from './CollapsedRail';
import { useProgramStore } from '../store/programStore';
import { useDeploymentStore } from '../store/deploymentStore';

interface ViewerErrorBoundaryProps {
  fallback: ReactNode;
  children: ReactNode;
}

interface ViewerErrorBoundaryState {
  hasError: boolean;
}

// Isolated error boundary around whichever viewer is mounted. The native
// R3F viewer is a panel — if it crashes (WebGL context loss, a drei
// version mismatch, a malformed URDF, etc.) Composer's editor must
// remain usable. Without this, an exception inside the viewer
// propagates up and React 19 unmounts the whole tree (operator sees a
// blank page).
class ViewerErrorBoundary extends Component<
  ViewerErrorBoundaryProps,
  ViewerErrorBoundaryState
> {
  state: ViewerErrorBoundaryState = { hasError: false };

  static getDerivedStateFromError(): ViewerErrorBoundaryState {
    return { hasError: true };
  }

  componentDidCatch(error: Error, info: ErrorInfo): void {
    console.error('Viewer panel crashed:', error, info.componentStack);
  }

  render(): ReactNode {
    if (this.state.hasError) {
      return this.props.fallback;
    }
    return this.props.children;
  }
}

function ViewerCrashFallback() {
  return (
    <div className="flex h-full w-full flex-col overflow-hidden bg-gray-900">
      <div className="flex h-10 items-center border-b border-gray-700 bg-gray-800 px-3">
        <div className="text-xs font-semibold uppercase tracking-wide text-orange-300">
          Viewer error
        </div>
      </div>
      <div className="flex flex-1 items-center justify-center p-6 text-center">
        <div className="max-w-sm rounded-lg border border-orange-700 bg-gray-900/95 p-4 shadow-xl">
          <div className="mb-2 text-sm font-semibold text-orange-300">
            Native viewer crashed
          </div>
          <p className="text-sm text-gray-300">
            The 3D viewer panel hit a runtime error and was isolated.
            Composer is still usable; the underlying program and runtime are unaffected.
            Try reloading the page to recreate the WebGL context.
          </p>
          <p className="mt-2 text-xs text-gray-500">
            See browser devtools console for the full stack trace.
          </p>
        </div>
      </div>
    </div>
  );
}

// The in-React-tree R3F viewer owns the active viewport.
const MIN_VIEWER_WIDTH = 480;
const MAX_VIEWER_WIDTH = 960;
const MIN_PROPERTIES_WIDTH = 260;
const MAX_PROPERTIES_WIDTH = 520;

function signature(value: unknown): string {
  return JSON.stringify(value ?? null);
}

function clamp(value: number, min: number, max: number): number {
  return Math.min(Math.max(value, min), max);
}

export function Viewport() {
  const [viewerWidth, setViewerWidth] = useState(720);
  const [propertyWidth, setPropertyWidth] = useState(320);
  const [viewerCollapsed, setViewerCollapsed] = useState(false);
  const [propertyCollapsed, setPropertyCollapsed] = useState(false);
  const [activeResize, setActiveResize] = useState<'properties' | 'viewer' | null>(null);

  const flowToTree = useProgramStore((state) => state.flowToTree);
  const appliedTree = useProgramStore((state) => state.appliedTree);
  const deployment = useDeploymentStore((state) => state.deployment);
  const appliedDeployment = useDeploymentStore((state) => state.appliedDeployment);
  const draftTree = flowToTree();
  const hasUnappliedChanges =
    signature(draftTree) !== signature(appliedTree) ||
    signature(deployment) !== signature(appliedDeployment);

  const beginResize = useCallback(
    (
      event: ReactPointerEvent<HTMLDivElement>,
      target: 'properties' | 'viewer',
      startWidth: number,
      onMoveWidth: (nextWidth: number, delta: number) => void,
    ) => {
      event.preventDefault();
      const handle = event.currentTarget;
      const startX = event.clientX;
      setActiveResize(target);
      handle.setPointerCapture(event.pointerId);

      const handleMove = (moveEvent: PointerEvent) => {
        const delta = moveEvent.clientX - startX;
        onMoveWidth(startWidth, delta);
      };
      const handleUp = () => {
        setActiveResize((current) => (current === target ? null : current));
        handle.removeEventListener('pointermove', handleMove);
        handle.removeEventListener('pointerup', handleUp);
        handle.removeEventListener('pointercancel', handleUp);
        if (handle.hasPointerCapture(event.pointerId)) {
          handle.releasePointerCapture(event.pointerId);
        }
      };

      handle.addEventListener('pointermove', handleMove);
      handle.addEventListener('pointerup', handleUp);
      handle.addEventListener('pointercancel', handleUp);
    },
    [],
  );

  return (
    <div className="flex flex-1 overflow-hidden">
      <div className="flex min-h-0 min-w-0 flex-1 overflow-hidden">
        <TreeCanvas />
      </div>

      {propertyCollapsed ? (
        <CollapsedRail
          label="Properties"
          side="right"
          onExpand={() => setPropertyCollapsed(false)}
        />
      ) : (
        <>
          <div
            onPointerDown={(event) =>
              beginResize(event, 'properties', propertyWidth, (startWidth, delta) =>
                setPropertyWidth(clamp(startWidth - delta, MIN_PROPERTIES_WIDTH, MAX_PROPERTIES_WIDTH)),
              )
            }
            className="w-3 shrink-0 touch-none cursor-col-resize bg-gray-800/40 transition-colors hover:bg-blue-500/60"
            title="Resize properties panel"
          />
          <div
            className="relative shrink-0 overflow-hidden border-l border-gray-700"
            style={{ width: propertyWidth }}
          >
            <PropertyPanel onCollapse={() => setPropertyCollapsed(true)} />
          </div>
        </>
      )}

      {viewerCollapsed ? (
        <CollapsedRail
          label="Viewer"
          side="right"
          onExpand={() => setViewerCollapsed(false)}
        />
      ) : (
        <>
          <div
            onPointerDown={(event) =>
              beginResize(event, 'viewer', viewerWidth, (startWidth, delta) =>
                setViewerWidth(clamp(startWidth - delta, MIN_VIEWER_WIDTH, MAX_VIEWER_WIDTH)),
              )
            }
            className="w-3 shrink-0 touch-none cursor-col-resize bg-gray-800/40 transition-colors hover:bg-blue-500/60"
            title="Resize viewer panel"
          />
          <div
            className="relative shrink-0 overflow-hidden border-l border-gray-700"
            style={{ width: viewerWidth }}
          >
            <ViewerErrorBoundary fallback={<ViewerCrashFallback />}>
              <SceneViewerPanel
                onCollapse={() => setViewerCollapsed(true)}
                blocked={hasUnappliedChanges}
                resizing={activeResize === 'viewer'}
              />
            </ViewerErrorBoundary>
          </div>
        </>
      )}
    </div>
  );
}
