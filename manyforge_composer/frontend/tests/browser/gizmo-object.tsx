// Actual viewer and scene release path, with a disposable local owner only.
import { StrictMode, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { StructuralViewer } from '../../src/components/StructuralViewer';
import type { LogicalSceneObject } from '../../src/types/api';
import '../../src/index.css';

const commits: unknown[] = [];
Object.assign(window, { objectGizmoHarness: { commits } });
function App() {
  const [inspector, setInspector] = useState<HTMLDivElement | null>(null);
  const [object, setObject] = useState<LogicalSceneObject>({
    scene_resource_id: 'test-box', collision_shapes: [{ type: 'box', dimensions_m: { x: 0.2, y: 0.2, z: 0.2 } }],
    placement: { kind: 'cell_fixed', cell_id: 'fixture', pose_in_cell: {
      position_m: { x: 0.123, y: 0.237, z: 0.344 }, orientation_xyzw: [0, 0, 0, 1],
    } }, provider_evidence: null, support_contacts: [],
  });
  return <div style={{ display: 'grid', gridTemplateColumns: '300px 1fr', height: '100vh' }}>
    <div ref={setInspector} />
    <StructuralViewer viewerActive manifest={{ status: 'loading' }} onRetry={() => undefined}
      inspectorTarget={inspector} sceneObjects={[object]} selectedSceneResourceId={object.scene_resource_id}
      sceneOwnerIdentity={{ ownerEpoch: 'disposable-fixture', generation: 1 }}
      sceneMutationAvailability={{ 'test-box': {
        transform: true, shape: true, duplicate: false, remove: false, detach: false,
        transformReason: null, shapeReason: null, duplicateReason: null, removeReason: null, detachReason: null,
      } }}
      onCommitSceneTransform={async (id, pose, scale, identity) => {
        commits.push({ id, pose, scale, identity });
        setObject({ ...object, placement: { kind: 'cell_fixed', cell_id: 'fixture', pose_in_cell: pose } });
        return true;
      }} />
  </div>;
}
createRoot(document.getElementById('root')!).render(<StrictMode><App /></StrictMode>);
