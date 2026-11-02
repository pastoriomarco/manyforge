# Request-driven pose provider

Source checkpoint: two new packages containing interfaces, the pure provider
model, native translation, a composed action wrapper, an ordered Orin launch
and a qualification client. The Orin Lyrical CPU build passed, together with
48 behavioral cases (34 model, six translation and eight ROS action/launch
fixtures) and all 13 registered CTest checks. Native integration and recorded
Orin inference qualification remain pending.
This is not a qualified product entrypoint. No changes to existing continuous
perception or ManyForge Core are included.

The model takes exact monotonic nanoseconds as arguments. Source camera time
is separate. It retains two raw captures by default, one current action job
and one awaited native processing stamp. A counter allocates distinct stamps
across cancel/timeout/preempt and capture-clock resets; no old-job registry
or background retry is retained. Overflow never wraps. One caller serializes
model methods; callbacks cannot block waiting for native inference.

Budget starts at acceptance. With no native work in flight it ends at the
work deadline; already admitted detection/pose work can finish in completion
grace. A late result cannot admit another estimate past the work deadline.
Cancellation frees the current job immediately and does not claim GPU idle.
External results retain their selected real capture metadata.
The goal callback retains only a bounded decision and its original acceptance
time. It cannot create or preempt a model job: the actual accepted-handle
callback commits the matching UUID. A failed framework goal-response send
therefore leaves no orphan job; a later decision replaces its unused context.
The original budget is enforced before any native payload preparation.

Region membership is per-request WHOLE (default) or CENTRE. WHOLE requires
every valid mask-depth pixel inside; CENTRE uses the mask's median valid
depth at its box centre. An explicit region with no valid depth is outside.
Both rules use the same optional padded full-resolution mask as native
inference; `mask_margin_px` defaults to zero.
No-region selection does not impose a region test. Image boxes are source
pixels; spatial boxes require the actual optical frame without TF lookup.

Pure operations include source-mask geometry, greedy one-to-one prior
matching, a three-way depth/colour gate and symmetry-aware refinement limits.
No comparable pixels is a distinct full-estimation route, never unchanged.
Serial refinement is checked against its own prior, otherwise full estimation
starts only within the work budget. Reused instances are relayed first.
The cache uses one configurable byte budget (initial 32 MiB, unqualified),
evicting whole least-recently-used viewpoints. An oversize entry is skipped
without changing results or adding an instance/viewpoint count limit. Cache
accounting includes unused outer vector capacity and string terminators.
Execution or capture-epoch invalidation releases outer backing storage.
The budget covers retained values/capacities, not allocator bookkeeping,
current-job images/crops, or total process RSS. Required tolerance
settings have no invented production defaults. Age defaults to 30 seconds
and can be disabled; disabled age scopes priors to the caller execution
identity, or the provider session when that identity is absent. Tests use
deliberately named fixture settings, not measured production tolerances.

The translator retains CPU source-message owners, hashes K/D/size for the
calibration identity, and prepares a separately stamped native input packet.
The caller must confirm that admission in the model after packet preparation
and immediately before publication. This includes the CPU preparation interval
in the work budget and rechecks a refinement prior's age. Native pose and
centered matrix outputs must have the same current processing stamp. The
public original-mesh pose and exact centered seed matrix are separate values;
the matrix never supplies the external pose. CPU-only Buffer access refuses
an unsupported backend rather than silently copying a CUDA image while idle.
Source translation failures are caught at the capture boundary and reported
through the existing throttled refusal log; they cannot escape as an
unsupported-backend exception from the subscription callback.
The encoder unletterbox geometry currently targets the recorded 1280x720,
640x640 encoder baseline; arbitrary odd-resolution resize rounding is unqualified.

ROS terminal mapping: COMPLETE, VALID_EMPTY and
PARTIAL use success with their authoritative outcome. FAILED uses abort.
An accepted client cancellation uses CANCELED with outcome CANCELLED; server
preemption uses standard abort with outcome CANCELLED. Cancellation releases
the model immediately and does not prove GPU idle. A single pending client
cancel terminal is drained on the next callback after the framework enters
canceling; it cannot delay admission of the next valid goal.

`phase1_orin.launch.py` constructs the seven-node encoder, YOLO inference and
decoder, full/refinement TensorRT components, FoundationPose and one-shot
tracking, then the sequencer, in one container. The existing scheduling timer
checks service discovery every 100 ms without a terminal deadline; it then
cancels and sends exactly one existing ListNodes request. Until the exact
known node names and unique IDs are returned, the action is unavailable.
Missing/error replies remain unavailable, and an absent reply remains visibly
startup-pending. There is no query retry, inference warmup or automatic restart.
The query proves construction, not engine execution readiness. A hanging
query is a qualification failure to diagnose; it does not establish the cause
of any other DDS service incident. The supported restart boundary is the whole
container, including all native nodes; no sequencer-only restart or persistence.

The supplied YAML lists approved initial values only: two captures, 1.5 s
completion grace, age enabled at 30 s, 32 MiB cache and zero mask margin.
All model identities, settle/duplicate/crop/matching/gate/refinement settings
and asset symmetries must be supplied explicitly. Their values require review
and measurement before an Orin run; this file deliberately cannot construct
the provider by itself. Engines and mesh are explicit existing-file launch
arguments; no download or engine rebuild is included. The native assembly
targets the recorded trocar asset with continuous X symmetry; supply that
asset's `asset_symmetry_axes: [x_full]` explicitly. The launch resolves this
required setting using standard ROS component parameter-file handling and
passes the same axes to FoundationPose and the sequencer. Another asset needs
a reviewed assembly/configuration pair. Required numeric settings accept
integer or floating-point YAML values; their existing bounds still apply.

The native YOLO decoder retains its confidence floor of 0.25. A request's
`min_detection_score` can further filter these detections; a lower request
value cannot recover detections already discarded by the decoder.

`perception_test_client` sends the real action and prints its complete feedback,
result and ROS terminal code. It requires model/request identity, score and an
explicit service-discovery wait bound. Its request defaults are NEXT, REFINE,
five instances and a five-second work budget; the defaults are qualification
targets, not measured guarantees. It does not add Program integration.

The pure tests cover state, geometry, budgets, cache and warm routes. Translator
tests distinguish the public original-mesh pose from the centered native seed.
The action test loads the actual wrapper through standard LoadNode and uses
native detection and ListNodes response stand-ins: it covers valid-empty
feedback, source stamps, timeout/immediate next, client cancellation, server
preemption, stale output and current malformed native output. It proves neither
NVIDIA construction nor inference, atomic seed acknowledgement, GPU draining,
live camera operation or measured tolerances. Native output correlation and
alternating two real refinement seeds remain mandatory Orin qualification.
Full FoundationPose uses native ApproximateTime over RGB/depth/info/mask and
reports an RGB-derived output stamp. Returned stamp equality does not prove
coherence of the other three input messages. Full-input association across
immediate next, cancel and partial publication is an unresolved correctness
qualification seam; no support claim, native fork or queue-flush protocol is
inferred from the current wrapper's output guard.
