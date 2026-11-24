import * as THREE from 'three';

export interface ViewerCamera {
  position: [number, number, number];
  target: [number, number, number];
}

export const DEFAULT_VIEWER_CAMERA: ViewerCamera = {
  position: [2.3, -2.3, 1.7],
  target: [0, 0, 0.45],
};

function parseTriple(raw: string | null): [number, number, number] | null {
  if (raw === null) return null;
  const parts = raw.split(',').map((part) => Number(part.trim()));
  if (parts.length !== 3 || parts.some((part) => !Number.isFinite(part))) {
    return null;
  }
  return [parts[0], parts[1], parts[2]];
}

/** Parse a shareable local camera pin without creating a mixed viewer store. */
export function parseViewerCameraQuery(search: string): ViewerCamera | null {
  const params = new URLSearchParams(search);
  const positionRaw = params.get('cam');
  const targetRaw = params.get('target');
  if (positionRaw === null && targetRaw === null) return null;
  return {
    position: parseTriple(positionRaw) ?? [...DEFAULT_VIEWER_CAMERA.position],
    target: parseTriple(targetRaw) ?? [...DEFAULT_VIEWER_CAMERA.target],
  };
}

/** The camera that frames `box` from the current viewing direction (default: the default camera's), with a margin. */
export function framedCameraPresentation(
  box: THREE.Box3,
  current: ViewerCamera,
  verticalFovDegrees: number,
  aspect: number,
  margin = 1.25,
): { camera: ViewerCamera } {
  if (box.isEmpty()) return { camera: current };
  const size = box.getSize(new THREE.Vector3());
  const center = box.getCenter(new THREE.Vector3());
  const maximumSize = Math.max(size.x, size.y, size.z, 0.01);
  const verticalDistance = maximumSize
    / (2 * Math.tan(THREE.MathUtils.degToRad(verticalFovDegrees) / 2));
  const distance = margin * Math.max(
    verticalDistance,
    verticalDistance / Math.max(aspect, Number.EPSILON),
  );
  const direction = new THREE.Vector3(...current.position)
    .sub(new THREE.Vector3(...current.target));
  if (direction.lengthSq() < Number.EPSILON) {
    direction
      .fromArray(DEFAULT_VIEWER_CAMERA.position)
      .sub(new THREE.Vector3(...DEFAULT_VIEWER_CAMERA.target));
  }
  direction.normalize();
  const position = center.clone().addScaledVector(direction, distance);
  return {
    camera: {
      position: [position.x, position.y, position.z],
      target: [center.x, center.y, center.z],
    },
  };
}

export function frameableContentBounds(root: THREE.Object3D): THREE.Box3 {
  const bounds = new THREE.Box3();
  const objectBounds = new THREE.Box3();
  root.updateWorldMatrix(true, true);

  const visit = (object: THREE.Object3D): void => {
    if (
      !object.visible
      || (object as THREE.Object3D & { isTransformControls?: boolean })
        .isTransformControls === true
      || object.type === 'AxesHelper'
    ) return;
    const bounded = object as THREE.Object3D & {
      boundingBox?: THREE.Box3 | null;
      computeBoundingBox?: () => void;
      geometry?: THREE.BufferGeometry;
    };
    let localBounds: THREE.Box3 | null | undefined;
    if (bounded.boundingBox !== undefined) {
      if (bounded.boundingBox === null) bounded.computeBoundingBox?.();
      localBounds = bounded.boundingBox;
    } else if (bounded.geometry !== undefined) {
      if (bounded.geometry.boundingBox === null) {
        bounded.geometry.computeBoundingBox();
      }
      localBounds = bounded.geometry.boundingBox;
    }
    if (localBounds !== null && localBounds !== undefined && !localBounds.isEmpty()) {
      objectBounds.copy(localBounds).applyMatrix4(object.matrixWorld);
      bounds.union(objectBounds);
    }
    for (const child of object.children) visit(child);
  };

  visit(root);
  return bounds;
}
