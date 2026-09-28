import { useEffect, useMemo, useRef } from 'react';
import { useFrame, useThree } from '@react-three/fiber';
import * as THREE from 'three';
import { EffectComposer } from 'three/examples/jsm/postprocessing/EffectComposer.js';
import { OutputPass } from 'three/examples/jsm/postprocessing/OutputPass.js';
import { Pass } from 'three/examples/jsm/postprocessing/Pass.js';
import { RenderPass } from 'three/examples/jsm/postprocessing/RenderPass.js';
import { ShaderPass } from 'three/examples/jsm/postprocessing/ShaderPass.js';

import { VIEWPORT_COLORS } from '../styles/tokens';

/** Group whose solid meshes (robots, Scene objects, tools) are outlined. */
export const OUTLINED_CONTENT_NAME = 'viewer-frame-content';

/** Edge look: thin core line plus a short soft glow. */
const OUTLINE_STRENGTH = 0;
const OUTLINE_GLOW = 0.25;
const OUTLINE_GLOW_RADIUS_PX = 1;

/** Solid, opaque surfaces only: collision wireframes, ghosts and previews
 * (transparent or wireframe), helpers with invisible materials (the
 * transform gizmo's picking plane) and points are not outlined. */
function outlineable(object: THREE.Object3D): boolean {
  const mesh = object as THREE.Mesh;
  if (!mesh.isMesh) return false;
  const materials = Array.isArray(mesh.material) ? mesh.material : [mesh.material];
  return materials.some((material) => (
    material.visible
    && !material.transparent
    && material.depthTest
    && !(material as THREE.MeshBasicMaterial).wireframe
  ));
}

/**
 * The component a mesh belongs to: a robot link resolves to its model
 * instance (the whole robot, the gripper…), a Scene object to its Scene
 * resource. Everything under one component shares one outline.
 */
function componentKey(
  object: THREE.Object3D,
  content: THREE.Object3D,
  componentByLink: ReadonlyMap<string, string>,
): string {
  for (let node: THREE.Object3D | null = object; node !== null && node !== content; node = node.parent) {
    const name = node.name;
    const link = name.startsWith('link:') ? name.slice(5)
      : name.startsWith('measured:') ? name.slice(9)
        : null;
    if (link !== null) return `model:${componentByLink.get(link) ?? link}`;
    if (name.startsWith('scene:')) return `scene:${name.slice(6)}`;
    if (name.startsWith('observed-scene:')) return `scene:${name.slice(15)}`;
  }
  return `mesh:${object.uuid}`;
}

function insideOf(object: THREE.Object3D, ancestor: THREE.Object3D): boolean {
  for (let node: THREE.Object3D | null = object; node !== null; node = node.parent) {
    if (node === ancestor) return true;
  }
  return false;
}

/** Draws each outlined component in one flat ID colour (0 = background). */
class ComponentIdPass extends Pass {
  readonly target: THREE.WebGLRenderTarget;
  private readonly scene: THREE.Scene;
  camera: THREE.Camera;
  componentByLink: ReadonlyMap<string, string> = new Map();
  private readonly ids = new Map<string, number>();
  private readonly materials = new Map<string, THREE.MeshBasicMaterial>();
  private readonly clearColor = new THREE.Color();

  constructor(scene: THREE.Scene, camera: THREE.Camera) {
    super();
    this.scene = scene;
    this.camera = camera;
    this.needsSwap = false;
    this.target = new THREE.WebGLRenderTarget(1, 1, {
      minFilter: THREE.NearestFilter,
      magFilter: THREE.NearestFilter,
      type: THREE.UnsignedByteType,
    });
  }

  setSize(width: number, height: number) {
    this.target.setSize(width, height);
  }

  private material(id: number, side: THREE.Side): THREE.MeshBasicMaterial {
    const key = `${id}:${side}`;
    let material = this.materials.get(key);
    if (material === undefined) {
      // Exact bytes in the target: linear colour, no tone mapping.
      material = new THREE.MeshBasicMaterial({ side, toneMapped: false });
      material.color.setRGB(
        (id & 0xff) / 255,
        ((id >> 8) & 0xff) / 255,
        ((id >> 16) & 0xff) / 255,
        THREE.LinearSRGBColorSpace,
      );
      this.materials.set(key, material);
    }
    return material;
  }

  render(renderer: THREE.WebGLRenderer) {
    const content = this.scene.getObjectByName(OUTLINED_CONTENT_NAME);
    const swapped: Array<[THREE.Mesh, THREE.Material | THREE.Material[]]> = [];
    const hidden: THREE.Object3D[] = [];
    this.scene.traverseVisible((object) => {
      const drawable = (object as THREE.Mesh).isMesh
        || (object as THREE.Points).isPoints
        || (object as THREE.Line).isLine
        || (object as THREE.Sprite).isSprite;
      if (!drawable) return;
      // Only solid surfaces occlude or get an ID, as in the normal render.
      if (!outlineable(object)) {
        hidden.push(object);
        return;
      }
      const mesh = object as THREE.Mesh;
      // Opaque things outside the content still occlude, as background.
      let id = 0;
      if (content !== undefined && insideOf(mesh, content)) {
        const key = componentKey(mesh, content, this.componentByLink);
        id = this.ids.get(key) ?? this.ids.size + 1;
        this.ids.set(key, id);
      }
      const side = (Array.isArray(mesh.material) ? mesh.material[0] : mesh.material).side;
      swapped.push([mesh, mesh.material]);
      mesh.material = this.material(id, side);
    });
    for (const object of hidden) object.visible = false;
    const background = this.scene.background;
    const clearAlpha = renderer.getClearAlpha();
    renderer.getClearColor(this.clearColor);
    try {
      this.scene.background = null;
      renderer.setClearColor(0x000000, 1);
      renderer.setRenderTarget(this.target);
      renderer.clear();
      renderer.render(this.scene, this.camera);
    } finally {
      this.scene.background = background;
      renderer.setClearColor(this.clearColor, clearAlpha);
      for (const [mesh, material] of swapped) mesh.material = material;
      for (const object of hidden) object.visible = true;
    }
  }

  dispose() {
    this.target.dispose();
    for (const material of this.materials.values()) material.dispose();
    this.materials.clear();
  }
}

/** Marks pixels whose component ID differs from a neighbour's: a thin core
 * line on the component side, plus a short glow on both sides. */
const EDGE_SHADER = {
  uniforms: {
    tDiffuse: { value: null as THREE.Texture | null },
    tId: { value: null as THREE.Texture | null },
    texel: { value: new THREE.Vector2(1, 1) },
    color: { value: new THREE.Color(VIEWPORT_COLORS.outline) },
    strength: { value: OUTLINE_STRENGTH },
    glow: { value: OUTLINE_GLOW },
  },
  vertexShader: /* glsl */ `
    varying vec2 vUv;
    void main() {
      vUv = uv;
      gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
    }`,
  fragmentShader: /* glsl */ `
    uniform sampler2D tDiffuse;
    uniform sampler2D tId;
    uniform vec2 texel;
    uniform vec3 color;
    uniform float strength;
    uniform float glow;
    varying vec2 vUv;
    const float RADIUS = ${OUTLINE_GLOW_RADIUS_PX.toFixed(1)};

    bool differs(vec3 a, vec2 offset) {
      vec3 b = texture2D(tId, vUv + offset * texel).rgb;
      return any(greaterThan(abs(a - b), vec3(0.5 / 255.0)));
    }

    void main() {
      vec4 base = texture2D(tDiffuse, vUv);
      vec3 id = texture2D(tId, vUv).rgb;
      bool component = any(greaterThan(id, vec3(0.5 / 255.0)));
      // Core: 1px, drawn on the component pixel next to a boundary.
      float core = 0.0;
      if (component && (differs(id, vec2(1.0, 0.0)) || differs(id, vec2(-1.0, 0.0))
          || differs(id, vec2(0.0, 1.0)) || differs(id, vec2(0.0, -1.0)))) {
        core = 1.0;
      }
      // Glow: distance to the nearest boundary within RADIUS, both sides.
      float nearest = RADIUS + 1.0;
      for (float r = 1.0; r <= RADIUS; r += 1.0) {
        if (differs(id, vec2(r, 0.0)) || differs(id, vec2(-r, 0.0))
            || differs(id, vec2(0.0, r)) || differs(id, vec2(0.0, -r))
            || differs(id, vec2(r, r) * 0.7071) || differs(id, vec2(-r, r) * 0.7071)
            || differs(id, vec2(r, -r) * 0.7071) || differs(id, vec2(-r, -r) * 0.7071)) {
          nearest = r;
          break;
        }
      }
      float halo = nearest <= RADIUS ? glow * (1.0 - (nearest - 1.0) / RADIUS) : 0.0;
      float edge = max(core * strength, halo);
      gl_FragColor = vec4(base.rgb + color * edge, base.a);
    }`,
};

/**
 * Per-component outline: every robot, gripper and Scene object gets its own
 * edge, against the background and against each other. Takes over
 * rendering while mounted; unmount it to return to the plain render.
 */
export function SceneOutline({
  multisample,
  componentByLink,
}: {
  multisample: boolean;
  componentByLink: ReadonlyMap<string, string>;
}) {
  const gl = useThree((state) => state.gl);
  const scene = useThree((state) => state.scene);
  const camera = useThree((state) => state.camera);
  const size = useThree((state) => state.size);
  const invalidate = useThree((state) => state.invalidate);
  const componentsRef = useRef(componentByLink);
  useEffect(() => {
    componentsRef.current = componentByLink;
    invalidate();
  }, [componentByLink, invalidate]);

  const pipeline = useMemo(() => {
    const target = new THREE.WebGLRenderTarget(1, 1, {
      type: THREE.HalfFloatType,
      samples: multisample ? 4 : 0,
    });
    const composer = new EffectComposer(gl, target);
    const render = new RenderPass(scene, camera);
    const ids = new ComponentIdPass(scene, camera);
    const edges = new ShaderPass(EDGE_SHADER);
    edges.uniforms.tId.value = ids.target.texture;
    composer.addPass(render);
    composer.addPass(ids);
    composer.addPass(edges);
    composer.addPass(new OutputPass());
    return { composer, render, ids, edges };
    // The camera is refreshed per frame; a new renderer or scene rebuilds.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [gl, scene, multisample]);

  useEffect(() => {
    const ratio = gl.getPixelRatio();
    pipeline.composer.setPixelRatio(ratio);
    pipeline.composer.setSize(size.width, size.height);
    pipeline.edges.uniforms.texel.value.set(
      1 / Math.max(1, Math.round(size.width * ratio)),
      1 / Math.max(1, Math.round(size.height * ratio)),
    );
    invalidate();
  }, [gl, invalidate, pipeline, size.height, size.width]);

  useEffect(() => {
    invalidate();
    return () => {
      pipeline.ids.dispose();
      pipeline.edges.dispose();
      pipeline.composer.dispose();
      invalidate();
    };
  }, [invalidate, pipeline]);

  useFrame(() => {
    pipeline.render.camera = camera;
    pipeline.ids.camera = camera;
    pipeline.ids.componentByLink = componentsRef.current;
    pipeline.composer.render();
  }, 1);

  return null;
}
