import type { AttachedObjectPoseMeasurement } from '../state/runtimeScenePresentation';

function poseText(pose: AttachedObjectPoseMeasurement['current_offset']): string {
  const position = pose.position_m;
  return `position [${position.x}, ${position.y}, ${position.z}] m; quaternion [${pose.orientation_xyzw.join(', ')}]`;
}

export function CarriedObjectPoseCard({ measurement }: {
  measurement: AttachedObjectPoseMeasurement | null;
}) {
  return (
    <section className="dcard" aria-label="Carried object pose">
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
    </section>
  );
}
