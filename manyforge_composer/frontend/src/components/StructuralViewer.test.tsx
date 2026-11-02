import { readFileSync } from 'node:fs';

import type { ReactElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import * as THREE from 'three';
import { describe, expect, it, vi } from 'vitest';

import {
  DEFAULT_MEASURED_STATE_GHOSTS_VISIBLE,
  DEFAULT_WIREFRAME_VISIBLE,
  VIEWER_CLICK_MAX_DURATION_MS,
  VIEWER_CONTEXT_DRAG_THRESHOLD_PX,
  clonePreviewMesh,
  candidateGhostColor,
  MeasuredStateGhostLayers,
  MeasuredStateGhostToggle,
  frameableContentBounds,
  framedCameraPresentation,
  sceneObjectMaterial,
  supportsIndependentSceneScale,
  viewerClickWithinDuration,
  viewerContextWasDragged,
  AttachedLogicalSceneObject,
  ViewerFailureBoundary,
} from './StructuralViewer';
import type { LogicalSceneObject, SceneRuntimeObservationPose, SceneUrdfManifest } from '../types/api';
import type {
  AttachedObjectPresentation,
  MeasuredPosePresentation,
  MeasuredRobotPresentation,
  RuntimeScenePresentation,
} from '../state/runtimeScenePresentation';
import { readStyleSource } from '../styles/readStyleSource';

function sceneObject(
  shape: LogicalSceneObject['collision_shapes'][number],
): LogicalSceneObject {
  return {
    scene_resource_id: 'part',
    collision_shapes: [shape],
    placement: {
      kind: 'cell_fixed',
      cell_id: 'cell',
      pose_in_cell: {
        position_m: { x: 0, y: 0, z: 0 },
        orientation_xyzw: [0, 0, 0, 1],
      },
    },
    provider_evidence: null,
    support_contacts: [],
  };
}

describe('viewer failure isolation', () => {
  it('uses independent unlit translucent preview materials and reuses mesh geometry', () => {
    expect(candidateGhostColor('receipt', false)).toBe('#58ddff');
    expect(candidateGhostColor('receipt', true)).toBe('#f2bb56');
    expect(candidateGhostColor(null, true)).toBe('#f2bb56');
    expect(candidateGhostColor(null, false, true)).toBe('#ff7066');
    const original = new THREE.Mesh(new THREE.BoxGeometry(), new THREE.MeshStandardMaterial());
    const preview = clonePreviewMesh(original, '#58ddff', 0.5) as THREE.Mesh<THREE.BoxGeometry, THREE.MeshBasicMaterial>;
    expect(preview.material).toBeInstanceOf(THREE.MeshBasicMaterial);
    expect(preview.material.color.getHexString()).toBe('58ddff');
    expect(preview.material).toMatchObject({ transparent: true, opacity: 0.5,
      toneMapped: false, depthWrite: false, side: THREE.FrontSide });
    expect(preview.geometry).toBe(original.geometry);
    expect(original.material).toMatchObject({ transparent: false, opacity: 1 });
    expect(preview.material).not.toBe(original.material);
    preview.material.dispose();
    original.material.dispose();
    original.geometry.dispose();
  });
  it('offers unconstrained scale only for shapes the gizmo can represent safely', () => {
    expect(supportsIndependentSceneScale(sceneObject({
      type: 'box', dimensions_m: { x: 1, y: 1, z: 1 },
    }))).toBe(true);
    expect(supportsIndependentSceneScale(sceneObject({
      type: 'mesh', resource_uri: 'package://part', scale: { x: 1, y: 1, z: 1 },
    }))).toBe(true);
    expect(supportsIndependentSceneScale(sceneObject({
      type: 'sphere', radius_m: 1,
    }))).toBe(false);
    expect(supportsIndependentSceneScale(sceneObject({
      type: 'cylinder', radius_m: 1, height_m: 1,
    }))).toBe(false);
  });

  it('replaces only the failed viewer subtree with its fallback', () => {
    const boundary = new ViewerFailureBoundary({
      children: <div>viewport</div>,
      fallback: <div>isolated; authoring remains usable</div>,
    });
    boundary.state = ViewerFailureBoundary.getDerivedStateFromError();

    expect(renderToStaticMarkup(boundary.render())).toContain(
      'isolated; authoring remains usable',
    );
  });

  it('defaults scene presentation to solid geometry and keeps wireframe optional', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(DEFAULT_WIREFRAME_VISIBLE).toBe(false);
    expect(DEFAULT_MEASURED_STATE_GHOSTS_VISIBLE).toBe(false);
    expect(source).toContain('<span>Wireframe overlays</span>');
    expect(source).toContain('wireframe={wireframe}');
    expect(source).toContain('depthWrite={!material.transparent && !wireframe}');
    expect(source).toContain(
      'setViewerQualificationWireframeDefault(DEFAULT_WIREFRAME_VISIBLE)',
    );
  });

  it('keeps authoritative, Draft, comparison, and candidate sources visually distinct', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );
    const authoritative = sceneObjectMaterial('authoritative', false);
    const draft = sceneObjectMaterial('draft', false);
    const comparison = sceneObjectMaterial('comparison', false);

    expect(authoritative).toMatchObject({ opacity: 1, transparent: false });
    expect(draft).toMatchObject({ color: '#e4ac55', opacity: 0.62, transparent: true });
    expect(comparison).toMatchObject({ color: '#43d7ec', opacity: 0.45, transparent: true });
    expect(new Set([authoritative.color, draft.color, comparison.color]).size).toBe(3);
    expect(source).toContain("appearance={programDraftPreview ? 'draft' : 'authoritative'}");
    expect(source).toContain("data-scene-presentation={programDraftPreview ? 'program-draft' : 'owner'}");
    expect(source).toContain('objects={sceneObjects}');
    expect(source).not.toContain('<ObservedLogicalSceneObjects');
    expect(source).toContain('viewer-source-swatch candidate');
    // The preview-layer dot moved from the Tools button to the viewer gear.
    const transport = readFileSync(new URL('./ProgramRuntimePanel.tsx', import.meta.url), 'utf8');
    expect(transport).toContain('<span className="viewer-preview-dot" aria-hidden="true" />');
    expect(transport).toContain('A Draft scene or target preview is visible. Open the viewer settings for layer details.');
    expect(source).toContain('previewLayerActive: auxiliaryViewerSourcesActive,');
    expect(source).not.toContain('viewer-source-active');
    expect(source).toContain('Measured state ghosts');
    expect(source).not.toContain('Non-live viewer overlays');
  });

  it('keeps viewer layers in one bounded inspector panel at narrow widths', () => {
    const css = readStyleSource();
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(css).toContain('.viewer-options-body');
    expect(source.match(/type="checkbox"/g)).toHaveLength(7);
    // Optional silhouette, off by default; it outlines the framed content.
    expect(source).toContain('<span>Object outline</span>');
    // Display toggles are installation settings, not local component state.
    expect(source).toContain('objectOutline: outlineVisible,');
    expect(source).toContain("setViewerSetting('objectOutline', event.target.checked)");
    expect(source).toContain('<SceneOutline multisample={graphicsProfile.outlineSmoothing} componentByLink={outlineComponents} />');
    // Viewer settings open from the viewer gear; there is no Tools button.
    expect(source).not.toContain("toggleInspectorPanel('tools')");
    expect(source).toContain("setInspectorPanel('tools');");
    // One scope vocabulary: a tag on the heading or row it applies to.
    expect(source).toContain('title="Display"');
    expect(source).toContain("onRestoreDefaults={() => composerSettings.restoreSection('viewer')}");
    // Tags only on headings; each heading has one scope.
    expect(source).toContain('<SettingsSubheading title="Size" scope="program" />');
    expect(source).toContain('<SettingsSubheading title="Shown" scope="session" />');
    expect(source).toContain("['Program objects', 'program', presentedObjectAxesRows.filter((row) => row.persistent)],");
    expect(source).toContain("['Other objects', 'session', presentedObjectAxesRows.filter((row) => !row.persistent)],");
    expect(source).not.toContain('Program Draft setting');
    expect(source).toContain('<summary>Control panels</summary>');
    expect(source).toContain('MeasuredStateGhostToggle');
    expect(source).toContain('checked={enabled}');
    expect(source).toContain('measuredStateGhostsEnabled');
    expect(source).toContain('Model and source facts');
    expect(source).toContain('{manifest.data.links.length} links · {manifest.data.joints.length} joints');
    expect(source).not.toContain('className="viewer-legend"');
  });

  it('keeps camera and transform commands in the compact viewer header', () => {
    const css = readStyleSource();
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );
    const header = source.indexOf('className="panel-header viewer-header"');
    const stage = source.indexOf('className="viewer-stage"', header);
    const controls = source.indexOf('aria-label="Scene viewport controls"', header);
    const launchers = source.indexOf('className="viewer-surface-launchers"', header);
    const frameAll = source.indexOf('aria-label="Frame all visible content"', controls);
    const transform = source.indexOf('title="Translate target (W)"', controls);

    expect(header).toBeGreaterThan(0);
    expect(controls).toBeLessThan(launchers);
    expect(launchers).toBeLessThan(stage);
    expect(frameAll).toBeLessThan(stage);
    expect(source.indexOf('aria-label="Frame selected object"', header)).toBeLessThan(stage);
    expect(source.indexOf('aria-label="Reset camera"', header)).toBeLessThan(stage);
    expect(frameAll).toBeLessThan(transform);
    expect(transform).toBeLessThan(launchers);
    expect(source.includes("inspectorPanel === 'object' || inspectorPanel === 'robot'")).toBe(true);
    expect(source.includes('else if (!generalTransformEnabled) return;')).toBe(true);
    expect(css).toMatch(/\.viewer-header-controls\s*\{[^}]*flex:\s*1 1 auto/s);
    expect(css).toMatch(/\.viewer-surface-launchers\s*\{[^}]*margin-left:\s*auto/s);
    // Robot, Object and Tool & I/O.
    expect(source.slice(launchers, stage).match(/aria-pressed=\{inspectorPanel ===/g)).toHaveLength(3);
    expect(css).toMatch(
      /\.viewer-surface-launchers > button\[aria-pressed='true'\]\s*\{[^}]*border-color:\s*#68cadd;[^}]*background:\s*#17404c;/s,
    );
    expect(css).toMatch(
      /\.program-inspector\.central-inspector-section\s*\{[^}]*border-left:\s*0;/s,
    );
    expect(source).toContain('className="viewer-camera-icon"');
    expect(source).toContain('<group name="viewer-frame-content">');
  });

  it('frames content without changing the current viewing direction', () => {
    const box = new THREE.Box3(
      new THREE.Vector3(9, -2, 1),
      new THREE.Vector3(11, 4, 5),
    );
    const current = {
      position: [5, -7, 8] as [number, number, number],
      target: [1, 2, 3] as [number, number, number],
    };
    const framed = framedCameraPresentation(box, current, 48, 16 / 9);
    const oldDirection = new THREE.Vector3(...current.position)
      .sub(new THREE.Vector3(...current.target))
      .normalize();
    const newDirection = new THREE.Vector3(...framed.camera.position)
      .sub(new THREE.Vector3(...framed.camera.target))
      .normalize();

    expect(framed.camera.target).toEqual([10, 1, 3]);
    expect(newDirection.distanceTo(oldDirection)).toBeLessThan(1e-12);
  });

  it('excludes transform and axes helpers from semantic framing bounds', () => {
    const content = new THREE.Group();
    content.add(new THREE.Mesh(new THREE.BoxGeometry(2, 2, 2)));
    const transformHelper = new THREE.Group() as THREE.Group & {
      isTransformControls: boolean;
    };
    transformHelper.isTransformControls = true;
    transformHelper.add(new THREE.Mesh(new THREE.BoxGeometry(1_000, 1_000, 1_000)));
    content.add(transformHelper);
    content.add(new THREE.AxesHelper(2_000));

    const size = frameableContentBounds(content).getSize(new THREE.Vector3());

    expect(size.toArray()).toEqual([2, 2, 2]);
  });

  it('uses direct R3F semantic clicks instead of a second selection engine', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(source).not.toContain('function ViewerSelectionSurface');
    expect(source).not.toContain('event.intersections');
    expect(source).toContain('onClick={(event) => onSelect(event, link.name)}');
    expect(source).toContain(
      'onClick={(event) => onSelect(event, object.scene_resource_id)}',
    );
    expect(source).toContain('onClickCapture={clearBeforeSemanticClick}');
    expect(source).toContain('className="viewer-canvas-input-surface"');
    expect(source).not.toContain('onPointerMissed=');
    expect(source).not.toContain('new THREE.Raycaster()');
    expect(source).toContain("const ignoreViewerRaycast: THREE.Object3D['raycast']");
    for (const [axes] of source.matchAll(/<axesHelper[^>]*\/>/g)) {
      if (!axes.includes('args={[0.45]}')) expect(axes).toContain('raycast={ignoreViewerRaycast}');
    }
    expect(source).toContain('<mesh rotation={cylinderRotation} renderOrder={14} raycast={ignoreViewerRaycast}>');
    expect(source).not.toContain('transformRevision');
    expect(source).not.toContain('markTransformChange');
    expect(source).toContain('transformEnabled={transformEnabled && objectEditorActive}');
    expect(source).toContain('editable={candidateTcpEditable && robotEditorActive');
  });

  it('accepts a release through 250 ms and rejects longer holds', () => {
    expect(VIEWER_CLICK_MAX_DURATION_MS).toBe(250);
    expect(viewerClickWithinDuration({
      startedAtMs: 100,
      releasedAtMs: 350,
    })).toBe(true);
    expect(viewerClickWithinDuration({
      startedAtMs: 100,
      releasedAtMs: 351,
    })).toBe(false);
    expect(viewerClickWithinDuration({
      startedAtMs: 101,
      releasedAtMs: 100,
    })).toBe(false);
  });

  it('opens the object context surface only for an un-dragged right-button release', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(VIEWER_CONTEXT_DRAG_THRESHOLD_PX).toBe(3);
    expect(viewerContextWasDragged({
      startX: 10,
      startY: 10,
      currentX: 12,
      currentY: 12,
    })).toBe(false);
    expect(viewerContextWasDragged({
      startX: 10,
      startY: 10,
      currentX: 14,
      currentY: 10,
    })).toBe(true);
    expect(source).toContain('onPointerUpCapture={releaseContextPointer}');
    expect(source).toContain('onContextMenu={(event) => event.preventDefault()}');
    expect(source).not.toContain('onContextMenu={(event) => {');
  });

  it('routes authored and measured render layers through the same selection callbacks', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(source.match(/onSelect={selectStructure}/g)).toHaveLength(2);
    expect(source.match(/onSelect={selectSceneResource}/g)).toHaveLength(1);
  });

  it('drives the measured robot once and nests each Attached object under its carrier', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );
    const driverStart = source.indexOf('function MeasuredRobotFrameDriver');
    const measuredLinkStart = source.indexOf('function MeasuredRuntimeLink');
    const measuredLinkEnd = source.indexOf('function CandidateRobotGhost');
    const driver = source.slice(driverStart, measuredLinkStart);
    const measuredLink = source.slice(measuredLinkStart, measuredLinkEnd);

    expect(driver).toContain('livePresentation.subscribeMeasuredRobotPresentation(apply)');
    expect(driver.match(/subscribeMeasuredRobotPresentation/g)).toHaveLength(1);
    expect(driver).toContain('for (const [linkName, target] of linkGroups)');
    expect(driver).toContain('if (!acceptLatestLivePresentation() && !acceptedLiveFrame.current)');
    expect(driver).toContain('useFrame(() => {');
    expect(driver).toContain('}, -1_000);');
    expect(driver).toContain('continuationTimer.current = window.setTimeout');
    expect(driver).toContain('}, 1_000 / 60);');
    expect(driver).toContain('}, 0);');
    expect(driver).toContain('appliedAfterRenderTimer.current = window.setTimeout');
    expect(driver).toContain('rendered.reader.markMeasuredRobotPresentationApplied(rendered.presentation)');
    expect(driver).toContain('pendingAppliedPresentation.current = { presentation: current, reader: livePresentation! };');
    expect(driver).toMatch(
      /pendingAppliedPresentation\.current = \{ presentation: current, reader: livePresentation! \};[\s\S]*?if \(wakeAfterApply\) wake\(\);/,
    );
    expect(driver).not.toMatch(/const acceptPresentation[\s\S]*?wake\(\);[\s\S]*?return true;/);
    expect(driver).not.toContain('if (changed) wake();');
    expect(driver).not.toContain(
      'markMeasuredRobotPresentationApplied(current)',
    );
    expect(measuredLink).not.toContain('<AuthoritativeTransformGroup');
    expect(measuredLink).toContain('ref={groupRef}');
    expect(measuredLink).toContain('const [bootstrapTransform] = useState');
    expect(measuredLink).toContain('position={bootstrapTransform.position}');
    expect(measuredLink).toContain('quaternion={bootstrapTransform.quaternion}');
    expect(measuredLink).not.toContain('position={initialTransform.position_m}');
    expect(measuredLink).toContain('attachedObjects.map((object) => (');
    expect(measuredLink).toContain('<AttachedLogicalSceneObject');
    expect(source).toContain(
      'const current = result.get(object.carrierCompiledFrameName) ?? []',
    );
    expect(source).toContain(
      'attachedObjects={attachedObjectsByLink.get(link.name) ?? []}',
    );
    expect(source).toContain('position={[pose.position_m.x, pose.position_m.y, pose.position_m.z]}');
    expect(source).toContain('quaternion={normalizeQuaternion(pose.orientation_xyzw)}');
    expect(source).toContain('appearance="attached"');
    expect(source).toContain('axesSizeM={objectAxesSizeM}');
    expect(source).toMatch(
      /<MeasuredRuntimeModel[\s\S]*?objectAxesSizeM=\{viewer\?\.object_axes_gizmo_size_m \?\? 0\.1\}/,
    );
    expect(sceneObjectMaterial('attached', false)).toMatchObject({
      color: '#5fa84a',
      opacity: 0.85,
    });
    expect(source).toContain("object.placement.kind !== 'cell_fixed'");
  });

  it('mounts an Attached primitive at its exact carrier-relative pose and semantic style', () => {
    const object = sceneObject({ type: 'sphere', radius_m: 0.02 });
    const presentation: AttachedObjectPresentation = {
      object,
      attachmentId: 'attachment-1',
      attachmentRevision: 'attachment-revision-1',
      carrierComponentId: 'robot-1',
      commandTargetId: 'arm',
      carrierModelInstanceId: 'model-instance-1',
      carrierFrameRoleId: 'tcp',
      carrierCompiledFrameName: 'tool0',
      poseMeasurement: null,
      poseInCarrierFrame: {
        position_m: { x: 0.01, y: -0.02, z: 0.03 },
        orientation_xyzw: [0, 0, 0, 1],
      },
    };

    // react-dom/server does not know React Three Fiber's intrinsic elements;
    // suppress only that renderer mismatch while still exercising the real
    // component tree and its emitted transform/material attributes.
    const consoleError = vi.spyOn(console, 'error').mockImplementation(() => undefined);
    let markup = '';
    try {
      markup = renderToStaticMarkup(
        <AttachedLogicalSceneObject
          presentation={presentation}
          selected={false}
          axesVisible={false}
          axesSizeM={0.05}
          wireframe={false}
          ownerIdentity={undefined}
          onSelect={() => undefined}
        />,
      );
    } finally {
      consoleError.mockRestore();
    }

    expect(markup).toContain('name="scene:part"');
    expect(markup).toContain('position="0.01,-0.02,0.03"');
    expect(markup).toContain('quaternion="0,0,0,1"');
    expect(markup).toContain('color="#5fa84a"');
    expect(markup).toContain('opacity="0.85"');
  });

  it('renders exact measured robot, TCP, and object facts as diagnostic overlays without changing the primary objects', () => {
    const manifest: SceneUrdfManifest = {
      cell_id: 'cell-1',
      graph_revision: 'graph-1',
      retained_bundle_id: 'bundle-1',
      viewer_model_revision: 'viewer-1',
      model_key: 'model-1',
      root_link: 'base',
      projection: {
        kind: 'retained_neutral',
        source: 'active_retained_viewer_model',
        root_frame: 'base',
        position_unit: 'm',
        angle_unit: 'rad',
        quaternion_order: 'xyzw',
        joint_coordinate_policy: 'zero_independent_with_retained_mimic',
        operational_validity: 'not_asserted',
        runtime_state_used: false,
      },
      links: [{
        name: 'base',
        neutral_transform: {
          relative_to: 'base',
          position_m: [0, 0, 0],
          orientation_xyzw: [0, 0, 0, 1],
        },
        visuals: [{
          index: 0,
          mesh_url: null,
          primitive: { type: 'box', size_m: [0.2, 0.3, 0.4] },
          visual_origin: {
            position_m: [0.1, 0, 0],
            orientation_rpy_rad: [0, 0, 0],
            orientation_xyzw: [0, 0, 0, 1],
          },
        }],
        collisions: [],
      }],
      joints: [],
    };
    const robotPresentation: MeasuredRobotPresentation = {
      objectId: 'robot-1',
      sourceId: 'scene-runtime:robot',
      sourceEpoch: 'scene-epoch-4',
      sourceSequence: 9,
      observedAtNs: '1000' as MeasuredRobotPresentation['observedAtNs'],
      linkTransforms: {
        base: { position_m: [1, 2, 3], orientation_xyzw: [0, 0, 0, 1] },
      },
    };
    const observedPose: SceneRuntimeObservationPose = {
      frame_id: 'cell',
      frame_kind: 'cell_frame',
      position_m: [4, 5, 6],
      orientation_xyzw: [0, 0, 0, 1],
      source_stamp_ns: '900' as SceneRuntimeObservationPose['source_stamp_ns'],
    };
    const measuredPose = (
      objectId: string,
      policyId: string,
      sourceId: string,
    ): MeasuredPosePresentation => ({
      objectId,
      policyId,
      sourceId,
      sourceEpoch: 'scene-epoch-4',
      sourceSequence: 10,
      observedAtNs: '1000' as MeasuredPosePresentation['observedAtNs'],
      pose: observedPose,
    });
    const presentation: RuntimeScenePresentation = {
      measuredRobot: robotPresentation,
      measuredRobotSources: [],
      measuredTcp: measuredPose('tcp-1', 'viewer.measured_tcp', 'scene-runtime:tcp'),
      measuredObjects: [measuredPose('part', 'continuous_unattached', 'scene-runtime:part')],
      attachedObjects: [],
      attachmentDiagnostics: [],
      status: { total: 3, freshGood: 3, staleGood: 0, unknown: 0, buffered: 0, warnings: [] },
    };
    const consoleError = vi.spyOn(console, 'error').mockImplementation(() => undefined);
    let markup = '';
    try {
      markup = renderToStaticMarkup(
        <MeasuredStateGhostLayers
          enabled
          presentation={presentation}
          manifest={manifest}
          objects={[sceneObject({ type: 'box', dimensions_m: { x: 1, y: 2, z: 3 } })]}
          ownerIdentity={{ ownerEpoch: 'owner-1', generation: 4 }}
          onAssetError={() => undefined}
        >
          <span data-measured-tcp-marker="tcp-1" />
        </MeasuredStateGhostLayers>,
      );
    } finally {
      consoleError.mockRestore();
    }

    expect(markup).toContain('name="measured-state-ghost-layers"');
    // The comparison robot follows the viewer stream's measured rows
    // (MeasuredRobotStateGhost.test.tsx), never the scene-runtime copy.
    expect(markup).not.toContain('measured-state-robot');
    expect(markup).toContain('data-measured-tcp-marker="tcp-1"');
    expect(markup).toContain('name="measured-state-object:part:scene-runtime:part"');
    expect(markup).toContain('color="#43d7ec"');
    expect(markup).toContain('opacity="0.45"');
    expect(markup).toContain('<boxGeometry args="1,2,3"></boxGeometry>');
    // A carried object keeps its measured world-pose ghost during the carry.
    const carried = {
      ...sceneObject({ type: 'box', dimensions_m: { x: 1, y: 2, z: 3 } }),
      placement: {
        kind: 'carried',
        carried_object_attachment_id: 'attachment.part',
        carried_object_attachment_revision: 'attachment-revision-1',
      },
    } as unknown as LogicalSceneObject;
    expect(renderToStaticMarkup(
      <MeasuredStateGhostLayers
        enabled
        presentation={{ ...presentation, measuredTcp: null }}
        manifest={manifest}
        objects={[carried]}
        ownerIdentity={{ ownerEpoch: 'owner-1', generation: 4 }}
        onAssetError={() => undefined}
      />,
    )).toContain('name="measured-state-object:part:scene-runtime:part"');
    expect(renderToStaticMarkup(
      <MeasuredStateGhostLayers
        enabled={false}
        presentation={presentation}
        manifest={manifest}
        objects={[sceneObject({ type: 'box', dimensions_m: { x: 1, y: 2, z: 3 } })]}
        ownerIdentity={{ ownerEpoch: 'owner-1', generation: 4 }}
        onAssetError={() => undefined}
      />,
    )).toBe('');
  });

  it('keeps the local ghost preference selectable at rest when feeds are missing or stale', () => {
    const onChange = vi.fn();
    const toggle = MeasuredStateGhostToggle({ enabled: false, onChange });
    const input = (toggle.props.children as ReactElement[])[0] as ReactElement<{
      checked: boolean;
      disabled?: boolean;
      onChange: (event: { target: { checked: boolean } }) => void;
    }>;
    const empty: RuntimeScenePresentation = {
      measuredRobot: null,
      measuredRobotSources: [],
      measuredTcp: null,
      measuredObjects: [],
      attachedObjects: [],
      attachmentDiagnostics: [],
      status: { total: 2, freshGood: 0, staleGood: 2, unknown: 0, buffered: 0, warnings: [] },
    };

    expect(DEFAULT_MEASURED_STATE_GHOSTS_VISIBLE).toBe(false);
    expect(input.props.disabled).toBeUndefined();
    expect(renderToStaticMarkup(toggle)).not.toContain('disabled');
    expect(renderToStaticMarkup(
      <MeasuredStateGhostLayers
        enabled
        presentation={empty}
        manifest={null}
        objects={[]}
        ownerIdentity={undefined}
        onAssetError={() => undefined}
      />,
    )).toBe('');
    input.props.onChange({ target: { checked: true } });
    expect(onChange).toHaveBeenCalledWith(true);
    expect(renderToStaticMarkup(
      <MeasuredStateGhostLayers
        enabled={false}
        presentation={undefined}
        manifest={null}
        objects={[]}
        ownerIdentity={undefined}
        onAssetError={() => undefined}
      />,
    )).toBe('');
  });

  it('uses Attached and Detached as operator terms while retaining carried as an internal kind', () => {
    const viewer = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );
    const workspace = readFileSync(
      new URL('./SceneAuthoringWorkspace.tsx', import.meta.url),
      'utf8',
    );

    expect(workspace).toContain('This object is Attached in the live cell');
    expect(workspace).toContain('objects.length} objects · {attachments.length} Attached');
    expect(workspace).toContain('`Attached to ${attachment.controllable_component_id}');
    expect(viewer).not.toContain('Detach carried object');
    expect(workspace).not.toContain('carried-object placement');
  });

  it('creates the viewer Canvas once per context setting, never keyed on onCreated discoveries', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );
    const canvasStart = source.indexOf('const ViewerCanvas = memo(function ViewerCanvas({');
    const canvas = source.slice(canvasStart, source.indexOf('<ViewerQualificationProfiler', canvasStart));
    const canvasElement = canvas.slice(canvas.indexOf('    <Canvas'), canvas.indexOf('frameloop='));
    const keyLine = canvasElement.split('\n').find((line) => line.trim().startsWith('key=')) ?? '';
    // The key is the context-creation setting only; the renderer kind found
    // by onCreated (inspection.software/renderer) must not remount the Canvas.
    expect(keyLine).toContain("key={renderer.antialias ? 'aa' : 'no-aa'}");
    expect(keyLine).not.toMatch(/software|inspection|vendor/);
    // Mounts are seeded from the page-level inspection, and onCreated only
    // records into that cache.
    expect(canvas).toContain('pageViewerRendererInspection()');
    expect(canvas).toContain('rememberViewerRendererInspection(inspectViewerRenderer(gl))');
    // DPR stays live (a prop, not part of the key).
    expect(canvas).toContain('dpr={renderer.effectiveDpr}');
  });

  it('retains wheel camera presentation without React state commits', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(source).toContain('createRetainedCameraPresentationStore');
    expect(source).toContain('cameraPresentation.write(presentation)');
    expect(source).toContain('onPresentation(presentation)');
    expect(source).toContain("addEventListener?.('change', capture)");
    const captureStart = source.indexOf('function CaptureCameraPresentation');
    const captureEnd = source.indexOf('function ViewerCanvas', captureStart);
    expect(source.slice(captureStart, captureEnd)).not.toContain('useFrame');
    expect(source.slice(captureStart, captureEnd)).not.toContain('latestPresentationRef');
    expect(source).not.toContain('setCameraPresentation(presentation)');
  });

  it('frames robot, scene, measured overlays, and point clouds directly on demand', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );
    const content = source.indexOf('<group name="viewer-frame-content">');
    const contentEnd = source.indexOf('</group>', content);

    expect(content).toBeGreaterThan(0);
    expect(source.indexOf('<RetainedNeutralModel', content)).toBeLessThan(contentEnd);
    expect(source.indexOf('<LogicalSceneObjects', content)).toBeLessThan(contentEnd);
    expect(source.indexOf('<ObservedLogicalSceneObjects', content)).toBeLessThan(contentEnd);
    expect(source.indexOf('<PointCloudPoints', content)).toBeLessThan(contentEnd);
    expect(source).toContain('viewer-stage viewer-fallback-stage');
    expect(source).not.toContain('<Bounds');
    expect(source).not.toContain('useBounds');
    expect(source).toContain('framedCameraPresentation(\n      framedBounds,');
    expect(source).toContain('clipCameraToBounds(perspective, framedBounds)');
    expect(source).not.toContain('fit={camera === null');
    expect(source).toContain('position: [...DEFAULT_VIEWER_CAMERA.position]');
    expect(source).toContain('target: [...DEFAULT_VIEWER_CAMERA.target]');
    expect(source).not.toContain('framed: camera !== null');
  });

  it('shows neutral empty-Scenario text only from a verified empty composition and keeps real-model geometry guidance', () => {
    const workspace = readFileSync(
      new URL('./SceneAuthoringWorkspace.tsx', import.meta.url),
      'utf8',
    );
    const viewer = readFileSync(new URL('./StructuralViewer.tsx', import.meta.url), 'utf8');
    expect(workspace).toContain('verifiedAppliedGraphHasNoModels(');
    expect(workspace).toContain('headlessState.snapshot,\n    headlessState.transportStatus,');
    expect(workspace).toContain('noRobotScenario={noRobotScenario}');
    expect(viewer).toContain('noRobotScenario = false');
    expect(viewer).toContain('export function ViewerSceneEmptyState(');
    expect(viewer).toContain('<strong>No robot in this Scenario</strong>');
    expect(viewer).toContain('manifestReady={manifest.status === \'ready\'}');
    expect(viewer).toContain('The retained link tree contains no renderable visual or collision geometry.');
  });

  it('passes robot geometry only after Manager identity gating and leaves Stop controls mounted', () => {
    const workspace = readFileSync(
      new URL('./SceneAuthoringWorkspace.tsx', import.meta.url),
      'utf8',
    );
    // Only a verified (ready) or a retained, not-current model is presented.
    expect(workspace).toContain(
      "modelPresentation.status === 'ready'\n    || modelPresentation.status === 'retained'\n    ? modelPresentation.manifest\n    : null",
    );
    expect(workspace).toContain('const readyManifest = presentedManifest;');
    expect(workspace).toContain('manifest={viewerManifest}');
    expect(workspace).toContain('modelPresentation={modelPresentation}');
    expect(workspace).toContain('if (readyManifest === null || motionDescriptor === null) return null;');
    expect(workspace).toContain('<RobotControlPanelView');
    expect(workspace).toContain('robotControlSurface={(');
  });

  it('resolves owner scene meshes by content identity, not by owner epoch or generation', () => {
    const source = readFileSync(new URL('./StructuralViewer.tsx', import.meta.url), 'utf8');
    const mesh = source.slice(source.indexOf('function SceneObjectMesh({'), source.indexOf('export function clonePreviewMesh'));
    const dependencies = mesh.slice(mesh.lastIndexOf('}, ['));
    // Only the mesh source (URI, scale) or the Program Draft identity re-resolves.
    expect(dependencies).toContain('shape.resource_uri,');
    expect(dependencies).toContain('scaleX,');
    expect(dependencies).toContain('draftId,');
    expect(dependencies).toContain('draftRevision,');
    expect(dependencies).toContain('ownerBound,');
    expect(mesh).not.toMatch(/ownerEpoch|ownerGeneration|ownerIdentity\?\.generation/);
    // The content-addressed URL is used as-is: no owner-generation filtering,
    // no epoch-rotation retry, no parallel loaded-asset identity.
    expect(mesh).toContain('.then((asset) => asset.mesh_asset_url)');
    expect(source).not.toMatch(
      /retryableSceneMeshFailure|SCENE_MESH_RERESOLVE_DELAY_MS|resolveAttempt|retriedEpoch|retryTimer|loadedAsset|sceneMeshMetadataMatchesOwner|SUPERSEDED_SCENE_MESH_MESSAGE/,
    );
  });

  it('shows a failed scene mesh load and lets a later remount refetch the stable URL', () => {
    const source = readFileSync(new URL('./StructuralViewer.tsx', import.meta.url), 'utf8');
    const mesh = source.slice(source.indexOf('function SceneObjectMesh({'), source.indexOf('export function clonePreviewMesh'));
    expect(mesh).toContain('<AssetErrorBoundary resetKey={url} onError={isolateAssetError}>');
    // A load failure is reported (error box), and the cached failure for the
    // stable URL is dropped so a retry remount fetches it again.
    expect(mesh).toContain('if (url !== null) forgetSceneMeshAsset(url);\n    isolateError(failure);');
    expect(mesh).toContain('<meshBasicMaterial color={VIEWPORT_COLORS.error} wireframe />');
    expect(source).toContain('.clear?.(resolveStructuralAssetUrl(url));');
  });

  it('reloads scene geometry explicitly without remounting it for each pose generation', () => {
    const source = readFileSync(new URL('./StructuralViewer.tsx', import.meta.url), 'utf8');
    const resolution = source.slice(source.indexOf('function SceneObjectMesh({'), source.indexOf('export function clonePreviewMesh'));
    const dependencies = resolution.slice(resolution.lastIndexOf('}, ['));
    expect(dependencies).not.toContain('ownerGeneration,');
    expect(dependencies).not.toContain('ownerEpoch,');
    expect(dependencies).toContain('shape.resource_uri,');
    const reloadKeys = [...source.matchAll(
      /<(?:LogicalSceneObjects|ObservedLogicalSceneObjects)\s+key=(.+)/g,
    )].map((match) => match[1]);
    expect(reloadKeys).toHaveLength(1);
    expect(reloadKeys.every((key) => key.includes('retryGeneration'))).toBe(true);
  });

  it('isolates individual retained and logical-scene mesh failures', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(source).toContain('<AssetErrorBoundary resetKey={visual.mesh_url}');
    expect(source).toContain('<AssetErrorBoundary resetKey={collision.mesh_url}');
    expect(source).toContain('<AssetErrorBoundary resetKey={url} onError={isolateAssetError}>');
    expect(source).toContain('onIsolatedAssetError={reportIsolatedAssetError}');
    expect(source).toContain('onPreviewAssetError={reportPreviewAssetError}');
  });

  it('keeps geometry errors until a retried model load verifiably succeeds', () => {
    const source = readFileSync(new URL('./StructuralViewer.tsx', import.meta.url), 'utf8');
    const retry = source.match(/const retry = \(\) => \{([\s\S]*?)\n {2}\};/)?.[1] ?? '';
    // Retry remounts the model and waits; it never clears the conditions itself.
    expect(retry).toContain('setGeometryRetryGeneration(generation)');
    expect(retry).not.toContain('setAssetError(null)');
    expect(retry).not.toContain('setAssetWarning(null)');
    expect(source).toContain('resetKey={`${manifest.model_key}:${retryGeneration}`}');
    expect(source).toContain('<Suspense key={retryGeneration} fallback={null}>');
    // Only the loaded signal of that exact retry clears them; a new error ends it as failed.
    expect(source).toContain(
      'if (pendingGeometryRetry !== null && loadedGeometryGeneration === pendingGeometryRetry) {',
    );
    expect(source).toContain('endFailedRetry();\n    setAssetError(error);');
  });

  it('publishes viewer conditions to the one notification stack below the controls', () => {
    const source = readFileSync(new URL('./StructuralViewer.tsx', import.meta.url), 'utf8');
    const css = readStyleSource();
    expect(source).toContain("usePublishWorkbenchNotices('viewer', viewerNotices({");
    expect(source).not.toContain('viewer-notice-stack');
    expect(css).not.toContain('.viewer-notice-stack');
    expect(css).not.toContain('.viewer-model-state');
    const layout = css.match(/\.workbench-notification-stack\s*\{([^}]+)\}/)?.[1];
    // Below the floating controls (1.9rem toolbar + 2 × 0.45rem inset), same side insets.
    expect(layout).toContain('top: calc(1.9rem + 2 * 0.45rem)');
    expect(layout).toContain('left: 0.45rem');
    expect(layout).toContain('right: 0.45rem');
    expect(layout).toContain('pointer-events: none');
  });

  it('shares disposable object, link, and frame selection with camera tools', () => {
    const viewer = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );
    const workspace = readFileSync(
      new URL('./SceneAuthoringWorkspace.tsx', import.meta.url),
      'utf8',
    );

    expect(viewer).toContain('selectedStructureName');
    expect(viewer).toContain('scene.getObjectByName(`measured:${request.target.id}`)');
    expect(viewer).toContain('scene.getObjectByName(`link:${request.target.id}`)');
    expect(workspace).toContain("kind: 'owner_object'");
    expect(workspace).toContain("kind: 'program_resource'");
    expect(workspace).toContain("kind: 'structure'");
  });

  it('routes pose and Cartesian TCP gizmo edits through the existing candidate controller', () => {
    const viewer = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );
    const workspace = readFileSync(
      new URL('./SceneAuthoringWorkspace.tsx', import.meta.url),
      'utf8',
    );

    expect(viewer).toContain('candidateTcpEditable');
    expect(viewer).toContain('onEditCandidateTcpPose');
    expect(viewer.includes('gizmoSettings={gizmo.settings}')).toBe(true);
    expect(viewer.includes('onBeginCandidateTcpDrag')).toBe(true);
    expect(workspace).toContain("robotControl.state.mode === 'pose_goal'");
    expect(workspace).toContain("robotControl.state.mode === 'cartesian_path'");
    // Identity-stable Canvas callbacks delegate to the latest drag closures.
    expect(workspace.includes('onEditCandidateTcpPose={finishCandidateDragStable}')).toBe(true);
    // Zero-change and robot-moved releases reach the panel as typed reasons.
    expect(viewer).toContain('onEdit(null, noChange);');
    expect(workspace).toContain("robotControl.finishPoseDrag(null, 'context_changed');");
    expect(workspace).toContain('robotControl.finishPoseDrag(null, noChange);');
    const gizmo = readFileSync(new URL('./AuthoringTransformControls.tsx', import.meta.url), 'utf8');
    expect(gizmo).toContain("onCancel(start.moved && settings.snap ? 'snapped_no_change' : 'no_change')");
    expect(workspace).toContain('candidateDragHandlers.current = { begin: beginCandidateDrag, finish: finishCandidateDrag }');
    expect(workspace).toContain('candidateTcpPoseFromCell(');
    expect(workspace).toContain('robotControlCandidatePresentationAllowed(');
    expect(workspace).toContain('candidatePresentationAllowed={candidatePresentationAllowed}');
    expect(workspace).not.toContain("programRuntime.cycleState === 'idle_after_reset'");
    expect(workspace).not.toContain("programRuntime.cycleState !== 'idle_after_reset'");
  });

  it('suspends only viewer observers while retaining outer presentation state', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(source).toContain('const pointCloudEnabled = viewerActive');
    expect(source).toContain('{viewerActive && (');
    expect(source).toContain('<BoundSnapshotResponder');
    expect(source).not.toContain("sceneOwnerIdentity?.generation ?? -1");
    expect(source.indexOf('const [pointCloudVisible')).toBeLessThan(
      source.indexOf('{viewerActive && ('),
    );
  });

  it('keeps qualification observation opt-in and demand-framed', () => {
    const source = readFileSync(
      new URL('./StructuralViewer.tsx', import.meta.url),
      'utf8',
    );

    expect(source).toContain('Profiler id="manyforge-structural-viewer"');
    expect(source).toContain('<ViewerPerformanceProbe />');
    expect(source).toContain('recordViewerRenderFrame(nowMs)');
    expect(source).toContain('recordViewerRenderWork(');
    expect(source).toContain('startViewerPerformanceMonitoring()');
    expect(source).toContain('stopViewerPerformanceMonitoring()');
    expect(source).toContain('connectViewerQualification()');
    expect(source).toContain('frameloop="demand"');
    expect(source).not.toContain('frameloop="always"');
  });
});
