import { useCallback, useMemo, useState } from 'react';
import type { PointerEvent as ReactPointerEvent } from 'react';
import { TreeCanvas } from './TreeCanvas';
import { PropertyPanel } from './PropertyPanel';
import { ViserPanel } from './ViserPanel';
import { CollapsedRail } from './CollapsedRail';
import { useProgramStore } from '../store/programStore';
import { useDeploymentStore } from '../store/deploymentStore';

const MIN_VISER_WIDTH = 480;
const MAX_VISER_WIDTH = 960;
const MIN_PROPERTIES_WIDTH = 260;
const MAX_PROPERTIES_WIDTH = 520;

function signature(value: unknown): string {
  return JSON.stringify(value ?? null);
}

function clamp(value: number, min: number, max: number): number {
  return Math.min(Math.max(value, min), max);
}

export function Viewport() {
  const [viserWidth, setViserWidth] = useState(720);
  const [propertyWidth, setPropertyWidth] = useState(320);
  const [viserCollapsed, setViserCollapsed] = useState(false);
  const [propertyCollapsed, setPropertyCollapsed] = useState(false);
  const [activeResize, setActiveResize] = useState<'properties' | 'viser' | null>(null);

  const programNodes = useProgramStore((state) => state.nodes);
  const programEdges = useProgramStore((state) => state.edges);
  const flowToTree = useProgramStore((state) => state.flowToTree);
  const appliedTree = useProgramStore((state) => state.appliedTree);
  const deployment = useDeploymentStore((state) => state.deployment);
  const appliedDeployment = useDeploymentStore((state) => state.appliedDeployment);
  const draftTree = useMemo(() => flowToTree(), [flowToTree, programEdges, programNodes]);
  const hasUnappliedChanges =
    signature(draftTree) !== signature(appliedTree) ||
    signature(deployment) !== signature(appliedDeployment);

  const beginResize = useCallback(
    (
      event: ReactPointerEvent<HTMLDivElement>,
      target: 'properties' | 'viser',
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

      {viserCollapsed ? (
        <CollapsedRail label="Viser" side="right" onExpand={() => setViserCollapsed(false)} />
      ) : (
        <>
          <div
            onPointerDown={(event) =>
              beginResize(event, 'viser', viserWidth, (startWidth, delta) =>
                setViserWidth(clamp(startWidth - delta, MIN_VISER_WIDTH, MAX_VISER_WIDTH)),
              )
            }
            className="w-3 shrink-0 touch-none cursor-col-resize bg-gray-800/40 transition-colors hover:bg-blue-500/60"
            title="Resize Viser panel"
          />
          <div
            className="relative shrink-0 overflow-hidden border-l border-gray-700"
            style={{ width: viserWidth }}
          >
            <ViserPanel
              onCollapse={() => setViserCollapsed(true)}
              blocked={hasUnappliedChanges}
              resizing={activeResize === 'viser'}
            />
          </div>
        </>
      )}
    </div>
  );
}
