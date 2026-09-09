import { useLayoutEffect, useRef, useState } from 'react';
import { useThree } from '@react-three/fiber';
import type { EventDispatcher, Object3D } from 'three';
import { TransformControls } from 'three-stdlib';
import { snapGizmoPose } from '../utils/gizmo';
import type { GizmoSettings } from '../utils/gizmo';
import { samePose } from '../utils/poseTransforms';
import type { PoseTuple } from '../utils/poseTransforms';

const poseOf = (object: Object3D): PoseTuple => ({
  position: object.position.toArray(), quaternion: object.quaternion.clone().normalize().toArray(),
});

/** Same installed gizmo, with explicit gesture/listener ownership. No effects
 * in render: this also survives React StrictMode's setup/cleanup replay. */
export function AuthoringTransformControls({ object, settings, contextKey, onBegin, onCommit, onCancel }: {
  object: Object3D;
  settings: GizmoSettings;
  contextKey: string;
  onBegin: () => boolean;
  onCommit: () => void;
  onCancel: () => void;
}) {
  const element = useThree((state) => state.gl.domElement);
  const camera = useThree((state) => state.camera);
  const get = useThree((state) => state.get);
  const invalidate = useThree((state) => state.invalidate);
  const [control, setControl] = useState<TransformControls | null>(null);
  const [generation, setGeneration] = useState(0);
  const latest = useRef({ settings, onBegin, onCommit, onCancel });
  useLayoutEffect(() => { latest.current = { settings, onBegin, onCommit, onCancel }; });
  const gestureKey = JSON.stringify([contextKey, settings]);
  useLayoutEffect(() => {
    const control = new TransformControls(camera, undefined);
    const settings = latest.current.settings;
    control.setMode(settings.mode);
    control.setSpace(settings.space);
    control.attach(object);
    control.connect(element);
    let drag: { pose: PoseTuple; scale: [number, number, number] } | null = null;
    let interaction: { camera: { enabled: boolean } | null; enabled: boolean; touchAction: string } | null = null;
    const releaseInteraction = () => {
      if (interaction === null) return;
      if (interaction.camera !== null) interaction.camera.enabled = interaction.enabled;
      element.style.setProperty('touch-action', interaction.touchAction);
      interaction = null;
    };
    const clearDrag = () => {
      if (drag === null) return false;
      const start = drag;
      drag = null;
      object.position.fromArray(start.pose.position);
      object.quaternion.fromArray(start.pose.quaternion);
      object.scale.fromArray(start.scale);
      releaseInteraction();
      latest.current.onCancel();
      return true;
    };
    const cancel = () => {
      if (drag === null) return;
      // Quiesce native pointer movement immediately, before React replaces
      // the control. detach() is public and prevents any further object move.
      control.detach();
      clearDrag();
      setGeneration((value) => value + 1);
    };
    const down = () => {
      if (!latest.current.onBegin()) {
        control.detach();
        releaseInteraction();
        setGeneration((value) => value + 1);
        return;
      }
      drag = { pose: poseOf(object), scale: object.scale.toArray() };
      if (interaction?.camera !== null && interaction?.camera !== undefined) interaction.camera.enabled = false;
    };
    const change = () => {
      if (drag === null) return;
      const pose = snapGizmoPose(drag.pose, poseOf(object), settings);
      object.position.fromArray(pose.position);
      object.quaternion.fromArray(pose.quaternion);
      invalidate();
    };
    const up = () => {
      const start = drag;
      drag = null;
      releaseInteraction();
      if (start === null) return;
      if (samePose(start.pose, poseOf(object)) && start.scale.every((value, index) => value === object.scale.getComponent(index))) {
        latest.current.onCancel();
      } else latest.current.onCommit();
    };
    const captureInteraction = () => {
      if (drag !== null) return;
      const camera = get().controls as { enabled: boolean } | null;
      interaction = { camera, enabled: camera?.enabled ?? false, touchAction: element.style.touchAction };
    };
    const escape = (event: KeyboardEvent) => { if (event.key === 'Escape') cancel(); };
    const hidden = () => { if (document.hidden) cancel(); };
    const redraw = () => invalidate();
    // Installed three-stdlib exposes these public events at runtime but its
    // declaration inherits only Object3D's event map.
    const events = control as unknown as EventDispatcher<Record<
      'mouseDown' | 'objectChange' | 'mouseUp' | 'change', Record<string, never>
    >>;
    events.addEventListener('mouseDown', down);
    events.addEventListener('objectChange', change);
    events.addEventListener('mouseUp', up);
    events.addEventListener('change', redraw);
    element.addEventListener('pointerdown', captureInteraction, true);
    window.addEventListener('keydown', escape, true);
    window.addEventListener('blur', cancel);
    window.addEventListener('pointercancel', cancel);
    document.addEventListener('visibilitychange', hidden);
    element.addEventListener('lostpointercapture', cancel);
    setControl(control);
    invalidate();
    return () => {
      clearDrag();
      releaseInteraction();
      control.detach();
      control.dispose();
      events.removeEventListener('mouseDown', down);
      events.removeEventListener('objectChange', change);
      events.removeEventListener('mouseUp', up);
      events.removeEventListener('change', redraw);
      element.removeEventListener('pointerdown', captureInteraction, true);
      window.removeEventListener('keydown', escape, true);
      window.removeEventListener('blur', cancel);
      window.removeEventListener('pointercancel', cancel);
      document.removeEventListener('visibilitychange', hidden);
      element.removeEventListener('lostpointercapture', cancel);
    };
  }, [camera, element, generation, gestureKey, get, invalidate, object]);
  return control === null ? null : <primitive object={control} />;
}
