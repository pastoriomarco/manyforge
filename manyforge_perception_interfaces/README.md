# Request-driven perception interface

One `EstimateInstances` goal selects one real RGB-D capture, detects once,
then returns selected instances and an authoritative final result. Feedback
is informational; clients must use the result to reconstruct final state.

Capture and pose headers always carry the original source timestamp and
optical frame. Unique internal processing timestamps never appear here.
`request_id`, `attempt`, and `provider_session` identify the action job;
`capture_epoch` and `calibration_id` identify the selected camera input.

Goal work budget is mandatory and at least one second, beginning on goal
acceptance. Completion grace is a provider setting (default 1.5 seconds).
Cancellation or timeout permits immediate next-goal acceptance; a terminal
result does not assert native GPU work has drained. Default client settings
are NEXT, REFINE, five instances and five seconds. Zero `max_instances` means
no count limit; a zero goal `settle_frames` selects the provider setting.

`execution_id` is an optional caller-supplied Program-run identity, encoded
as an empty string when unavailable. With prior age disabled, priors belong
to the same execution identity; without one, to the same provider session.
Age disabling never disables depth/colour gates or camera/model invalidation.

Boxes use `[x, y, width, height]` in source pixels. A space region is an
axis-aligned box in the source optical frame, in metres. Instance indices
refer to detections. An unposed instance has no usable pose: clients must
check status before reading pose values. No pose confidence is invented;
only the detector score is reported. Timing fields are local elapsed
milliseconds, not capture-clock differences.

CPU source checkpoint: generated types and four actual ROS action stand-in
cases passed on the recorded Orin Lyrical image with source13. This does not
qualify cross-distribution action transport, NVIDIA inference or Program use.
