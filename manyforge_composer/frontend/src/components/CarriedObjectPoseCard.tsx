import { useRef, useState } from 'react';

import { commandRefusalCause, commandRefusalText } from '../state/commandRefusal';
import type { CarriedObjectAttachment } from '../types/api';
import type { AttachedObjectPoseMeasurement } from '../state/runtimeScenePresentation';
import { OverlayPanel } from './OverlayPanel';

function poseText(pose: AttachedObjectPoseMeasurement['current_offset']): string {
  const position = pose.position_m;
  return `position [${position.x}, ${position.y}, ${position.z}] m; quaternion [${pose.orientation_xyzw.join(', ')}]`;
}

type DetachConfirmation = Pick<CarriedObjectAttachment,
  'carried_object_attachment_id' | 'scene_resource_id' | 'controllable_component_id' | 'command_target_id'>;

function sameAttachment(left: DetachConfirmation, right: CarriedObjectAttachment): boolean {
  return left.carried_object_attachment_id === right.carried_object_attachment_id
    && left.scene_resource_id === right.scene_resource_id
    && left.controllable_component_id === right.controllable_component_id
    && left.command_target_id === right.command_target_id;
}

function detachErrorText(error: unknown): string {
  const refusal = commandRefusalCause(error);
  if (refusal !== null) return commandRefusalText(refusal);
  return error instanceof Error ? error.message : 'Detach failed.';
}

export function CarriedObjectPoseCard({ measurement, attachment, detachUnavailableReason, onDetach }: {
  measurement: AttachedObjectPoseMeasurement | null;
  /** Undefined hides the action; null shows it disabled while the attachment is unavailable. */
  attachment?: CarriedObjectAttachment | null;
  detachUnavailableReason?: string | null;
  onDetach?: (attachment: CarriedObjectAttachment) => Promise<void>;
}) {
  const [confirmation, setConfirmation] = useState<DetachConfirmation | null>(null);
  const [pending, setPending] = useState(false);
  const [detachError, setDetachError] = useState<string | null>(null);
  const [detachStatus, setDetachStatus] = useState<string | null>(null);
  const detachButton = useRef<HTMLButtonElement>(null);
  const cardSection = useRef<HTMLElement>(null);
  const confirmationReturnFocus = useRef<HTMLElement | null>(null);
  const closeConfirmation = () => {
    setConfirmation(null);
    const trigger = detachButton.current;
    confirmationReturnFocus.current = trigger !== null && !trigger.disabled
      ? trigger
      : cardSection.current;
    window.requestAnimationFrame(() => confirmationReturnFocus.current?.focus());
  };
  const available = detachUnavailableReason == null && attachment !== null && attachment !== undefined
    && onDetach !== undefined;
  const confirmationMatches = confirmation !== null && attachment !== null && attachment !== undefined
    && sameAttachment(confirmation, attachment);
  const confirmDetach = async () => {
    if (confirmation === null || !confirmationMatches || !available || attachment === null || attachment === undefined
      || onDetach === undefined) return;
    setPending(true);
    setDetachError(null);
    setDetachStatus(null);
    try {
      await onDetach(attachment);
      closeConfirmation();
      setDetachStatus(`Detached ${attachment.scene_resource_id} at its current pose.`);
    } catch (error) {
      closeConfirmation();
      setDetachError(detachErrorText(error));
    } finally {
      setPending(false);
    }
  };
  return (
    <section ref={cardSection} className="dcard" aria-label="Carried object pose" tabIndex={-1}>
      <h4>Pose</h4>
      {measurement === null ? (
        <p className="muted" role="status">
          No resolved carried-object pose measurement is available for this object.
        </p>
      ) : (
        <>
          <p className="scene-contract-note hint" role="status">
            {measurement.fresh ? 'Fresh pose sample.' : 'Stale pose sample; showing the last reported values.'}
          </p>
          <dl>
            {'holder' in measurement && (
              <div>
                <dt>Holder</dt>
                <dd>
                  <code>{measurement.holder.controllable_component_id}</code>
                  {' · target · '}
                  <code>{measurement.holder.command_target_id}</code>
                  {' · gripper base link · '}
                  <code>{measurement.holder.gripper_link}</code>
                </dd>
              </div>
            )}
            <div>
              <dt>Measured against</dt>
              <dd>{typeof measurement.measured_against === 'string'
                ? measurement.measured_against
                : <>
                  <code>{measurement.measured_against.entity}</code>
                  {' · body link · '}
                  <code>{measurement.measured_against.link}</code>
                </>}</dd>
            </div>
            <div>
              <dt>Frame (offset expressed in)</dt>
              <dd>
                <code>
                  {measurement.frame.controllable_component_id}
                  {' / '}
                  {measurement.frame.model_instance_id}
                  {' / '}
                  {measurement.frame.frame_role_id}
                </code>
              </dd>
            </div>
            {'source_header_frame_id' in measurement && measurement.source_header_frame_id !== undefined && (
              <div>
                <dt>Source header frame</dt>
                <dd><code>{measurement.source_header_frame_id}</code></dd>
              </div>
            )}
            <div>
              <dt>{measurement.fresh ? 'Current offset' : 'Last reported offset (stale)'}</dt>
              <dd>{poseText(measurement.current_offset)}</dd>
            </div>
            {measurement.baseline_offset !== undefined && measurement.drift !== undefined && (
              <>
                <div>
                  <dt>Grasp baseline offset</dt>
                  <dd>{poseText(measurement.baseline_offset)}</dd>
                </div>
                <div>
                  <dt>Drift from grasp baseline</dt>
                  <dd>
                    {measurement.drift.translation_m} m translation ·{' '}
                    {measurement.drift.rotation_rad} rad rotation
                  </dd>
                </div>
              </>
            )}
          </dl>
        </>
      )}
      {attachment !== undefined && (
        <div className="dactions">
          <button
            ref={detachButton}
            type="button"
            className="danger-button"
            disabled={!available || pending}
            title={detachUnavailableReason ?? undefined}
            onClick={() => {
              if (!available || attachment === null || attachment === undefined) return;
              setDetachError(null);
              setDetachStatus(null);
              setConfirmation({
                carried_object_attachment_id: attachment.carried_object_attachment_id,
                scene_resource_id: attachment.scene_resource_id,
                controllable_component_id: attachment.controllable_component_id,
                command_target_id: attachment.command_target_id,
              });
            }}
          >Detach</button>
          {detachUnavailableReason !== null && detachUnavailableReason !== undefined && (
            <p className="field-hint" role="status">{detachUnavailableReason}</p>
          )}
        </div>
      )}
      <OverlayPanel
        open={confirmation !== null}
        title="Confirm carried-object detach"
        titleId="carried-object-detach-confirmation-title"
        kind="dialog"
        compact
        onClose={closeConfirmation}
        returnFocusRef={confirmationReturnFocus}
      >
        <div className="dialog-copy transport-confirmation">
          {confirmation !== null && (
            <p>{`Detach ${confirmation.scene_resource_id} from ${confirmation.controllable_component_id} / ${confirmation.command_target_id} at its current pose?`}</p>
          )}
          {detachUnavailableReason !== null && detachUnavailableReason !== undefined && (
            <p className="field-error" role="status">{detachUnavailableReason}</p>
          )}
          {!confirmationMatches && (
            <p className="field-error" role="status">The carried-object attachment changed. Review the confirmation again.</p>
          )}
          <div className="dialog-actions">
            <button type="button" disabled={pending} onClick={closeConfirmation}>Cancel</button>
            <button
              type="button"
              className="danger-button"
              disabled={!available || !confirmationMatches || pending}
              onClick={() => confirmDetach()}
            >Detach</button>
          </div>
        </div>
      </OverlayPanel>
      {detachError !== null && <p className="field-error" role="alert">{detachError}</p>}
      {detachStatus !== null && <p className="scene-contract-note hint" role="status">{detachStatus}</p>}
    </section>
  );
}
