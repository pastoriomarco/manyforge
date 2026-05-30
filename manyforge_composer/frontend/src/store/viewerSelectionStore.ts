import { create } from 'zustand';

interface ViewerCamera {
  position: [number, number, number];
  target: [number, number, number];
}

type RobotMode = 'joint' | 'cartesian' | 'move-to' | 'pose-edit';
type MoveToSource = 'object' | 'frame' | 'pose';
type MoveToOrientationMode = 'keep-current' | 'face-down' | 'use-entity';

interface MoveToState {
  source: MoveToSource;
  objectId: string | null;
  frameName: string | null;
  poseNodeName: string | null;
  orientationMode: MoveToOrientationMode;
  offsetXyzM: [number, number, number];
  offsetRpyDeg: [number, number, number];
}

const MOVE_TO_DEFAULT_STATE: MoveToState = {
  source: 'object',
  objectId: null,
  frameName: null,
  poseNodeName: null,
  orientationMode: 'keep-current',
  offsetXyzM: [0, 0, 0],
  offsetRpyDeg: [0, 0, 0],
};
type CartesianFrame = 'base' | 'tcp';
type ActivePanel = 'tools' | 'robot' | 'object' | null;

interface ViewerSelectionStore {
  selectedId: string | null;
  lockedIds: Set<string>;
  pendingCamera: ViewerCamera | null;
  // Which overlay panel is open above the 3D canvas. Lifted out of
  // SceneViewer local state so Canvas descendants (TcpJogGizmo,
  // SceneObjectMesh's TransformControls) can gate themselves on
  // panel focus without prop-drilling through the Canvas boundary.
  // Switching the active panel implicitly stops the relevant
  // gizmos — the gizmo components unmount and reset their drag state.
  activePanel: ActivePanel;
  // Day-5c: robot-panel mode. The Cartesian mode renders a TCP
  // gizmo inside the 3D Canvas (managed by SceneViewer reading from
  // this store). Joint mode hides the gizmo. Pose-Edit mode is
  // non-destructive — sliders edit stored values, no commanding.
  robotMode: RobotMode;
  // Move-To tab state. Lives in the store so the 3D ghost preview
  // (rendered inside the Canvas) can read the same selection the
  // panel UI is editing. Reset on panel close so each session starts
  // from a clean default.
  moveTo: MoveToState;
  cartesianFrame: CartesianFrame;
  cartesianGizmoMode: 'translate' | 'rotate';
  // True while the TCP jog gizmo is actively being dragged. Flips
  // the global edit-locked state ON so the rest of the UI behaves
  // the way it does during direct_control — even if the backend
  // hasn't entered the direct_control cycle state yet (the first
  // jog request only races there on mouseUp).
  isDraggingGizmo: boolean;
  // Optional diagnostics overlay for external pose-source observations
  // (Isaac/perception measured state). Kept viewer-local and off by
  // default because it is an operator aid, not planning-scene truth.
  measuredStateGhostVisible: boolean;
  setSelected: (id: string | null) => void;
  toggleLocked: (id: string) => void;
  isLocked: (id: string) => boolean;
  setPendingCamera: (cam: ViewerCamera | null) => void;
  consumePendingCamera: () => ViewerCamera | null;
  setActivePanel: (panel: ActivePanel) => void;
  toggleActivePanel: (panel: NonNullable<ActivePanel>) => void;
  setRobotMode: (mode: RobotMode) => void;
  patchMoveTo: (patch: Partial<MoveToState>) => void;
  resetMoveTo: () => void;
  setCartesianFrame: (frame: CartesianFrame) => void;
  setCartesianGizmoMode: (mode: 'translate' | 'rotate') => void;
  setDraggingGizmo: (next: boolean) => void;
  setMeasuredStateGhostVisible: (visible: boolean) => void;
}

export type {
  RobotMode,
  CartesianFrame,
  ActivePanel,
  MoveToSource,
  MoveToOrientationMode,
  MoveToState,
};

/**
 * Shared viewer-side selection state.
 *
 * Day 4 makes the 3D viewer's selection visible outside the canvas
 * (right-click context menu, click-to-emit-to-side-panels, the
 * Day-5 object-edit overlay). Lifting `selectedId` out of
 * <SceneCanvas> local state into this store keeps the existing
 * intra-canvas behaviour (TransformControls anchored on the
 * selected mesh) while letting other React components subscribe.
 *
 * `lockedIds` is a frontend-only flag from the right-click context
 * menu's "Lock" entry — a locked object suppresses its
 * <TransformControls> regardless of selection, so it can't be
 * accidentally nudged while authoring nearby objects.
 *
 * `pendingCamera` is set by URL-pin parsing on mount and by future
 * "frame to selection" / programmatic-set actions. `SceneCanvas`
 * consumes it once via `consumePendingCamera()` (single-shot) to
 * apply to the camera + OrbitControls target.
 */
export const useViewerSelectionStore = create<ViewerSelectionStore>((set, get) => ({
  selectedId: null,
  lockedIds: new Set<string>(),
  pendingCamera: null,
  activePanel: null,
  robotMode: 'joint',
  moveTo: MOVE_TO_DEFAULT_STATE,
  cartesianFrame: 'base',
  cartesianGizmoMode: 'translate',
  isDraggingGizmo: false,
  measuredStateGhostVisible: false,
  setSelected: (id) => set({ selectedId: id }),
  toggleLocked: (id) =>
    set((state) => {
      const next = new Set(state.lockedIds);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return { lockedIds: next };
    }),
  isLocked: (id) => get().lockedIds.has(id),
  setPendingCamera: (cam) => set({ pendingCamera: cam }),
  consumePendingCamera: () => {
    const current = get().pendingCamera;
    if (current !== null) set({ pendingCamera: null });
    return current;
  },
  setActivePanel: (panel) => set({ activePanel: panel }),
  toggleActivePanel: (panel) =>
    set((state) => ({ activePanel: state.activePanel === panel ? null : panel })),
  setRobotMode: (mode) => set({ robotMode: mode }),
  patchMoveTo: (patch) => set((state) => ({ moveTo: { ...state.moveTo, ...patch } })),
  resetMoveTo: () => set({ moveTo: MOVE_TO_DEFAULT_STATE }),
  setCartesianFrame: (frame) => set({ cartesianFrame: frame }),
  setCartesianGizmoMode: (mode) => set({ cartesianGizmoMode: mode }),
  setDraggingGizmo: (next) => set({ isDraggingGizmo: next }),
  setMeasuredStateGhostVisible: (visible) => set({ measuredStateGhostVisible: visible }),
}));

/**
 * Parse `?cam=x,y,z&target=x,y,z` from window.location.search and seed
 * the viewer selection store's pendingCamera. Called once at app
 * startup (see `main.tsx`). Idempotent; missing or malformed params
 * leave pendingCamera at null and the viewer uses its built-in
 * defaults.
 */
export function seedCameraFromUrl(): void {
  if (typeof window === 'undefined') return;
  const params = new URLSearchParams(window.location.search);
  const camRaw = params.get('cam');
  const targetRaw = params.get('target');
  if (camRaw === null && targetRaw === null) return;

  function parseTriple(raw: string | null): [number, number, number] | null {
    if (raw === null) return null;
    const parts = raw.split(',').map((s) => Number(s.trim()));
    if (parts.length !== 3 || parts.some((n) => !Number.isFinite(n))) return null;
    return [parts[0], parts[1], parts[2]];
  }

  const position = parseTriple(camRaw) ?? [2, 2, 1.5];
  const target = parseTriple(targetRaw) ?? [0, 0, 0.3];
  useViewerSelectionStore.getState().setPendingCamera({ position, target });
}
