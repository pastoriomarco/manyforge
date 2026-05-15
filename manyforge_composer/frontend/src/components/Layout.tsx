import { useCallback, useState } from 'react';
import type { PointerEvent as ReactPointerEvent } from 'react';
import { Sidebar } from './Sidebar';
import { Viewport } from './Viewport';
import { ControlBar } from './ControlBar';
import { CollapsedRail } from './CollapsedRail';
import { AssistantOverlay } from './AssistantOverlay';
import { RuntimeStatusOverlay } from './RuntimeStatusOverlay';
import { useDeploymentStore } from '../store/deploymentStore';

const MIN_SIDEBAR_WIDTH = 220;
const MAX_SIDEBAR_WIDTH = 420;

function clamp(value: number, min: number, max: number): number {
  return Math.min(Math.max(value, min), max);
}

export function Layout() {
  const [sidebarWidth, setSidebarWidth] = useState(340);
  const [sidebarCollapsed, setSidebarCollapsed] = useState(false);
  // Gate the AssistantOverlay (AI button + panel) on the loaded deployment's
  // `assistant.enabled` flag (per spec 430 §5). When no assistant block is
  // present, the overlay is hidden — matches the MUST that Composer not
  // route provider-mode assistant calls through an unconfigured assistant.
  const assistantEnabled = useDeploymentStore(
    (state) => state.deployment?.assistant?.enabled === true,
  );

  const beginResize = useCallback(
    (
      event: ReactPointerEvent<HTMLDivElement>,
      startWidth: number,
      onMoveWidth: (nextWidth: number, delta: number) => void,
    ) => {
      event.preventDefault();
      const handle = event.currentTarget;
      const startX = event.clientX;
      handle.setPointerCapture(event.pointerId);

      const handleMove = (moveEvent: PointerEvent) => {
        const delta = moveEvent.clientX - startX;
        onMoveWidth(startWidth, delta);
      };
      const handleUp = () => {
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
    <div className="flex h-screen flex-col bg-gray-900 text-white">
      <div className="flex flex-1 overflow-hidden">
        {sidebarCollapsed ? (
          <CollapsedRail label="Sidebar" side="left" onExpand={() => setSidebarCollapsed(false)} />
        ) : (
          <>
            <div
              className="relative shrink-0 overflow-hidden border-r border-gray-700"
              style={{ width: sidebarWidth }}
            >
              <Sidebar onCollapse={() => setSidebarCollapsed(true)} />
            </div>
            <div
              onPointerDown={(event) =>
                beginResize(event, sidebarWidth, (startWidth, delta) =>
                  setSidebarWidth(clamp(startWidth + delta, MIN_SIDEBAR_WIDTH, MAX_SIDEBAR_WIDTH)),
                )
              }
              className="w-3 shrink-0 touch-none cursor-col-resize bg-gray-800/40 transition-colors hover:bg-blue-500/60"
              title="Resize sidebar"
            />
          </>
        )}

        <Viewport />
      </div>
      <ControlBar />
      <RuntimeStatusOverlay />
      {assistantEnabled ? <AssistantOverlay /> : null}
    </div>
  );
}
