"""Deployment artifact loader and validator for ManyForge."""

from __future__ import annotations

import copy
import hashlib
import math
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

import yaml

from manyforge_behavior.capabilities import (
    CAPABILITY_REGISTRY,
    get_adapter_capability_manifest,
    get_required_keys,
)
from manyforge_behavior.catalog import SEMVER_RE
from manyforge_behavior.model_paths import normalize_package_paths, resolve_resource_path

EXPECTED_API_VERSION = "manyforge.deployment.v0"
EXPECTED_MODEL_ENDPOINT_PROFILE_REGISTRY_API_VERSION = (
    "manyforge.model_endpoint_profiles.v0"
)
_REMOVED_COOPERATIVE_INTERRUPT_FIELDS = frozenset({"interrupt_sources"})

# Conservative deployment-wide TCP linear-speed safety cap (m/s) used as the
# default when a RobotConfig is constructed in code (e.g. a new-deployment
# scaffold). Deployment YAML loaders still REQUIRE the cap explicitly and reject
# an omitted or non-positive value (see _parse_robot_config) — the cap is
# mandatory; this default only gives programmatic construction a safe starting
# value and resolves the 0.0-vs-">0" inconsistency the bare default used to have.
DEFAULT_GLOBAL_MAX_TCP_LINEAR_SPEED_MPS = 0.25


@dataclass
class RobotControlJointLimit:
    """Per-joint validated Robot Control operator caps (spec/430 §3.2).

    These bound operator-authored Robot Control targets; they are not a
    live-streaming concept (default Robot Control authors validated
    candidates, it does not stream motion).

    `max_jerk_rad_s3` is carried through the request DTO end-to-end but is only
    enforceable where the active planning path constrains jerk; the discrete
    TOPP-RA path does not, so a supplied jerk cap is rejected at request
    validation (`unsupported_joint_dynamics_limit`) rather than silently
    accepted (see VALIDATED_ROBOT_CONTROL_PLAN Phase 1K)."""

    max_velocity_rad_s: Optional[float] = None
    max_acceleration_rad_s2: Optional[float] = None
    max_jerk_rad_s3: Optional[float] = None


@dataclass
class RobotConfig:
    robot_id: str
    group_id: str
    urdf_path: str
    srdf_path: str = ""
    package_paths: list[str] = field(default_factory=list)
    global_max_tcp_linear_speed_mps: float = DEFAULT_GLOBAL_MAX_TCP_LINEAR_SPEED_MPS
    # Per-joint validated Robot Control operator caps keyed by joint name.
    # Absent joints fall back to backend engine defaults (0.5 rad/s velocity,
    # 2.0 rad/s² acceleration). Validation: joint names must be in the
    # program's drivable joint set; loaders enforce strictly-positive values.
    robot_control_joint_limits: dict[str, "RobotControlJointLimit"] = field(
        default_factory=dict
    )


@dataclass
class GripperConfig:
    command_mode: str
    group_id: str
    joint_name: str
    motion_time_s: float
    srdf_presets: dict[str, float] = field(default_factory=dict)
    description: str = ""
    touch_links: list[str] = field(default_factory=list)
    mount_frame: str = ""
    attach_frame: str = ""
    tcp_frame: str = ""
    auxiliary_frames: list[str] = field(default_factory=list)


@dataclass
class AdapterConfig:
    adapter_id: str
    adapter_version: str
    config: dict[str, Any] = field(default_factory=dict)
    supported_profiles: list[str] = field(default_factory=list)
    capability_keys: list[str] = field(default_factory=list)


@dataclass(frozen=True)
class ContactPolicyConfig:
    policy_id: str
    raw: dict[str, Any]


@dataclass
class OrchestratorConfig:
    engine: str
    config: dict[str, Any] = field(default_factory=dict)


@dataclass
class RunnerConfig:
    module: str
    args: list[str] = field(default_factory=list)


@dataclass
class UiBackendConfig:
    module: str = ""
    args: list[str] = field(default_factory=list)
    enabled: bool = True


@dataclass(frozen=True)
class PoseSourceMeasuredStateGhostConfig:
    """Per-object external measured-state overlay settings."""

    enabled: bool = False
    robot_tcp: bool = True
    object: bool = True
    opacity: float = 0.5


@dataclass(frozen=True)
class PoseSourceMeasuredAttachmentBaselineConfig:
    """Measured grasp-baseline capture policy for an attached object."""

    capture: str = "on_attach_or_first_paired_sample"
    require_same_time_robot_tcp: bool = True


@dataclass(frozen=True)
class PoseSourceAttachedDriftMonitorConfig:
    """Measured TCP/object drift monitor policy for an attached object."""

    enabled: bool = False
    translation_threshold_m: Optional[float] = None
    rotation_threshold_rad: Optional[float] = None
    debounce_s: float = 0.1
    stale_behavior: str = "fault"
    action: str = "interrupt"
    signal_id: str = ""
    sample_time_tolerance_s: float = 0.05


@dataclass(frozen=True)
class PoseSourceMeasuredTcpConfig:
    """Optional source-level measured TCP entity sampled with object poses."""

    entity: str
    tcp_frame_id: str = ""
    frame_id: str = ""


@dataclass(frozen=True)
class PoseSourceMeasuredRobotConfig:
    """Optional measured robot joint-state stream for visual overlays."""

    joint_state_topic: str = "/joint_states"


@dataclass(frozen=True)
class PoseSourceObjectBinding:
    """Deployment binding from an external pose source to a scene object."""

    object_id: str
    apply_policy: str = "observe_only"
    entity: str = ""
    frame_id: str = ""
    confidence_min: Optional[float] = None
    measured_state_ghost: Optional[PoseSourceMeasuredStateGhostConfig] = None
    measured_attachment_baseline: Optional[
        PoseSourceMeasuredAttachmentBaselineConfig
    ] = None
    attached_drift_monitor: Optional[PoseSourceAttachedDriftMonitorConfig] = None
    metadata: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class PoseSourceConfig:
    """Contract-only pose source declaration.

    Runtime sidecars consume these declarations in later slices. The
    deployment loader validates authority and shape now so malformed
    deployments fail closed before any source can mutate or observe scene
    state.
    """

    source_id: str
    kind: str
    enabled: bool = True
    max_update_hz: float = 4.0
    stale_after_s: float = 1.0
    frame_id: str = ""
    service_prefix: str = "auto"
    topic: str = ""
    description: str = ""
    measured_tcp: Optional[PoseSourceMeasuredTcpConfig] = None
    measured_robot: Optional[PoseSourceMeasuredRobotConfig] = None
    objects: dict[str, PoseSourceObjectBinding] = field(default_factory=dict)


@dataclass(frozen=True)
class SimulationControlCapabilities:
    """Deployment-declared simulator control operations."""

    reset: bool = False
    pause: bool = False
    play: bool = False
    step: bool = False
    set_entity_state: bool = False


@dataclass(frozen=True)
class SimulationResetConfig:
    """Reset policy for simulator lifecycle control."""

    default_scope: str = "state"
    close_command_gate: bool = True
    wait_ready_s: float = 3.0
    resume_play_after_reset: bool = True


@dataclass(frozen=True)
class SimulationControlConfig:
    """Contract-only simulator control declaration."""

    kind: str
    service_prefix: str = "auto"
    capabilities: SimulationControlCapabilities = field(
        default_factory=SimulationControlCapabilities
    )
    reset: SimulationResetConfig = field(default_factory=SimulationResetConfig)

    def capability_keys(self) -> set[str]:
        keys: set[str] = set()
        if self.capabilities.reset:
            keys.add("simulator.control.reset")
        if self.capabilities.pause:
            keys.add("simulator.control.pause")
        if self.capabilities.play:
            keys.add("simulator.control.play")
        if self.capabilities.step:
            keys.add("simulator.control.step")
        if self.capabilities.set_entity_state:
            keys.add("simulator.entity_state.set")
        return keys


@dataclass
class AuthorityDomain:
    owner: str
    on_disconnect: str
    description: str = ""


@dataclass
class LaunchConfig:
    runner: RunnerConfig
    ui_backend: Optional[UiBackendConfig] = None


class DeploymentValidationError(Exception):
    """Raised when deployment artifact validation fails."""
    pass


_ALLOWED_HASH_ALGORITHMS = {"sha256", "sha384", "sha512"}
_ALLOWED_POSE_SOURCE_KINDS = {
    "simulation_interfaces",
    "pose_stamped_topic",
    "pose_array_topic",
    "detection3d_array_topic",
    "tf",
}
_ALLOWED_POSE_SOURCE_APPLY_POLICIES = {
    "observe_only",
    "when_idle",
    "continuous",
    "continuous_unattached",
    "before_planning",
    "between_nodes",
}
_ALLOWED_SIMULATION_CONTROL_KINDS = {
    "ros2_simulation_interfaces",
}
_ALLOWED_SIMULATION_RESET_SCOPES = {
    "state",
}
_SIMULATION_CONTROL_CAPABILITY_FLAGS = {
    "reset",
    "pause",
    "play",
    "step",
    "set_entity_state",
}
_ALLOWED_GRIPPER_COMMAND_MODES = {
    "simulated_joint",
    "ros_gripper_command",
}
_ALLOWED_AUTHORITY_DOMAINS = {
    "motion",
    "scene",
    "gripper",
    "plc_signals",
    "robot_signals",
    "perception",
    "simulation",
}
_ALLOWED_AUTHORITY_OWNERS = {
    "manyforge",
    "plc",
    "robot_controller",
    "perception_service",
    "operator",
    "shared",
}
_ALLOWED_AUTHORITY_DISCONNECT_POLICIES = {
    "ignore",
    "hold",
    "reset_required",
    "latched_fault",
}
_ALLOWED_IMAGE_TRANSPORTS = {"inline_data_url", "http_url"}
_ALLOWED_ASSISTANT_PROVIDERS = {
    "mock",
    "deterministic-scene-authoring",
    "nemoclaw",
    "openclaw",
}
_ASSISTANT_PROVIDERS_REQUIRING_ENDPOINT = {"nemoclaw", "openclaw"}
_ALLOWED_ASSISTANT_TOOL_EFFECTS = {
    "read_only",
    "proposal",
    "composer_draft_mutating",
    "runtime_mutating",
}
_ALLOWED_ASSISTANT_SESSION_KINDS = {
    "manual_intervention",
    "delegated_recovery",
    "not_applicable",
}
_ALLOWED_ASSISTANT_APPROVAL_MODES = {
    "human_approved",
    "policy_granted",
}
_ALLOWED_ASSISTANT_REVIEW_POLICIES = {
    "required",
    "bounded_by_recovery_profile",
    "not_required",
}
_QUERY_ASSISTANT_MODE_ID = "query"
_COMPOSER_ASSISTANT_MODE_ID = "composer-assistant"


def _validate_capability_keys(value: Any, path: str) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, list) or not all(
        isinstance(capability, str) and capability.strip()
        for capability in value
    ):
        raise DeploymentValidationError(
            f"{path} must be a list of non-empty strings"
        )
    unknown_capabilities = sorted(
        capability
        for capability in value
        if capability not in CAPABILITY_REGISTRY
    )
    if unknown_capabilities:
        raise DeploymentValidationError(
            f"{path} contains unknown capability keys: {unknown_capabilities}"
        )
    return list(value)


@dataclass
class ModelAuditPolicy:
    """Deployment-level audit and redaction policy for model calls (OP-9)."""
    store_full_prompts: bool = False
    store_full_responses: bool = False
    hash_algorithm: str = "sha256"
    redact_argument_values: bool = True


@dataclass
class ModelEndpointCapabilities:
    """Declared capabilities for a deployment-configured model endpoint."""
    supports_response_tools: bool = False
    supports_json_object: bool = False
    supports_image_input: bool = False
    supported_image_transports: list[str] = field(default_factory=list)
    tool_call_family: str = ""
    latency_budget_ms: Optional[int] = None


@dataclass
class ModelEndpoint:
    """Deployment-configured model endpoint alias (OP-7)."""
    alias: str
    provider: str  # "openai", "anthropic", "local", etc.
    model_id: str
    profile_ref: str = ""
    base_url: Optional[str] = None
    image_fetch_base_url: Optional[str] = None
    api_key_env: Optional[str] = None  # env var name, never the key itself
    default_timeout_s: float = 30.0
    max_tokens: int = 4096
    capabilities: ModelEndpointCapabilities = field(
        default_factory=ModelEndpointCapabilities
    )


@dataclass
class AssistantConfig:
    """Deployment-selected Composer assistant provider."""
    provider: str
    endpoint_alias: str = ""
    enabled: bool = True


@dataclass(frozen=True)
class AssistantToolDefinition:
    """Deployment-declared model-visible assistant tool."""
    tool_id: str
    effect: str
    description: str = ""
    extra: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class AssistantModeAuthority:
    """Authority and review policy declared by an assistant mode."""
    session_kind: str
    approval_mode: str
    review: str


@dataclass(frozen=True)
class AssistantModeCatalog:
    """Finite instrument allowlist declared by an assistant mode."""
    skills: list[str] = field(default_factory=list)
    tools: list[str] = field(default_factory=list)
    nodes: list[str] = field(default_factory=list)


@dataclass(frozen=True)
class AssistantModeDefinition:
    """Deployment-declared assistant mode."""
    mode_id: str
    enabled: bool
    description: str
    authority: AssistantModeAuthority
    catalog: AssistantModeCatalog


@dataclass
class PromptTemplate:
    """Deployment-side versioned prompt template (OP-7)."""
    template_id: str
    version: str
    template_text: str
    output_schema: Optional[dict[str, Any]] = None
    response_tool: Optional[dict[str, Any]] = None

    @property
    def content_hash(self) -> str:
        return hashlib.sha256(self.template_text.encode()).hexdigest()[:16]


class Deployment:
    """Loads and validates a ManyForge deployment artifact from YAML."""

    def __init__(self, path: str | Path):
        self._path = Path(path)
        self.name: str = ""
        self.description: str = ""
        self.api_version: str = ""
        self.schema_version: str = ""
        self.program_profile: str = ""
        self.robot_config: Optional[RobotConfig] = None
        self.grippers: dict[str, GripperConfig] = {}
        self.adapters: dict[str, AdapterConfig] = {}
        self.orchestrator: Optional[OrchestratorConfig] = None
        self.launch: Optional[LaunchConfig] = None
        self.pose_sources: dict[str, PoseSourceConfig] = {}
        self.simulation_control: Optional[SimulationControlConfig] = None
        self.contact_policies: list[ContactPolicyConfig] = []
        self.authority_matrix: dict[str, AuthorityDomain] = {}
        self.model_audit_policy: Optional[ModelAuditPolicy] = None
        self.model_endpoint_profile_registry: Optional[str] = None
        self.model_endpoints: dict[str, ModelEndpoint] = {}
        self.assistant: Optional[AssistantConfig] = None
        self.assistant_modes: dict[str, AssistantModeDefinition] = {}
        self.tool_catalog: dict[str, AssistantToolDefinition] = {}
        self.prompt_templates: dict[str, PromptTemplate] = {}
        self._load_and_validate()

    @property
    def source_path(self) -> Path:
        return self._path

    @classmethod
    def from_dict(
        cls,
        data: dict[str, Any],
        path: str | Path = "<memory>",
    ) -> "Deployment":
        """Construct and validate a deployment directly from a mapping."""
        self = cls.__new__(cls)
        self._path = Path(path)
        self.name = ""
        self.description = ""
        self.api_version = ""
        self.schema_version = ""
        self.program_profile = ""
        self.robot_config = None
        self.grippers = {}
        self.adapters = {}
        self.orchestrator = None
        self.launch = None
        self.pose_sources = {}
        self.simulation_control = None
        self.contact_policies = []
        self.authority_matrix = {}
        self.model_audit_policy = None
        self.model_endpoint_profile_registry = None
        self.model_endpoints = {}
        self.assistant = None
        self.assistant_modes = {}
        self.tool_catalog = {}
        self.prompt_templates = {}
        self._load_from_data_and_validate(data)
        return self

    def _load_and_validate(self) -> None:
        with open(self._path) as f:
            data = yaml.safe_load(f)

        self._load_from_data_and_validate(data)

    def _parse_robot_config(
        self,
        robot_data: dict[str, Any] | None,
        *,
        current: RobotConfig | None = None,
    ) -> RobotConfig:
        if not isinstance(robot_data, dict):
            raise DeploymentValidationError("robot_config must be a mapping")

        merged = {
            "robot_id": current.robot_id if current is not None else "",
            "group_id": current.group_id if current is not None else "",
            "urdf_path": current.urdf_path if current is not None else "",
            "srdf_path": current.srdf_path if current is not None else "",
            "package_paths": list(current.package_paths) if current is not None else [],
            "global_max_tcp_linear_speed_mps": (
                current.global_max_tcp_linear_speed_mps if current is not None else 0.0
            ),
            "robot_control_joint_limits": (
                {k: v for k, v in current.robot_control_joint_limits.items()}
                if current is not None else {}
            ),
        }
        merged.update(robot_data)

        # Clean break (no silent fallback): the legacy `streaming_joint_limits`
        # key was renamed to `robot_control_joint_limits`. Reject it explicitly
        # so an operator who kept an old deployment fails loud at load time
        # instead of running with their per-joint caps silently dropped.
        if "streaming_joint_limits" in robot_data:
            raise DeploymentValidationError(
                "robot_config.streaming_joint_limits was removed; rename it to "
                "robot_control_joint_limits (validated Robot Control per-joint "
                "caps). The old key is no longer honored."
            )

        for field_name in ("robot_id", "group_id", "urdf_path"):
            if not str(merged.get(field_name, "")).strip():
                raise DeploymentValidationError(
                    f"robot_config.{field_name} is required"
                )

        tcp_speed = merged.get("global_max_tcp_linear_speed_mps", 0.0)
        try:
            tcp_speed = float(tcp_speed)
        except (TypeError, ValueError) as exc:
            raise DeploymentValidationError(
                "robot_config.global_max_tcp_linear_speed_mps must be a number"
            ) from exc
        if tcp_speed <= 0:
            raise DeploymentValidationError(
                "robot_config.global_max_tcp_linear_speed_mps must be > 0"
            )

        package_paths = merged.get("package_paths", [])
        if package_paths is None:
            package_paths = []
        if not isinstance(package_paths, list) or any(
            not isinstance(value, str) or not value.strip() for value in package_paths
        ):
            raise DeploymentValidationError(
                "robot_config.package_paths must be a list of non-empty strings"
            )

        robot_control_limits_raw = merged.get("robot_control_joint_limits", {})
        if robot_control_limits_raw is None:
            robot_control_limits_raw = {}
        if not isinstance(robot_control_limits_raw, dict):
            raise DeploymentValidationError(
                "robot_config.robot_control_joint_limits must be a mapping of "
                "joint_name -> {max_velocity_rad_s, max_acceleration_rad_s2}"
            )
        robot_control_limits: dict[str, RobotControlJointLimit] = {}
        for joint_name, limit_raw in robot_control_limits_raw.items():
            if isinstance(limit_raw, RobotControlJointLimit):
                robot_control_limits[str(joint_name)] = limit_raw
                continue
            if not isinstance(limit_raw, dict):
                raise DeploymentValidationError(
                    f"robot_config.robot_control_joint_limits['{joint_name}'] must be a mapping"
                )
            def _parse_cap(raw: object, dim_name: str, _jn: str = str(joint_name)):
                # Per-joint dynamics caps fail closed: a present value must be a
                # finite, strictly-positive number (rejects NaN/inf and <= 0).
                if raw is None:
                    return None
                try:
                    val = float(raw)  # type: ignore[arg-type]
                except (TypeError, ValueError) as exc:
                    raise DeploymentValidationError(
                        f"robot_control_joint_limits['{_jn}'].{dim_name} must be a number"
                    ) from exc
                if not math.isfinite(val) or val <= 0:
                    raise DeploymentValidationError(
                        f"robot_control_joint_limits['{_jn}'].{dim_name} must be a "
                        "finite number > 0"
                    )
                return val

            v_val = _parse_cap(limit_raw.get("max_velocity_rad_s"), "max_velocity_rad_s")
            a_val = _parse_cap(
                limit_raw.get("max_acceleration_rad_s2"), "max_acceleration_rad_s2"
            )
            j_val = _parse_cap(limit_raw.get("max_jerk_rad_s3"), "max_jerk_rad_s3")
            robot_control_limits[str(joint_name)] = RobotControlJointLimit(
                max_velocity_rad_s=v_val,
                max_acceleration_rad_s2=a_val,
                max_jerk_rad_s3=j_val,
            )

        return RobotConfig(
            robot_id=str(merged["robot_id"]),
            group_id=str(merged["group_id"]),
            urdf_path=str(merged["urdf_path"]),
            srdf_path=str(merged.get("srdf_path", "")),
            package_paths=[value.strip() for value in package_paths],
            global_max_tcp_linear_speed_mps=tcp_speed,
            robot_control_joint_limits=robot_control_limits,
        )

    def _parse_authority_matrix(
        self,
        authority_matrix_data: dict[str, Any] | None,
    ) -> dict[str, AuthorityDomain]:
        if authority_matrix_data and not isinstance(authority_matrix_data, dict):
            raise DeploymentValidationError("authority_matrix must be a mapping")
        parsed: dict[str, AuthorityDomain] = {}
        for domain, domain_data in (authority_matrix_data or {}).items():
            if domain not in _ALLOWED_AUTHORITY_DOMAINS:
                allowed = ", ".join(sorted(_ALLOWED_AUTHORITY_DOMAINS))
                raise DeploymentValidationError(
                    f"authority_matrix.{domain} is not a supported domain; "
                    f"allowed domains: {allowed}"
                )
            if not isinstance(domain_data, dict):
                raise DeploymentValidationError(
                    f"authority_matrix.{domain} must be a mapping"
                )
            owner = str(domain_data.get("owner", "")).strip()
            on_disconnect = str(domain_data.get("on_disconnect", "")).strip()
            if owner not in _ALLOWED_AUTHORITY_OWNERS:
                allowed = ", ".join(sorted(_ALLOWED_AUTHORITY_OWNERS))
                raise DeploymentValidationError(
                    f"authority_matrix.{domain}.owner must be one of: {allowed}"
                )
            if on_disconnect not in _ALLOWED_AUTHORITY_DISCONNECT_POLICIES:
                allowed = ", ".join(sorted(_ALLOWED_AUTHORITY_DISCONNECT_POLICIES))
                raise DeploymentValidationError(
                    f"authority_matrix.{domain}.on_disconnect must be one of: {allowed}"
                )
            parsed[domain] = AuthorityDomain(
                owner=owner,
                on_disconnect=on_disconnect,
                description=str(domain_data.get("description", "")),
            )
        return parsed

    def _parse_pose_sources(
        self,
        pose_sources_data: list[dict[str, Any]] | None,
    ) -> dict[str, PoseSourceConfig]:
        if pose_sources_data is None:
            return {}
        if not isinstance(pose_sources_data, list):
            raise DeploymentValidationError("pose_sources must be a list")

        def get_alias(data: dict[str, Any], snake: str, camel: str, default: Any) -> Any:
            if snake in data:
                return data[snake]
            if camel in data:
                return data[camel]
            return default

        def parse_bool(value: Any, field_path: str) -> bool:
            if not isinstance(value, bool):
                raise DeploymentValidationError(f"{field_path} must be a boolean")
            return value

        def parse_ghost_config(
            raw: Any,
            field_path: str,
        ) -> Optional[PoseSourceMeasuredStateGhostConfig]:
            if raw is None:
                return None
            if not isinstance(raw, dict):
                raise DeploymentValidationError(f"{field_path} must be a mapping")
            enabled = parse_bool(
                get_alias(raw, "enabled", "enabled", False),
                f"{field_path}.enabled",
            )
            robot_tcp = parse_bool(
                get_alias(raw, "robot_tcp", "robotTcp", True),
                f"{field_path}.robot_tcp",
            )
            object_enabled = parse_bool(
                get_alias(raw, "object", "object", True),
                f"{field_path}.object",
            )
            opacity = self._parse_bounded_float(
                get_alias(raw, "opacity", "opacity", 0.5),
                f"{field_path}.opacity",
                minimum=0.05,
                maximum=1.0,
            )
            return PoseSourceMeasuredStateGhostConfig(
                enabled=enabled,
                robot_tcp=robot_tcp,
                object=object_enabled,
                opacity=opacity,
            )

        def parse_baseline_config(
            raw: Any,
            field_path: str,
        ) -> Optional[PoseSourceMeasuredAttachmentBaselineConfig]:
            if raw is None:
                return None
            if not isinstance(raw, dict):
                raise DeploymentValidationError(f"{field_path} must be a mapping")
            capture = self._parse_optional_text(
                get_alias(
                    raw,
                    "capture",
                    "capture",
                    "on_attach_or_first_paired_sample",
                ),
                f"{field_path}.capture",
            )
            if capture != "on_attach_or_first_paired_sample":
                raise DeploymentValidationError(
                    f"{field_path}.capture must be on_attach_or_first_paired_sample"
                )
            require_same_time = get_alias(
                raw,
                "require_same_time_robot_tcp",
                "requireSameTimeRobotTcp",
                True,
            )
            if not isinstance(require_same_time, bool):
                raise DeploymentValidationError(
                    f"{field_path}.require_same_time_robot_tcp must be a boolean"
                )
            return PoseSourceMeasuredAttachmentBaselineConfig(
                capture=capture,
                require_same_time_robot_tcp=require_same_time,
            )

        def parse_measured_tcp_config(
            raw: Any,
            field_path: str,
            *,
            kind: str,
        ) -> Optional[PoseSourceMeasuredTcpConfig]:
            if raw is None:
                return None
            if not isinstance(raw, dict):
                raise DeploymentValidationError(f"{field_path} must be a mapping")
            entity = self._parse_optional_text(
                get_alias(raw, "entity", "entity", ""),
                f"{field_path}.entity",
            )
            if kind == "simulation_interfaces" and not entity:
                raise DeploymentValidationError(
                    f"{field_path}.entity is required for simulation_interfaces"
                )
            tcp_frame_id = self._parse_optional_text(
                get_alias(raw, "tcp_frame_id", "tcpFrameId", ""),
                f"{field_path}.tcp_frame_id",
            )
            if not tcp_frame_id:
                raise DeploymentValidationError(
                    f"{field_path}.tcp_frame_id must be non-empty"
                )
            frame_id_value = self._parse_optional_text(
                get_alias(raw, "frame_id", "frameId", ""),
                f"{field_path}.frame_id",
            )
            return PoseSourceMeasuredTcpConfig(
                entity=entity,
                tcp_frame_id=tcp_frame_id,
                frame_id=frame_id_value,
            )

        def parse_measured_robot_config(
            raw: Any,
            field_path: str,
        ) -> Optional[PoseSourceMeasuredRobotConfig]:
            if raw is None:
                return None
            if not isinstance(raw, dict):
                raise DeploymentValidationError(f"{field_path} must be a mapping")
            joint_state_topic = self._parse_optional_text(
                get_alias(raw, "joint_state_topic", "jointStateTopic", "/joint_states"),
                f"{field_path}.joint_state_topic",
            )
            if not joint_state_topic:
                raise DeploymentValidationError(
                    f"{field_path}.joint_state_topic must be non-empty"
                )
            return PoseSourceMeasuredRobotConfig(
                joint_state_topic=joint_state_topic,
            )

        def parse_drift_monitor_config(
            raw: Any,
            field_path: str,
            *,
            object_id: str,
        ) -> Optional[PoseSourceAttachedDriftMonitorConfig]:
            if raw is None:
                return None
            if not isinstance(raw, dict):
                raise DeploymentValidationError(f"{field_path} must be a mapping")
            enabled_raw = get_alias(raw, "enabled", "enabled", False)
            if not isinstance(enabled_raw, bool):
                raise DeploymentValidationError(f"{field_path}.enabled must be a boolean")
            translation_raw = get_alias(
                raw,
                "translation_threshold_m",
                "translationThresholdM",
                None,
            )
            translation_threshold_m: Optional[float] = None
            if translation_raw is not None:
                translation_threshold_m = self._parse_positive_float(
                    translation_raw,
                    f"{field_path}.translation_threshold_m",
                )
            rotation_raw = get_alias(
                raw,
                "rotation_threshold_rad",
                "rotationThresholdRad",
                None,
            )
            rotation_threshold_rad: Optional[float] = None
            if rotation_raw is not None:
                rotation_threshold_rad = self._parse_positive_float(
                    rotation_raw,
                    f"{field_path}.rotation_threshold_rad",
                )
            if enabled_raw and translation_threshold_m is None and rotation_threshold_rad is None:
                raise DeploymentValidationError(
                    f"{field_path} enabled monitors require at least one threshold"
                )
            debounce_s = self._parse_positive_float(
                get_alias(raw, "debounce_s", "debounceS", 0.1),
                f"{field_path}.debounce_s",
            )
            stale_behavior = self._parse_optional_text(
                get_alias(raw, "stale_behavior", "staleBehavior", "fault"),
                f"{field_path}.stale_behavior",
            )
            if stale_behavior not in {"fault", "unverifiable", "ignore"}:
                raise DeploymentValidationError(
                    f"{field_path}.stale_behavior must be one of: fault, unverifiable, ignore"
                )
            action = self._parse_optional_text(
                get_alias(raw, "action", "action", "interrupt"),
                f"{field_path}.action",
            )
            if action not in {"interrupt", "diagnostic_only"}:
                raise DeploymentValidationError(
                    f"{field_path}.action must be one of: interrupt, diagnostic_only"
                )
            signal_id = self._parse_optional_text(
                get_alias(
                    raw,
                    "signal_id",
                    "signalId",
                    f"attached_object_drift/{object_id}",
                ),
                f"{field_path}.signal_id",
            )
            if not signal_id:
                raise DeploymentValidationError(f"{field_path}.signal_id must be non-empty")
            sample_time_tolerance_s = self._parse_positive_float(
                get_alias(
                    raw,
                    "sample_time_tolerance_s",
                    "sampleTimeToleranceS",
                    0.05,
                ),
                f"{field_path}.sample_time_tolerance_s",
            )
            return PoseSourceAttachedDriftMonitorConfig(
                enabled=enabled_raw,
                translation_threshold_m=translation_threshold_m,
                rotation_threshold_rad=rotation_threshold_rad,
                debounce_s=debounce_s,
                stale_behavior=stale_behavior,
                action=action,
                signal_id=signal_id,
                sample_time_tolerance_s=sample_time_tolerance_s,
            )

        parsed: dict[str, PoseSourceConfig] = {}
        object_writers: dict[str, str] = {}
        for index, source_data in enumerate(pose_sources_data):
            field_path = f"pose_sources[{index}]"
            if not isinstance(source_data, dict):
                raise DeploymentValidationError(f"{field_path} must be a mapping")

            source_id_raw = source_data.get("id")
            if not isinstance(source_id_raw, str) or not source_id_raw.strip():
                raise DeploymentValidationError(f"{field_path}.id is required")
            source_id = source_id_raw.strip()
            if source_id in parsed:
                raise DeploymentValidationError(
                    f"pose_sources id '{source_id}' is declared more than once"
                )

            kind_raw = source_data.get("kind")
            if not isinstance(kind_raw, str):
                raise DeploymentValidationError(
                    f"{field_path}.kind must be a string"
                )
            kind = kind_raw.strip()
            if kind not in _ALLOWED_POSE_SOURCE_KINDS:
                allowed = ", ".join(sorted(_ALLOWED_POSE_SOURCE_KINDS))
                raise DeploymentValidationError(
                    f"{field_path}.kind must be one of: {allowed}"
                )

            enabled = source_data.get("enabled", True)
            if not isinstance(enabled, bool):
                raise DeploymentValidationError(
                    f"{field_path}.enabled must be a boolean"
                )

            max_update_hz = self._parse_positive_float(
                get_alias(source_data, "max_update_hz", "maxUpdateHz", 4.0),
                f"{field_path}.max_update_hz",
            )
            stale_after_s = self._parse_positive_float(
                get_alias(source_data, "stale_after_s", "staleAfterS", 1.0),
                f"{field_path}.stale_after_s",
            )

            frame_id = self._parse_optional_text(
                get_alias(source_data, "frame_id", "frameId", ""),
                f"{field_path}.frame_id",
            )
            service_prefix = self._parse_optional_text(
                get_alias(source_data, "service_prefix", "servicePrefix", "auto"),
                f"{field_path}.service_prefix",
            )
            topic = self._parse_optional_text(
                source_data.get("topic", ""),
                f"{field_path}.topic",
            )
            description = str(source_data.get("description", ""))
            measured_tcp = parse_measured_tcp_config(
                get_alias(source_data, "measured_tcp", "measuredTcp", None),
                f"{field_path}.measured_tcp",
                kind=kind,
            )
            measured_robot = parse_measured_robot_config(
                get_alias(source_data, "measured_robot", "measuredRobot", None),
                f"{field_path}.measured_robot",
            )

            objects_data = source_data.get("objects", {})
            if not isinstance(objects_data, dict) or not objects_data:
                raise DeploymentValidationError(
                    f"{field_path}.objects must be a non-empty mapping"
                )
            objects: dict[str, PoseSourceObjectBinding] = {}
            for object_id_raw, object_data in objects_data.items():
                if not isinstance(object_id_raw, str) or not object_id_raw.strip():
                    raise DeploymentValidationError(
                        f"{field_path}.objects keys must be non-empty strings"
                    )
                object_id = object_id_raw.strip()
                object_path = f"{field_path}.objects.{object_id}"
                if not isinstance(object_data, dict):
                    raise DeploymentValidationError(f"{object_path} must be a mapping")

                apply_policy = self._parse_optional_text(
                    get_alias(object_data, "apply_policy", "applyPolicy", "observe_only"),
                    f"{object_path}.apply_policy",
                )
                if apply_policy not in _ALLOWED_POSE_SOURCE_APPLY_POLICIES:
                    allowed = ", ".join(sorted(_ALLOWED_POSE_SOURCE_APPLY_POLICIES))
                    raise DeploymentValidationError(
                        f"{object_path}.apply_policy must be one of: {allowed}"
                    )

                previous_writer = object_writers.get(object_id)
                if previous_writer is not None:
                    raise DeploymentValidationError(
                        "pose_sources declare multiple write authorities for "
                        f"object '{object_id}': '{previous_writer}' and '{source_id}'"
                    )
                object_writers[object_id] = source_id

                confidence_raw = get_alias(object_data, "confidence_min", "confidenceMin", None)
                confidence_min: Optional[float] = None
                if confidence_raw is not None:
                    confidence_min = self._parse_bounded_float(
                        confidence_raw,
                        f"{object_path}.confidence_min",
                        minimum=0.0,
                        maximum=1.0,
                    )
                    if kind in {"pose_stamped_topic", "pose_array_topic", "tf"}:
                        raise DeploymentValidationError(
                            f"{object_path}.confidence_min is not supported for {kind}"
                        )

                metadata = object_data.get("metadata", {})
                if metadata is None:
                    metadata = {}
                if not isinstance(metadata, dict):
                    raise DeploymentValidationError(
                        f"{object_path}.metadata must be a mapping"
                    )

                entity = self._parse_optional_text(
                    object_data.get("entity", ""),
                    f"{object_path}.entity",
                )
                if kind in {"simulation_interfaces", "tf"} and not entity:
                    raise DeploymentValidationError(
                        f"{object_path}.entity is required for {kind}"
                    )
                binding_frame_id = self._parse_optional_text(
                    get_alias(object_data, "frame_id", "frameId", ""),
                    f"{object_path}.frame_id",
                )
                measured_state_ghost = parse_ghost_config(
                    get_alias(
                        object_data,
                        "measured_state_ghost",
                        "measuredStateGhost",
                        None,
                    ),
                    f"{object_path}.measured_state_ghost",
                )
                measured_attachment_baseline = parse_baseline_config(
                    get_alias(
                        object_data,
                        "measured_attachment_baseline",
                        "measuredAttachmentBaseline",
                        None,
                    ),
                    f"{object_path}.measured_attachment_baseline",
                )
                attached_drift_monitor = parse_drift_monitor_config(
                    get_alias(
                        object_data,
                        "attached_drift_monitor",
                        "attachedDriftMonitor",
                        None,
                    ),
                    f"{object_path}.attached_drift_monitor",
                    object_id=object_id,
                )

                objects[object_id] = PoseSourceObjectBinding(
                    object_id=object_id,
                    apply_policy=apply_policy,
                    entity=entity,
                    frame_id=binding_frame_id,
                    confidence_min=confidence_min,
                    measured_state_ghost=measured_state_ghost,
                    measured_attachment_baseline=measured_attachment_baseline,
                    attached_drift_monitor=attached_drift_monitor,
                    metadata=dict(metadata),
                )

            if kind == "simulation_interfaces" and not service_prefix:
                raise DeploymentValidationError(
                    f"{field_path}.service_prefix must be non-empty"
                )
            if kind in {
                "pose_stamped_topic",
                "pose_array_topic",
                "detection3d_array_topic",
            } and not topic:
                raise DeploymentValidationError(f"{field_path}.topic is required")
            if kind in {
                "pose_stamped_topic",
                "pose_array_topic",
                "detection3d_array_topic",
                "tf",
            } and measured_tcp is not None:
                raise DeploymentValidationError(
                    f"{field_path}.measured_tcp is not supported for {kind}"
                )
            if kind == "pose_stamped_topic" and len(objects) != 1:
                raise DeploymentValidationError(
                    f"{field_path}.objects must contain exactly one object for pose_stamped_topic"
                )
            if kind == "pose_array_topic":
                seen_pose_indexes: dict[int, str] = {}
                for object_id, binding in objects.items():
                    pose_index = binding.metadata.get("pose_index")
                    if not isinstance(pose_index, int) or isinstance(pose_index, bool) or pose_index < 0:
                        raise DeploymentValidationError(
                            f"{field_path}.objects.{object_id}.metadata.pose_index must be a non-negative integer for pose_array_topic"
                        )
                    previous = seen_pose_indexes.get(pose_index)
                    if previous is not None:
                        raise DeploymentValidationError(
                            f"{field_path}.objects.{object_id}.metadata.pose_index duplicates object '{previous}'"
                        )
                    seen_pose_indexes[pose_index] = object_id
            if kind == "detection3d_array_topic":
                seen_detection_associations: dict[tuple[str, str], str] = {}
                for object_id, binding in objects.items():
                    detection_id = binding.metadata.get("detection_id")
                    class_id = binding.metadata.get("class_id")
                    has_detection_id = (
                        isinstance(detection_id, str) and bool(detection_id.strip())
                    )
                    has_class_id = isinstance(class_id, str) and bool(class_id.strip())
                    if has_detection_id == has_class_id:
                        raise DeploymentValidationError(
                            f"{field_path}.objects.{object_id}.metadata must define exactly one of detection_id or class_id for detection3d_array_topic"
                        )
                    association = (
                        "detection_id",
                        detection_id.strip()
                        if isinstance(detection_id, str)
                        else class_id.strip(),
                    )
                    if has_class_id:
                        association = ("class_id", class_id.strip())
                    previous = seen_detection_associations.get(association)
                    if previous is not None:
                        association_key, association_value = association
                        raise DeploymentValidationError(
                            f"{field_path}.objects.{object_id}.metadata.{association_key} "
                            f"duplicates object '{previous}' for value '{association_value}'"
                        )
                    seen_detection_associations[association] = object_id
                    max_position_variance = binding.metadata.get(
                        "max_position_variance_m2"
                    )
                    if max_position_variance is not None and (
                        isinstance(max_position_variance, bool)
                        or not isinstance(max_position_variance, (int, float))
                        or not math.isfinite(float(max_position_variance))
                        or float(max_position_variance) <= 0.0
                    ):
                        raise DeploymentValidationError(
                            f"{field_path}.objects.{object_id}.metadata.max_position_variance_m2 must be a positive finite number"
                        )
                    if binding.apply_policy != "observe_only":
                        if binding.confidence_min is None:
                            raise DeploymentValidationError(
                                f"{field_path}.objects.{object_id}.confidence_min is required for execution-affecting detection3d_array_topic policies"
                            )
                        if max_position_variance is None:
                            raise DeploymentValidationError(
                                f"{field_path}.objects.{object_id}.metadata.max_position_variance_m2 is required for execution-affecting detection3d_array_topic policies"
                            )
            if kind == "tf":
                for object_id, binding in objects.items():
                    if not (binding.frame_id or frame_id):
                        raise DeploymentValidationError(
                            f"{field_path}.objects.{object_id}.frame_id or {field_path}.frame_id is required for tf"
                        )

            parsed[source_id] = PoseSourceConfig(
                source_id=source_id,
                kind=kind,
                enabled=enabled,
                max_update_hz=max_update_hz,
                stale_after_s=stale_after_s,
                frame_id=frame_id,
                service_prefix=service_prefix or "auto",
                topic=topic,
                description=description,
                measured_tcp=measured_tcp,
                measured_robot=measured_robot,
                objects=objects,
            )
        return parsed

    def _parse_simulation_control(
        self,
        simulation_control_data: dict[str, Any] | None,
    ) -> Optional[SimulationControlConfig]:
        if simulation_control_data is None:
            return None
        if not isinstance(simulation_control_data, dict):
            raise DeploymentValidationError("simulation_control must be a mapping")

        def get_alias(data: dict[str, Any], snake: str, camel: str, default: Any) -> Any:
            if snake in data:
                return data[snake]
            if camel in data:
                return data[camel]
            return default

        if any(
            not isinstance(field, str) or not field.strip()
            for field in simulation_control_data
        ):
            raise DeploymentValidationError(
                "simulation_control keys must be non-empty strings"
            )
        unknown_top_fields = sorted(
            field
            for field in simulation_control_data
            if field
            not in {
                "kind",
                "service_prefix",
                "servicePrefix",
                "capabilities",
                "reset",
            }
        )
        if unknown_top_fields:
            raise DeploymentValidationError(
                "simulation_control contains unknown fields: "
                f"{unknown_top_fields}"
            )

        kind_raw = simulation_control_data.get("kind")
        if not isinstance(kind_raw, str):
            raise DeploymentValidationError("simulation_control.kind must be a string")
        kind = kind_raw.strip()
        if kind not in _ALLOWED_SIMULATION_CONTROL_KINDS:
            allowed = ", ".join(sorted(_ALLOWED_SIMULATION_CONTROL_KINDS))
            raise DeploymentValidationError(
                f"simulation_control.kind must be one of: {allowed}"
            )

        service_prefix = self._parse_optional_text(
            get_alias(
                simulation_control_data,
                "service_prefix",
                "servicePrefix",
                "auto",
            ),
            "simulation_control.service_prefix",
        )
        if not service_prefix:
            raise DeploymentValidationError(
                "simulation_control.service_prefix must be non-empty"
            )

        capabilities_input = simulation_control_data.get("capabilities", {})
        if capabilities_input is None:
            capabilities_input = {}
        if not isinstance(capabilities_input, dict):
            raise DeploymentValidationError(
                "simulation_control.capabilities must be a mapping"
            )
        capabilities_raw: dict[str, Any] = {}
        for raw_flag, value in capabilities_input.items():
            if not isinstance(raw_flag, str) or not raw_flag.strip():
                raise DeploymentValidationError(
                    "simulation_control.capabilities keys must be non-empty strings"
                )
            flag = "set_entity_state" if raw_flag == "setEntityState" else raw_flag
            if flag in capabilities_raw:
                raise DeploymentValidationError(
                    "simulation_control.capabilities declares duplicate aliases "
                    f"for '{flag}'"
                )
            capabilities_raw[flag] = value
        unknown_flags = sorted(
            flag
            for flag in capabilities_raw
            if flag not in _SIMULATION_CONTROL_CAPABILITY_FLAGS
        )
        if unknown_flags:
            allowed = ", ".join(sorted(_SIMULATION_CONTROL_CAPABILITY_FLAGS))
            raise DeploymentValidationError(
                "simulation_control.capabilities contains unknown flags "
                f"{unknown_flags}; allowed flags: {allowed}"
            )

        def parse_capability_flag(name: str) -> bool:
            value = capabilities_raw.get(name, False)
            if not isinstance(value, bool):
                raise DeploymentValidationError(
                    f"simulation_control.capabilities.{name} must be a boolean"
                )
            return value

        capabilities = SimulationControlCapabilities(
            reset=parse_capability_flag("reset"),
            pause=parse_capability_flag("pause"),
            play=parse_capability_flag("play"),
            step=parse_capability_flag("step"),
            set_entity_state=parse_capability_flag("set_entity_state"),
        )
        if not any(
            (
                capabilities.reset,
                capabilities.pause,
                capabilities.play,
                capabilities.step,
                capabilities.set_entity_state,
            )
        ):
            raise DeploymentValidationError(
                "simulation_control.capabilities must declare at least one supported operation"
            )

        reset_present = "reset" in simulation_control_data
        reset_raw = simulation_control_data.get("reset", {})
        if reset_raw is None:
            reset_raw = {}
        if not isinstance(reset_raw, dict):
            raise DeploymentValidationError("simulation_control.reset must be a mapping")
        if reset_present and not capabilities.reset:
            raise DeploymentValidationError(
                "simulation_control.reset may be configured only when "
                "simulation_control.capabilities.reset is true"
            )
        if any(not isinstance(field, str) or not field.strip() for field in reset_raw):
            raise DeploymentValidationError(
                "simulation_control.reset keys must be non-empty strings"
            )
        unknown_reset_fields = sorted(
            field
            for field in reset_raw
            if field
            not in {
                "default_scope",
                "defaultScope",
                "close_command_gate",
                "closeCommandGate",
                "wait_ready_s",
                "waitReadyS",
                "resume_play_after_reset",
                "resumePlayAfterReset",
            }
        )
        if unknown_reset_fields:
            raise DeploymentValidationError(
                "simulation_control.reset contains unknown fields: "
                f"{unknown_reset_fields}"
            )

        default_scope = self._parse_optional_text(
            get_alias(reset_raw, "default_scope", "defaultScope", "state"),
            "simulation_control.reset.default_scope",
        )
        if default_scope not in _ALLOWED_SIMULATION_RESET_SCOPES:
            allowed = ", ".join(sorted(_ALLOWED_SIMULATION_RESET_SCOPES))
            raise DeploymentValidationError(
                f"simulation_control.reset.default_scope must be one of: {allowed}"
            )
        close_command_gate = get_alias(
            reset_raw,
            "close_command_gate",
            "closeCommandGate",
            True,
        )
        if not isinstance(close_command_gate, bool):
            raise DeploymentValidationError(
                "simulation_control.reset.close_command_gate must be a boolean"
            )
        resume_play_after_reset = get_alias(
            reset_raw,
            "resume_play_after_reset",
            "resumePlayAfterReset",
            True,
        )
        if not isinstance(resume_play_after_reset, bool):
            raise DeploymentValidationError(
                "simulation_control.reset.resume_play_after_reset must be a boolean"
            )
        reset_config = SimulationResetConfig(
            default_scope=default_scope,
            close_command_gate=close_command_gate,
            wait_ready_s=self._parse_positive_float(
                get_alias(reset_raw, "wait_ready_s", "waitReadyS", 3.0),
                "simulation_control.reset.wait_ready_s",
            ),
            resume_play_after_reset=resume_play_after_reset,
        )

        return SimulationControlConfig(
            kind=kind,
            service_prefix=service_prefix,
            capabilities=capabilities,
            reset=reset_config,
        )

    def _parse_contact_policies(
        self,
        contact_policies_data: Any,
    ) -> list[ContactPolicyConfig]:
        if contact_policies_data is None:
            return []
        if not isinstance(contact_policies_data, list):
            raise DeploymentValidationError("contact_policies must be a list")
        parsed: list[ContactPolicyConfig] = []
        seen: set[str] = set()
        for index, raw_policy in enumerate(contact_policies_data):
            path = f"contact_policies[{index}]"
            if not isinstance(raw_policy, dict):
                raise DeploymentValidationError(f"{path} must be a mapping")
            policy_id = raw_policy.get("policy_id")
            if not isinstance(policy_id, str) or not policy_id.strip():
                raise DeploymentValidationError(f"{path}.policy_id is required")
            policy_id = policy_id.strip()
            if policy_id in seen:
                raise DeploymentValidationError(
                    f"contact_policies contains duplicate policy_id '{policy_id}'"
                )
            seen.add(policy_id)
            parsed.append(ContactPolicyConfig(policy_id=policy_id, raw=dict(raw_policy)))
        return parsed

    def _parse_grippers(
        self,
        grippers_data: dict[str, Any] | None,
    ) -> dict[str, GripperConfig]:
        if grippers_data and not isinstance(grippers_data, dict):
            raise DeploymentValidationError("grippers must be a mapping")
        parsed: dict[str, GripperConfig] = {}
        srdf_group_states = self._load_srdf_group_state_positions()
        for gripper_id, gripper_data in (grippers_data or {}).items():
            if not isinstance(gripper_data, dict):
                raise DeploymentValidationError(f"grippers.{gripper_id} must be a mapping")

            command_mode = str(gripper_data.get("command_mode", "")).strip()
            group_id = str(gripper_data.get("group_id", "")).strip()
            joint_name = str(gripper_data.get("joint_name", "")).strip()

            if command_mode not in _ALLOWED_GRIPPER_COMMAND_MODES:
                allowed = ", ".join(sorted(_ALLOWED_GRIPPER_COMMAND_MODES))
                raise DeploymentValidationError(
                    f"grippers.{gripper_id}.command_mode must be one of: {allowed}"
                )
            if not group_id:
                raise DeploymentValidationError(f"grippers.{gripper_id}.group_id is required")
            if not joint_name:
                raise DeploymentValidationError(f"grippers.{gripper_id}.joint_name is required")

            if "open_position" in gripper_data:
                raise DeploymentValidationError(
                    f"grippers.{gripper_id}.open_position is no longer supported; "
                    "define an SRDF group_state preset instead"
                )
            if "closed_position" in gripper_data:
                raise DeploymentValidationError(
                    f"grippers.{gripper_id}.closed_position is no longer supported; "
                    "define an SRDF group_state preset instead"
                )
            try:
                motion_time_s = float(gripper_data.get("motion_time_s", 0.0))
            except (TypeError, ValueError) as exc:
                raise DeploymentValidationError(
                    f"grippers.{gripper_id}.motion_time_s must be a number"
                ) from exc
            if motion_time_s <= 0.0:
                raise DeploymentValidationError(
                    f"grippers.{gripper_id}.motion_time_s must be > 0"
                )
            srdf_presets = dict(srdf_group_states.get((group_id, joint_name), {}))
            touch_links_data = gripper_data.get("touch_links", [])
            if not isinstance(touch_links_data, list):
                raise DeploymentValidationError(
                    f"grippers.{gripper_id}.touch_links must be a list of strings"
                )
            touch_links: list[str] = []
            for index, link_name in enumerate(touch_links_data):
                link_name_str = str(link_name).strip()
                if not link_name_str:
                    raise DeploymentValidationError(
                        f"grippers.{gripper_id}.touch_links[{index}] must be a non-empty string"
                    )
                touch_links.append(link_name_str)

            mount_frame = self._parse_optional_frame_name(
                gripper_data.get("mount_frame"),
                f"grippers.{gripper_id}.mount_frame",
            )
            attach_frame = self._parse_optional_frame_name(
                gripper_data.get("attach_frame"),
                f"grippers.{gripper_id}.attach_frame",
            )
            tcp_frame = self._parse_optional_frame_name(
                gripper_data.get("tcp_frame"),
                f"grippers.{gripper_id}.tcp_frame",
            )
            auxiliary_frames = self._parse_frame_name_list(
                gripper_data.get("auxiliary_frames", []),
                f"grippers.{gripper_id}.auxiliary_frames",
            )

            parsed[gripper_id] = GripperConfig(
                command_mode=command_mode,
                group_id=group_id,
                joint_name=joint_name,
                motion_time_s=motion_time_s,
                srdf_presets=srdf_presets,
                description=str(gripper_data.get("description", "")),
                touch_links=touch_links,
                mount_frame=mount_frame,
                attach_frame=attach_frame,
                tcp_frame=tcp_frame,
                auxiliary_frames=auxiliary_frames,
            )
        return parsed

    def _load_srdf_group_state_positions(
        self,
    ) -> dict[tuple[str, str], dict[str, float]]:
        """Return SRDF group-state positions keyed by (group, joint).

        Only explicit SRDF group states are treated as semantic presets. If the
        SRDF cannot be resolved during early deployment editing, we return no
        presets and keep the raw deployment values as ordinary metadata.
        """
        robot_config = self.robot_config
        if robot_config is None or not robot_config.srdf_path:
            return {}
        try:
            search_roots = normalize_package_paths(
                robot_config.package_paths,
                base_dir=self._path.resolve().parent,
            )
            srdf_path = resolve_resource_path(
                robot_config.srdf_path,
                base_dir=self._path.resolve().parent,
                package_paths=search_roots,
            )
            root = ET.parse(srdf_path).getroot()
        except Exception:
            return {}

        states: dict[tuple[str, str], dict[str, float]] = {}
        for group_state in root.findall("group_state"):
            state_name = str(group_state.get("name") or "").strip()
            group_name = str(group_state.get("group") or "").strip()
            if not state_name or not group_name:
                continue
            normalized_state = state_name.lower()
            for joint in group_state.findall("joint"):
                joint_name = str(joint.get("name") or "").strip()
                raw_value = joint.get("value")
                if not joint_name or raw_value is None:
                    continue
                try:
                    value = float(raw_value)
                except ValueError:
                    continue
                states.setdefault((group_name, joint_name), {})[
                    normalized_state
                ] = value
        return states

    def _parse_optional_frame_name(
        self,
        value: Any,
        field_path: str,
    ) -> str:
        if value is None:
            return ""
        if not isinstance(value, str):
            raise DeploymentValidationError(f"{field_path} must be a string")
        return value.strip()

    def _parse_frame_name_list(
        self,
        values: Any,
        field_path: str,
    ) -> list[str]:
        if values is None:
            return []
        if not isinstance(values, list):
            raise DeploymentValidationError(f"{field_path} must be a list of strings")
        parsed: list[str] = []
        for index, frame_name in enumerate(values):
            frame_name_str = self._parse_optional_frame_name(
                frame_name,
                f"{field_path}[{index}]",
            )
            if not frame_name_str:
                raise DeploymentValidationError(
                    f"{field_path}[{index}] must be a non-empty string"
                )
            parsed.append(frame_name_str)
        return parsed

    def _parse_required_string(self, value: Any, field_path: str) -> str:
        if not isinstance(value, str) or not value.strip():
            raise DeploymentValidationError(f"{field_path} must be a non-empty string")
        return value.strip()

    def _parse_string_list(self, values: Any, field_path: str) -> list[str]:
        if values is None:
            return []
        if not isinstance(values, list):
            raise DeploymentValidationError(f"{field_path} must be a list of strings")
        parsed: list[str] = []
        for index, value in enumerate(values):
            if not isinstance(value, str) or not value.strip():
                raise DeploymentValidationError(
                    f"{field_path}[{index}] must be a non-empty string"
                )
            parsed.append(value.strip())
        duplicates = sorted({item for item in parsed if parsed.count(item) > 1})
        if duplicates:
            raise DeploymentValidationError(
                f"{field_path} contains duplicate entries: {duplicates}"
            )
        return parsed

    def _parse_optional_string(self, value: Any, field_path: str) -> Optional[str]:
        if value is None:
            return None
        if not isinstance(value, str):
            raise DeploymentValidationError(f"{field_path} must be a string")
        return value.strip()

    def _parse_optional_text(self, value: Any, field_path: str) -> str:
        if value is None:
            return ""
        if not isinstance(value, str):
            raise DeploymentValidationError(f"{field_path} must be a string")
        return value.strip()

    def _parse_positive_float(self, value: Any, field_path: str) -> float:
        if isinstance(value, bool):
            raise DeploymentValidationError(f"{field_path} must be a number > 0")
        try:
            parsed = float(value)
        except (TypeError, ValueError) as exc:
            raise DeploymentValidationError(f"{field_path} must be a number > 0") from exc
        if not math.isfinite(parsed) or parsed <= 0.0:
            raise DeploymentValidationError(f"{field_path} must be a number > 0")
        return parsed

    def _parse_bounded_float(
        self,
        value: Any,
        field_path: str,
        *,
        minimum: float,
        maximum: float,
    ) -> float:
        if isinstance(value, bool):
            raise DeploymentValidationError(
                f"{field_path} must be a number between {minimum} and {maximum}"
            )
        try:
            parsed = float(value)
        except (TypeError, ValueError) as exc:
            raise DeploymentValidationError(
                f"{field_path} must be a number between {minimum} and {maximum}"
            ) from exc
        if not math.isfinite(parsed) or parsed < minimum or parsed > maximum:
            raise DeploymentValidationError(
                f"{field_path} must be a number between {minimum} and {maximum}"
            )
        return parsed

    def _parse_positive_int(self, value: Any, field_path: str) -> int:
        if not isinstance(value, int) or isinstance(value, bool):
            raise DeploymentValidationError(f"{field_path} must be an integer > 0")
        if value <= 0:
            raise DeploymentValidationError(f"{field_path} must be an integer > 0")
        return value

    def _parse_model_endpoint_capabilities(
        self,
        raw_capabilities: Any,
        field_path: str,
    ) -> ModelEndpointCapabilities:
        if raw_capabilities is None:
            return ModelEndpointCapabilities()
        if not isinstance(raw_capabilities, dict):
            raise DeploymentValidationError(f"{field_path} must be a mapping")

        bool_fields = {
            "supports_response_tools": False,
            "supports_json_object": False,
            "supports_image_input": False,
        }
        parsed_bool_fields: dict[str, bool] = {}
        for key, default in bool_fields.items():
            value = raw_capabilities.get(key, default)
            if not isinstance(value, bool):
                raise DeploymentValidationError(f"{field_path}.{key} must be a boolean")
            parsed_bool_fields[key] = value

        image_transports = raw_capabilities.get("supported_image_transports", [])
        if image_transports is None:
            image_transports = []
        if not isinstance(image_transports, list):
            raise DeploymentValidationError(
                f"{field_path}.supported_image_transports must be a list of strings"
            )
        parsed_transports: list[str] = []
        for index, transport in enumerate(image_transports):
            if not isinstance(transport, str) or not transport.strip():
                raise DeploymentValidationError(
                    f"{field_path}.supported_image_transports[{index}] must be a non-empty string"
                )
            normalized = transport.strip()
            if normalized not in _ALLOWED_IMAGE_TRANSPORTS:
                allowed = ", ".join(sorted(_ALLOWED_IMAGE_TRANSPORTS))
                raise DeploymentValidationError(
                    f"{field_path}.supported_image_transports[{index}] must be one of: {allowed}"
                )
            parsed_transports.append(normalized)

        latency_budget_ms = raw_capabilities.get("latency_budget_ms")
        if latency_budget_ms is not None:
            latency_budget_ms = self._parse_positive_int(
                latency_budget_ms,
                f"{field_path}.latency_budget_ms",
            )

        return ModelEndpointCapabilities(
            supports_response_tools=parsed_bool_fields["supports_response_tools"],
            supports_json_object=parsed_bool_fields["supports_json_object"],
            supports_image_input=parsed_bool_fields["supports_image_input"],
            supported_image_transports=parsed_transports,
            tool_call_family=self._parse_optional_string(
                raw_capabilities.get("tool_call_family", ""),
                f"{field_path}.tool_call_family",
            ) or "",
            latency_budget_ms=latency_budget_ms,
        )

    def _model_endpoint_to_raw(self, endpoint: ModelEndpoint) -> dict[str, Any]:
        return {
            "provider": endpoint.provider,
            "model_id": endpoint.model_id,
            "base_url": endpoint.base_url,
            "image_fetch_base_url": endpoint.image_fetch_base_url,
            "api_key_env": endpoint.api_key_env,
            "default_timeout_s": endpoint.default_timeout_s,
            "max_tokens": endpoint.max_tokens,
            "capabilities": {
                "supports_response_tools": (
                    endpoint.capabilities.supports_response_tools
                ),
                "supports_json_object": endpoint.capabilities.supports_json_object,
                "supports_image_input": endpoint.capabilities.supports_image_input,
                "supported_image_transports": list(
                    endpoint.capabilities.supported_image_transports
                ),
                "tool_call_family": endpoint.capabilities.tool_call_family,
                **(
                    {"latency_budget_ms": endpoint.capabilities.latency_budget_ms}
                    if endpoint.capabilities.latency_budget_ms is not None
                    else {}
                ),
            },
        }

    def _merge_model_endpoint_profile(
        self,
        base: dict[str, Any],
        override: dict[str, Any],
    ) -> dict[str, Any]:
        merged = copy.deepcopy(base)
        for key, value in override.items():
            if key == "profile_ref":
                continue
            if (
                key == "capabilities"
                and isinstance(merged.get("capabilities"), dict)
                and isinstance(value, dict)
            ):
                capabilities = copy.deepcopy(merged["capabilities"])
                capabilities.update(copy.deepcopy(value))
                merged["capabilities"] = capabilities
            else:
                merged[key] = copy.deepcopy(value)
        return merged

    def _parse_model_endpoint_entry(
        self,
        alias: str,
        endpoint_data: Any,
        field_path: str,
        *,
        profile_registry: dict[str, dict[str, Any]] | None = None,
        allow_profile_ref: bool = True,
    ) -> ModelEndpoint:
        if not isinstance(endpoint_data, dict):
            raise DeploymentValidationError(f"{field_path} must be a mapping")

        profile_ref = ""
        resolved_data = endpoint_data
        if allow_profile_ref:
            profile_ref = self._parse_optional_string(
                endpoint_data.get("profile_ref", ""),
                f"{field_path}.profile_ref",
            ) or ""
            if profile_ref:
                if profile_registry is None or profile_ref not in profile_registry:
                    raise DeploymentValidationError(
                        f"{field_path}.profile_ref references unknown model endpoint profile '{profile_ref}'"
                    )
                resolved_data = self._merge_model_endpoint_profile(
                    profile_registry[profile_ref],
                    endpoint_data,
                )
        elif "profile_ref" in endpoint_data:
            raise DeploymentValidationError(
                f"{field_path}.profile_ref is not allowed in model endpoint profile entries"
            )

        for req_field in ("provider", "model_id"):
            if req_field not in resolved_data:
                raise DeploymentValidationError(
                    f"{field_path}.{req_field} is required"
                )
        return ModelEndpoint(
            alias=alias,
            profile_ref=profile_ref,
            provider=self._parse_required_string(
                resolved_data["provider"],
                f"{field_path}.provider",
            ),
            model_id=self._parse_required_string(
                resolved_data["model_id"],
                f"{field_path}.model_id",
            ),
            base_url=self._parse_optional_string(
                resolved_data.get("base_url"),
                f"{field_path}.base_url",
            ),
            image_fetch_base_url=self._parse_optional_string(
                resolved_data.get("image_fetch_base_url"),
                f"{field_path}.image_fetch_base_url",
            ),
            api_key_env=self._parse_optional_string(
                resolved_data.get("api_key_env"),
                f"{field_path}.api_key_env",
            ),
            default_timeout_s=self._parse_positive_float(
                resolved_data.get("default_timeout_s", 30.0),
                f"{field_path}.default_timeout_s",
            ),
            max_tokens=self._parse_positive_int(
                resolved_data.get("max_tokens", 4096),
                f"{field_path}.max_tokens",
            ),
            capabilities=self._parse_model_endpoint_capabilities(
                resolved_data.get("capabilities"),
                f"{field_path}.capabilities",
            ),
        )

    def _load_model_endpoint_profile_registry(
        self,
        registry_path_data: Any,
    ) -> dict[str, dict[str, Any]]:
        self.model_endpoint_profile_registry = self._parse_optional_string(
            registry_path_data,
            "model_endpoint_profile_registry",
        )
        if not self.model_endpoint_profile_registry:
            return {}

        registry_path = Path(self.model_endpoint_profile_registry)
        if not registry_path.is_absolute() and str(self._path) != "<memory>":
            registry_path = self._path.parent / registry_path

        try:
            with open(registry_path, encoding="utf-8") as f:
                registry_data = yaml.safe_load(f)
        except OSError as exc:
            raise DeploymentValidationError(
                f"model_endpoint_profile_registry could not be read: {registry_path}"
            ) from exc

        if not isinstance(registry_data, dict):
            raise DeploymentValidationError(
                "model_endpoint_profile_registry file must be a YAML mapping"
            )
        api_version = registry_data.get("api_version")
        if api_version != EXPECTED_MODEL_ENDPOINT_PROFILE_REGISTRY_API_VERSION:
            raise DeploymentValidationError(
                "model_endpoint_profile_registry.api_version must be "
                f"'{EXPECTED_MODEL_ENDPOINT_PROFILE_REGISTRY_API_VERSION}'"
            )
        schema_version = registry_data.get("schema_version", "")
        if not isinstance(schema_version, str) or not SEMVER_RE.match(schema_version):
            raise DeploymentValidationError(
                "model_endpoint_profile_registry.schema_version must match X.Y.Z"
            )
        profiles_data = registry_data.get("profiles")
        if not isinstance(profiles_data, dict):
            raise DeploymentValidationError(
                "model_endpoint_profile_registry.profiles must be a mapping"
            )

        profiles: dict[str, dict[str, Any]] = {}
        for profile_id, profile_data in profiles_data.items():
            if not isinstance(profile_id, str) or not profile_id.strip():
                raise DeploymentValidationError(
                    "model_endpoint_profile_registry.profiles keys must be non-empty strings"
                )
            profile_id = profile_id.strip()
            parsed = self._parse_model_endpoint_entry(
                profile_id,
                profile_data,
                f"model_endpoint_profile_registry.profiles.{profile_id}",
                allow_profile_ref=False,
            )
            profiles[profile_id] = self._model_endpoint_to_raw(parsed)
        return profiles

    def _parse_model_endpoints(
        self,
        endpoints_data: Any,
        profile_registry: dict[str, dict[str, Any]] | None = None,
    ) -> dict[str, ModelEndpoint]:
        if endpoints_data is None:
            return {}
        if not isinstance(endpoints_data, dict):
            raise DeploymentValidationError("model_endpoints must be a mapping")

        parsed: dict[str, ModelEndpoint] = {}
        for alias, endpoint_data in endpoints_data.items():
            if not isinstance(alias, str) or not alias.strip():
                raise DeploymentValidationError(
                    "model_endpoints keys must be non-empty strings"
                )
            alias = alias.strip()
            parsed[alias] = self._parse_model_endpoint_entry(
                alias,
                endpoint_data,
                f"model_endpoints.{alias}",
                profile_registry=profile_registry or {},
            )
        return parsed

    def _parse_assistant_config(
        self,
        assistant_data: Any,
        model_endpoints: dict[str, ModelEndpoint],
    ) -> Optional[AssistantConfig]:
        if assistant_data is None:
            return None
        if assistant_data == {}:
            return None
        if not isinstance(assistant_data, dict):
            raise DeploymentValidationError("assistant must be a mapping")

        provider = self._parse_required_string(
            assistant_data.get("provider"),
            "assistant.provider",
        )
        if provider == "deterministic":
            provider = "deterministic-scene-authoring"
        if provider not in _ALLOWED_ASSISTANT_PROVIDERS:
            allowed = ", ".join(sorted(_ALLOWED_ASSISTANT_PROVIDERS))
            raise DeploymentValidationError(
                f"assistant.provider must be one of: {allowed}"
            )

        enabled = assistant_data.get("enabled", True)
        if not isinstance(enabled, bool):
            raise DeploymentValidationError("assistant.enabled must be a boolean")

        endpoint_alias = self._parse_optional_string(
            assistant_data.get("endpoint_alias", ""),
            "assistant.endpoint_alias",
        ) or ""
        if enabled and provider in _ASSISTANT_PROVIDERS_REQUIRING_ENDPOINT:
            if not endpoint_alias:
                raise DeploymentValidationError(
                    f"assistant.endpoint_alias is required for provider '{provider}'"
                )
            endpoint = model_endpoints.get(endpoint_alias)
            if endpoint is None:
                raise DeploymentValidationError(
                    f"assistant.endpoint_alias references unknown model endpoint '{endpoint_alias}'"
                )
            if not endpoint.base_url:
                raise DeploymentValidationError(
                    f"assistant.endpoint_alias '{endpoint_alias}' must reference a model endpoint with base_url"
                )

        return AssistantConfig(
            provider=provider,
            endpoint_alias=endpoint_alias,
            enabled=enabled,
        )

    def _parse_tool_catalog(
        self,
        tool_catalog_data: Any,
        *,
        assistant_modes_present: bool,
    ) -> dict[str, AssistantToolDefinition]:
        if tool_catalog_data is None:
            if assistant_modes_present:
                raise DeploymentValidationError(
                    "tool_catalog is required when assistant_modes is non-empty"
                )
            return {}
        if not isinstance(tool_catalog_data, list):
            raise DeploymentValidationError("tool_catalog must be a list")

        parsed: dict[str, AssistantToolDefinition] = {}
        for index, raw_tool in enumerate(tool_catalog_data):
            field_path = f"tool_catalog[{index}]"
            if not isinstance(raw_tool, dict):
                raise DeploymentValidationError(f"{field_path} must be a mapping")
            tool_id = self._parse_required_string(
                raw_tool.get("id"),
                f"{field_path}.id",
            )
            if tool_id in parsed:
                raise DeploymentValidationError(
                    f"tool_catalog contains duplicate tool id '{tool_id}'"
                )
            effect = self._parse_required_string(
                raw_tool.get("effect"),
                f"{field_path}.effect",
            )
            if effect not in _ALLOWED_ASSISTANT_TOOL_EFFECTS:
                allowed = ", ".join(sorted(_ALLOWED_ASSISTANT_TOOL_EFFECTS))
                raise DeploymentValidationError(
                    f"{field_path}.effect must be one of: {allowed}"
                )
            description = self._parse_optional_string(
                raw_tool.get("description", ""),
                f"{field_path}.description",
            ) or ""
            extra = {
                key: copy.deepcopy(value)
                for key, value in raw_tool.items()
                if key not in {"id", "effect", "description"}
            }
            for schema_key in ("input_schema", "inputSchema", "parameters"):
                if schema_key in extra:
                    self._validate_assistant_tool_input_schema(
                        extra[schema_key],
                        f"{field_path}.{schema_key}",
                    )
            parsed[tool_id] = AssistantToolDefinition(
                tool_id=tool_id,
                effect=effect,
                description=description,
                extra=extra,
            )
        return parsed

    def _validate_assistant_tool_input_schema(
        self,
        input_schema: Any,
        field_path: str,
    ) -> None:
        if not isinstance(input_schema, dict):
            raise DeploymentValidationError(f"{field_path} must be a mapping")
        schema_type = input_schema.get("type", "object")
        if schema_type != "object":
            raise DeploymentValidationError(f"{field_path}.type must be 'object'")
        properties = input_schema.get("properties", {})
        if not isinstance(properties, dict):
            raise DeploymentValidationError(f"{field_path}.properties must be a mapping")
        required = input_schema.get("required", [])
        if not isinstance(required, list) or not all(
            isinstance(item, str) for item in required
        ):
            raise DeploymentValidationError(
                f"{field_path}.required must be a list of strings"
            )

    def _assistant_tool_to_raw(self, tool: AssistantToolDefinition) -> dict[str, Any]:
        raw = {
            "id": tool.tool_id,
            "effect": tool.effect,
        }
        if tool.description:
            raw["description"] = tool.description
        raw.update(copy.deepcopy(tool.extra))
        return raw

    def _parse_assistant_mode_authority(
        self,
        authority_data: Any,
        field_path: str,
    ) -> AssistantModeAuthority:
        if not isinstance(authority_data, dict):
            raise DeploymentValidationError(f"{field_path} must be a mapping")

        session_kind = self._parse_required_string(
            authority_data.get("session_kind", authority_data.get("sessionKind")),
            f"{field_path}.session_kind",
        )
        if session_kind not in _ALLOWED_ASSISTANT_SESSION_KINDS:
            allowed = ", ".join(sorted(_ALLOWED_ASSISTANT_SESSION_KINDS))
            raise DeploymentValidationError(
                f"{field_path}.session_kind must be one of: {allowed}"
            )

        approval_mode = self._parse_required_string(
            authority_data.get("approval_mode", authority_data.get("approvalMode")),
            f"{field_path}.approval_mode",
        )
        if approval_mode not in _ALLOWED_ASSISTANT_APPROVAL_MODES:
            allowed = ", ".join(sorted(_ALLOWED_ASSISTANT_APPROVAL_MODES))
            raise DeploymentValidationError(
                f"{field_path}.approval_mode must be one of: {allowed}"
            )

        review = self._parse_required_string(
            authority_data.get("review"),
            f"{field_path}.review",
        )
        if review not in _ALLOWED_ASSISTANT_REVIEW_POLICIES:
            allowed = ", ".join(sorted(_ALLOWED_ASSISTANT_REVIEW_POLICIES))
            raise DeploymentValidationError(
                f"{field_path}.review must be one of: {allowed}"
            )
        if review == "bounded_by_recovery_profile" and session_kind != "delegated_recovery":
            raise DeploymentValidationError(
                f"{field_path}.review='bounded_by_recovery_profile' requires "
                "session_kind='delegated_recovery'"
            )
        if approval_mode == "policy_granted" and session_kind != "delegated_recovery":
            raise DeploymentValidationError(
                f"{field_path}.approval_mode='policy_granted' requires "
                "session_kind='delegated_recovery'"
            )
        return AssistantModeAuthority(
            session_kind=session_kind,
            approval_mode=approval_mode,
            review=review,
        )

    def _parse_assistant_modes(
        self,
        assistant_modes_data: Any,
        tool_catalog: dict[str, AssistantToolDefinition],
    ) -> dict[str, AssistantModeDefinition]:
        if assistant_modes_data is None:
            return {}
        if not isinstance(assistant_modes_data, dict):
            raise DeploymentValidationError("assistant_modes must be a mapping")

        parsed: dict[str, AssistantModeDefinition] = {}
        for mode_id, raw_mode in assistant_modes_data.items():
            if not isinstance(mode_id, str) or not mode_id.strip():
                raise DeploymentValidationError(
                    "assistant_modes keys must be non-empty strings"
                )
            mode_id = mode_id.strip()
            field_path = f"assistant_modes.{mode_id}"
            if not isinstance(raw_mode, dict):
                raise DeploymentValidationError(f"{field_path} must be a mapping")

            enabled = raw_mode.get("enabled", True)
            if not isinstance(enabled, bool):
                raise DeploymentValidationError(f"{field_path}.enabled must be a boolean")
            description = self._parse_required_string(
                raw_mode.get("description"),
                f"{field_path}.description",
            )
            authority = self._parse_assistant_mode_authority(
                raw_mode.get("authority"),
                f"{field_path}.authority",
            )
            catalog_data = raw_mode.get("catalog")
            if not isinstance(catalog_data, dict):
                raise DeploymentValidationError(f"{field_path}.catalog must be a mapping")
            if "skills" not in catalog_data:
                raise DeploymentValidationError(f"{field_path}.catalog.skills is required")
            if "tools" not in catalog_data:
                raise DeploymentValidationError(f"{field_path}.catalog.tools is required")
            skills = self._parse_string_list(
                catalog_data.get("skills"),
                f"{field_path}.catalog.skills",
            )
            tools = self._parse_string_list(
                catalog_data.get("tools"),
                f"{field_path}.catalog.tools",
            )
            nodes = self._parse_string_list(
                catalog_data.get("nodes", []),
                f"{field_path}.catalog.nodes",
            )
            for tool_id in tools:
                if tool_id not in tool_catalog:
                    raise DeploymentValidationError(
                        f"{field_path}.catalog.tools references unknown tool '{tool_id}'"
                    )
                if (
                    tool_catalog[tool_id].effect == "composer_draft_mutating"
                    and mode_id != _COMPOSER_ASSISTANT_MODE_ID
                ):
                    raise DeploymentValidationError(
                        f"{field_path}.catalog.tools references composer draft "
                        f"mutating tool '{tool_id}', which is only allowed in "
                        f"assistant_modes.{_COMPOSER_ASSISTANT_MODE_ID}"
                    )

            parsed[mode_id] = AssistantModeDefinition(
                mode_id=mode_id,
                enabled=enabled,
                description=description,
                authority=authority,
                catalog=AssistantModeCatalog(
                    skills=skills,
                    tools=tools,
                    nodes=nodes,
                ),
            )

        self._validate_query_assistant_mode(parsed, tool_catalog)
        return parsed

    def _validate_query_assistant_mode(
        self,
        assistant_modes: dict[str, AssistantModeDefinition],
        tool_catalog: dict[str, AssistantToolDefinition],
    ) -> None:
        query_mode = assistant_modes.get(_QUERY_ASSISTANT_MODE_ID)
        if query_mode is None or not query_mode.enabled:
            return
        if query_mode.catalog.skills:
            raise DeploymentValidationError(
                "assistant_modes.query.catalog.skills must be empty"
            )
        if query_mode.catalog.nodes:
            raise DeploymentValidationError(
                "assistant_modes.query.catalog.nodes must be empty"
            )
        non_read_only = sorted(
            tool_id
            for tool_id in query_mode.catalog.tools
            if tool_catalog[tool_id].effect != "read_only"
        )
        if non_read_only:
            raise DeploymentValidationError(
                "assistant_modes.query.catalog.tools must contain only read_only tools: "
                f"{non_read_only}"
            )

    def _assistant_mode_to_raw(self, mode: AssistantModeDefinition) -> dict[str, Any]:
        return {
            "enabled": mode.enabled,
            "description": mode.description,
            "authority": {
                "session_kind": mode.authority.session_kind,
                "approval_mode": mode.authority.approval_mode,
                "review": mode.authority.review,
            },
            "catalog": {
                "skills": list(mode.catalog.skills),
                "tools": list(mode.catalog.tools),
                "nodes": list(mode.catalog.nodes),
            },
        }

    def _load_from_data_and_validate(self, data: dict[str, Any]) -> None:
        self.adapters = {}
        self.robot_config = None
        self.grippers = {}
        self.orchestrator = None
        self.launch = None
        self.pose_sources = {}
        self.simulation_control = None
        self.contact_policies = []
        self.authority_matrix = {}
        self.model_audit_policy = None
        self.model_endpoint_profile_registry = None
        self.model_endpoints = {}
        self.assistant = None
        self.assistant_modes = {}
        self.tool_catalog = {}
        self.prompt_templates = {}

        if not isinstance(data, dict):
            raise DeploymentValidationError("Deployment must be a YAML mapping")

        removed_fields = sorted(_REMOVED_COOPERATIVE_INTERRUPT_FIELDS.intersection(data))
        if removed_fields:
            raise DeploymentValidationError(
                "Cooperative interrupt fields were removed and must not be "
                f"present in deployment artifacts: {removed_fields}. Use the "
                "kernel-native interrupt model once it is available."
            )

        # Schema validation
        api_version = data.get("api_version")
        if api_version != EXPECTED_API_VERSION:
            raise DeploymentValidationError(
                f"Expected api_version '{EXPECTED_API_VERSION}', got '{api_version}'"
            )
        self.api_version = api_version

        self.schema_version = data.get("schema_version", "")
        if not isinstance(self.schema_version, str) or not self.schema_version.strip():
            raise DeploymentValidationError("schema_version is required")
        if not SEMVER_RE.match(self.schema_version):
            raise DeploymentValidationError(
                f"schema_version must match X.Y.Z, got '{self.schema_version}'"
            )

        self.program_profile = data.get("program_profile", "")
        if not isinstance(self.program_profile, str) or not self.program_profile.strip():
            raise DeploymentValidationError("program_profile is required")
        try:
            get_required_keys(self.program_profile)
        except ValueError as exc:
            raise DeploymentValidationError(str(exc)) from exc

        self.name = data.get("name", "")
        if not isinstance(self.name, str) or not self.name.strip():
            raise DeploymentValidationError("name is required")
        self.description = data.get("description", "")

        # Robot config
        robot_data = data.get("robot_config")
        if not robot_data:
            raise DeploymentValidationError("robot_config section is required")
        self.robot_config = self._parse_robot_config(robot_data)

        # Grippers (optional)
        grippers_data = data.get("grippers", {})
        self.grippers = self._parse_grippers(grippers_data)

        # Adapters
        adapters_data = data.get("adapters", {})
        required_adapter_slots = ("planning", "context", "execution")
        for slot in required_adapter_slots:
            if slot not in adapters_data:
                raise DeploymentValidationError(
                    f"adapters.{slot} section is required"
                )
            adapter_data = adapters_data[slot]
            if "adapter_id" not in adapter_data:
                raise DeploymentValidationError(
                    f"adapters.{slot}.adapter_id is required"
                )
            if "adapter_version" not in adapter_data:
                raise DeploymentValidationError(
                    f"adapters.{slot}.adapter_version is required"
                )
            self.adapters[slot] = AdapterConfig(
                adapter_id=adapter_data["adapter_id"],
                adapter_version=adapter_data["adapter_version"],
                config=adapter_data.get("config", {}),
                supported_profiles=adapter_data.get("supported_profiles", []),
                capability_keys=_validate_capability_keys(
                    adapter_data.get("capability_keys", []),
                    f"adapters.{slot}.capability_keys",
                ),
            )
        self._validate_adapter_capability_manifests()

        # Orchestrator
        orchestrator_data = data.get("orchestrator")
        if not orchestrator_data:
            raise DeploymentValidationError("orchestrator section is required")
        if "engine" not in orchestrator_data:
            raise DeploymentValidationError(
                "orchestrator.engine is required"
            )
        self.orchestrator = OrchestratorConfig(
            engine=orchestrator_data["engine"],
            config=orchestrator_data.get("config", {}),
        )

        # Launch
        launch_data = data.get("launch")
        if not launch_data:
            raise DeploymentValidationError("launch section is required")
        runner_data = launch_data.get("runner")
        if not runner_data:
            raise DeploymentValidationError("launch.runner section is required")
        if "module" not in runner_data:
            raise DeploymentValidationError(
                "launch.runner.module is required"
            )
        runner = RunnerConfig(
            module=runner_data["module"],
            args=runner_data.get("args", []),
        )

        # UI backend (optional)
        ui_backend: Optional[UiBackendConfig] = None
        ui_data = launch_data.get("ui_backend")
        if ui_data is not None:
            ui_backend = UiBackendConfig(
                module=ui_data.get("module", ""),
                args=ui_data.get("args", []),
                enabled=ui_data.get("enabled", True),
            )

        self.launch = LaunchConfig(runner=runner, ui_backend=ui_backend)

        # Object pose sources (optional, contract-only in this slice)
        self.pose_sources = self._parse_pose_sources(data.get("pose_sources"))

        # Simulator lifecycle/control surface (optional, contract-only in this slice)
        self.simulation_control = self._parse_simulation_control(
            data.get("simulation_control")
        )
        self.contact_policies = self._parse_contact_policies(
            data.get("contact_policies")
        )

        # Authority matrix (optional)
        authority_matrix_data = data.get("authority_matrix", {})
        self.authority_matrix = self._parse_authority_matrix(authority_matrix_data)

        # Model audit policy (optional)
        audit_data = data.get("model_audit_policy")
        if audit_data is not None:
            if not isinstance(audit_data, dict):
                raise DeploymentValidationError(
                    "model_audit_policy must be a mapping"
                )
            hash_algo = audit_data.get("hash_algorithm", "sha256")
            if hash_algo not in _ALLOWED_HASH_ALGORITHMS:
                allowed = ", ".join(sorted(_ALLOWED_HASH_ALGORITHMS))
                raise DeploymentValidationError(
                    f"model_audit_policy.hash_algorithm must be one of: {allowed}"
                )
            self.model_audit_policy = ModelAuditPolicy(
                store_full_prompts=bool(audit_data.get("store_full_prompts", False)),
                store_full_responses=bool(audit_data.get("store_full_responses", False)),
                hash_algorithm=hash_algo,
                redact_argument_values=bool(audit_data.get("redact_argument_values", True)),
            )

        # Model endpoints and assistant selection (optional)
        model_endpoint_profiles = self._load_model_endpoint_profile_registry(
            data.get("model_endpoint_profile_registry")
        )
        self.model_endpoints = self._parse_model_endpoints(
            data.get("model_endpoints", {}),
            model_endpoint_profiles,
        )
        self.assistant = self._parse_assistant_config(
            data.get("assistant"),
            self.model_endpoints,
        )
        assistant_modes_data = data.get("assistant_modes")
        if assistant_modes_data is not None and not isinstance(assistant_modes_data, dict):
            raise DeploymentValidationError("assistant_modes must be a mapping")
        self.tool_catalog = self._parse_tool_catalog(
            data.get("tool_catalog"),
            assistant_modes_present=bool(assistant_modes_data),
        )
        self.assistant_modes = self._parse_assistant_modes(
            assistant_modes_data,
            self.tool_catalog,
        )

        # Prompt templates (optional)
        templates_data = data.get("prompt_templates", {})
        if templates_data and not isinstance(templates_data, dict):
            raise DeploymentValidationError("prompt_templates must be a mapping")
        for tid, tmpl_data in (templates_data or {}).items():
            if not isinstance(tmpl_data, dict):
                raise DeploymentValidationError(
                    f"prompt_templates.{tid} must be a mapping"
                )
            if "template_text" not in tmpl_data:
                raise DeploymentValidationError(
                    f"prompt_templates.{tid}.template_text is required"
                )
            self.prompt_templates[tid] = PromptTemplate(
                template_id=tid,
                version=tmpl_data.get("version", "1"),
                template_text=tmpl_data["template_text"],
                output_schema=tmpl_data.get("output_schema"),
                response_tool=tmpl_data.get("response_tool"),
            )

    def validate_against_program(self, program: Any) -> None:
        """Cross-artifact validation against a Program.

        Raises DeploymentValidationError if:
        - program_profile doesn't match
        - robot_config.robot_id doesn't match program.robot.robot_id
        - robot_config.group_id doesn't match program.robot.group_id
        - any node's per-move max_tcp_linear_speed_mps exceeds the deployment
          global_max_tcp_linear_speed_mps (fail closed: an authored per-move cap
          above the global safety cap is rejected rather than silently clamped)
        """
        if self.program_profile != program.program_profile:
            raise DeploymentValidationError(
                f"program_profile mismatch: deployment has '{self.program_profile}', "
                f"program has '{program.program_profile}'"
            )

        if self.robot_config.robot_id != program.robot.robot_id:
            raise DeploymentValidationError(
                f"robot_id mismatch: deployment has '{self.robot_config.robot_id}', "
                f"program has '{program.robot.robot_id}'"
            )

        if self.robot_config.group_id != program.robot.group_id:
            raise DeploymentValidationError(
                f"group_id mismatch: deployment has '{self.robot_config.group_id}', "
                f"program has '{program.robot.group_id}'"
            )

        # Per-move TCP linear-speed caps must not exceed the deployment-wide
        # global cap. Fail closed: an authored per-move cap above the global
        # safety limit is a misconfiguration — the motion nodes would otherwise
        # silently clamp it via min(per-move, global) and run something other
        # than authored. Reject at load/save/apply with an actionable error.
        # Equality with the global cap is allowed. The runtime min() clamp in
        # the motion nodes stays as defense-in-depth for paths that bypass this
        # validation (e.g. runtime parameter overrides applied after load).
        global_cap = self.robot_config.global_max_tcp_linear_speed_mps
        for node_name, per_move_cap in program.collect_per_move_tcp_speed_caps():
            if per_move_cap > global_cap + 1e-9:
                raise DeploymentValidationError(
                    f"Node '{node_name}': max_tcp_linear_speed_mps={per_move_cap} m/s "
                    f"exceeds the deployment global cap "
                    f"global_max_tcp_linear_speed_mps={global_cap} m/s. Lower the "
                    f"per-move cap to <= {global_cap} m/s, or raise the global cap."
                )

    def validate_robot_control_joint_limits_against(
        self, drivable_joint_names: list[str]
    ) -> None:
        """Spec/430 §3.2 cross-validation: every key in
        `robot_config.robot_control_joint_limits` MUST name a drivable joint
        from the bound URDF's group. Caller is responsible for supplying
        the joint list (typically `kernel.get_joint_names()` once the
        kernel/URDF is loaded). Raises `DeploymentValidationError` with
        the offending name(s) on mismatch so typo'd limits surface
        rather than silently no-op.
        """
        if not self.robot_config or not self.robot_config.robot_control_joint_limits:
            return
        allowed = set(drivable_joint_names)
        unknown = [
            name for name in self.robot_config.robot_control_joint_limits.keys()
            if name not in allowed
        ]
        if unknown:
            allowed_sorted = sorted(allowed)
            raise DeploymentValidationError(
                "robot_config.robot_control_joint_limits references unknown "
                f"joint(s): {unknown}. Drivable joints for this robot are: "
                f"{allowed_sorted}"
            )

    def validate_capabilities(self, program: Any | None = None) -> None:
        """Verify adapter capabilities cover the program profile requirements.

        Raises DeploymentValidationError if:
        - Any adapter declares supported_profiles but doesn't include self.program_profile
        - The union of all adapters' capability_keys doesn't cover all required keys
        - Required keys are missing from the capability union
        - Program node/skill capability keys are missing from the capability union

        Missing capability declarations fail closed; supported backends must
        declare the capability keys they provide.
        """
        all_capability_keys = self.capability_keys()

        # Check that each adapter that declares supported_profiles includes our profile
        for slot, adapter in self.adapters.items():
            if adapter.supported_profiles:
                if self.program_profile not in adapter.supported_profiles:
                    raise DeploymentValidationError(
                        f"Adapter '{slot}' declares supported_profiles but doesn't "
                        f"include '{self.program_profile}': "
                        f"{adapter.supported_profiles}"
                    )

        # Get required keys for the program profile
        required_keys = get_required_keys(self.program_profile)

        if not all_capability_keys:
            raise DeploymentValidationError(
                f"Missing capability keys for profile '{self.program_profile}': "
                f"{sorted(required_keys)}"
            )

        # Compute missing keys
        missing = required_keys - all_capability_keys

        if missing:
            raise DeploymentValidationError(
                f"Missing capability keys for profile '{self.program_profile}': "
                f"{sorted(missing)}"
            )

        if program is not None:
            required_program_keys = set(program.required_capabilities())
            missing_program_keys = required_program_keys - all_capability_keys
            if missing_program_keys:
                raise DeploymentValidationError(
                    "Missing capability keys for program nodes: "
                    f"{sorted(missing_program_keys)}"
                )

    def capability_keys(self) -> set[str]:
        """Return all capability keys advertised by this deployment."""
        capability_keys: set[str] = set()
        for adapter in self.adapters.values():
            capability_keys.update(adapter.capability_keys)
        if self.simulation_control is not None:
            capability_keys.update(self.simulation_control.capability_keys())
        return capability_keys

    def baseline_collision_policy_snapshot(self) -> dict[str, Any] | None:
        """Return the deployment-baseline collision policy for kernel config."""
        if not self.contact_policies:
            return None
        policy = self.contact_policies[0]
        canonical = yaml.safe_dump(
            policy.raw,
            sort_keys=True,
            allow_unicode=False,
        )
        return {
            "policy_id": policy.policy_id,
            "revision_id": 1,
            "content_hash": hashlib.sha256(canonical.encode("utf-8")).hexdigest(),
            "require_full_diagnostics": bool(
                policy.raw.get("require_full_diagnostics", True)
            ),
        }

    def _validate_adapter_capability_manifests(self) -> None:
        grippers_configured = bool(self.grippers)
        for slot, adapter in self.adapters.items():
            manifest = get_adapter_capability_manifest(adapter.adapter_id)
            if manifest is None:
                continue
            declared = set(adapter.capability_keys)
            published = set(
                manifest.capability_keys_for(
                    grippers_configured=grippers_configured
                )
            )
            if declared != published:
                missing = sorted(published - declared)
                extra = sorted(declared - published)
                raise DeploymentValidationError(
                    f"adapters.{slot}.capability_keys do not match published "
                    f"capability manifest for '{adapter.adapter_id}': "
                    f"missing={missing}, extra={extra}"
                )

    def validate_assistant_modes(
        self,
        node_catalog: Any | None,
        skill_catalog: Any | None,
    ) -> None:
        """Validate assistant modes against the loaded node and skill catalogs."""
        if not self.assistant_modes:
            return
        if node_catalog is None:
            raise DeploymentValidationError(
                "assistant_modes validation requires the loaded node catalog"
            )
        if skill_catalog is None:
            raise DeploymentValidationError(
                "assistant_modes validation requires the loaded skill catalog"
            )

        node_ids = {entry.id for entry in getattr(node_catalog, "entries", [])}
        skill_ids = set(getattr(skill_catalog, "skill_ids", []))
        tool_ids = set(self.tool_catalog)

        tool_skill_collisions = sorted(tool_ids & skill_ids)
        tool_node_collisions = sorted(tool_ids & node_ids)
        skill_node_collisions = sorted(skill_ids & node_ids)
        if tool_skill_collisions or tool_node_collisions or skill_node_collisions:
            details: list[str] = []
            if tool_skill_collisions:
                details.append(f"tool/skill={tool_skill_collisions}")
            if tool_node_collisions:
                details.append(f"tool/node={tool_node_collisions}")
            if skill_node_collisions:
                details.append(f"skill/node={skill_node_collisions}")
            raise DeploymentValidationError(
                "assistant instrument IDs must be globally disjoint: "
                + ", ".join(details)
            )

        for mode_id, mode in self.assistant_modes.items():
            mode_tools = set(mode.catalog.tools)
            missing_skills = sorted(
                skill_id
                for skill_id in mode.catalog.skills
                if skill_id not in skill_ids
            )
            if missing_skills:
                raise DeploymentValidationError(
                    f"assistant_modes.{mode_id}.catalog.skills references unknown skills: "
                    f"{missing_skills}"
                )
            missing_nodes = sorted(
                node_id
                for node_id in mode.catalog.nodes
                if node_id not in node_ids
            )
            if missing_nodes:
                raise DeploymentValidationError(
                    f"assistant_modes.{mode_id}.catalog.nodes references unknown nodes: "
                    f"{missing_nodes}"
                )
            for skill_id in mode.catalog.skills:
                skill = skill_catalog.get(skill_id)
                if skill is None:
                    continue
                required_tools = set(getattr(skill, "requires_tools", []))
                missing_tools = sorted(required_tools - mode_tools)
                if missing_tools:
                    raise DeploymentValidationError(
                        f"assistant_modes.{mode_id}.catalog.skills includes skill "
                        f"'{skill_id}' without required tools: {missing_tools}"
                    )

        self._validate_query_assistant_mode(self.assistant_modes, self.tool_catalog)

    def resolve_assistant_mode_catalog(self, mode_id: str) -> dict[str, Any]:
        """Return the request-visible catalog for an enabled assistant mode."""
        mode = self.assistant_modes.get(mode_id)
        if mode is None:
            raise DeploymentValidationError(f"Unknown assistant mode: '{mode_id}'")
        if not mode.enabled:
            raise DeploymentValidationError(f"Assistant mode is disabled: '{mode_id}'")
        return {
            "modeId": mode.mode_id,
            "authority": {
                "sessionKind": mode.authority.session_kind,
                "approvalMode": mode.authority.approval_mode,
                "review": mode.authority.review,
            },
            "skills": list(mode.catalog.skills),
            "tools": [
                self._assistant_tool_to_raw(self.tool_catalog[tool_id])
                for tool_id in mode.catalog.tools
            ],
            "nodes": list(mode.catalog.nodes),
        }

    def to_dict(self) -> dict[str, Any]:
        """Serialize to dict matching manyforge.deployment.v0 schema."""
        d: dict[str, Any] = {
            "api_version": EXPECTED_API_VERSION,
            "schema_version": self.schema_version,
            "name": self.name,
            "description": self.description,
            "program_profile": self.program_profile,
        }
        if self.robot_config:
            d["robot_config"] = {
                "robot_id": self.robot_config.robot_id,
                "group_id": self.robot_config.group_id,
                "urdf_path": self.robot_config.urdf_path,
                "srdf_path": self.robot_config.srdf_path,
                "package_paths": self.robot_config.package_paths,
                "global_max_tcp_linear_speed_mps": self.robot_config.global_max_tcp_linear_speed_mps,
            }
            # Spec/430 §3.2 robot-control-joint-limits round-trip.
            # Joints where BOTH fields are unset are dropped from the
            # serialized form so the YAML stays minimal when the
            # operator hasn't customized anything; loaders treat the
            # absence as "engine defaults".
            limits_out: dict[str, dict[str, float]] = {}
            for joint_name, limit in self.robot_config.robot_control_joint_limits.items():
                entry: dict[str, float] = {}
                if limit.max_velocity_rad_s is not None:
                    entry["max_velocity_rad_s"] = float(limit.max_velocity_rad_s)
                if limit.max_acceleration_rad_s2 is not None:
                    entry["max_acceleration_rad_s2"] = float(limit.max_acceleration_rad_s2)
                if limit.max_jerk_rad_s3 is not None:
                    entry["max_jerk_rad_s3"] = float(limit.max_jerk_rad_s3)
                if entry:
                    limits_out[joint_name] = entry
            if limits_out:
                d["robot_config"]["robot_control_joint_limits"] = limits_out
        if self.grippers:
            d["grippers"] = {
                gripper_id: {
                    "command_mode": gripper.command_mode,
                    "group_id": gripper.group_id,
                    "joint_name": gripper.joint_name,
                    "motion_time_s": gripper.motion_time_s,
                    "description": gripper.description,
                    "touch_links": list(gripper.touch_links),
                    "mount_frame": gripper.mount_frame,
                    "attach_frame": gripper.attach_frame,
                    "tcp_frame": gripper.tcp_frame,
                    "auxiliary_frames": list(gripper.auxiliary_frames),
                }
                for gripper_id, gripper in self.grippers.items()
            }
        # adapters dict with slots: planning, context, execution
        d["adapters"] = {}
        for slot, adapter in self.adapters.items():
            adapter_d = {
                "adapter_id": adapter.adapter_id,
                "adapter_version": adapter.adapter_version,
            }
            if adapter.supported_profiles:
                adapter_d["supported_profiles"] = adapter.supported_profiles
            if adapter.capability_keys:
                adapter_d["capability_keys"] = adapter.capability_keys
            adapter_d["config"] = adapter.config
            d["adapters"][slot] = adapter_d
        # orchestrator
        if self.orchestrator:
            d["orchestrator"] = {
                "engine": self.orchestrator.engine,
                "config": self.orchestrator.config,
            }
        # launch
        if self.launch:
            launch_d: dict[str, Any] = {
                "runner": {
                    "module": self.launch.runner.module,
                    "args": self.launch.runner.args,
                }
            }
            if self.launch.ui_backend:
                launch_d["ui_backend"] = {
                    "module": self.launch.ui_backend.module,
                    "args": self.launch.ui_backend.args,
                    "enabled": self.launch.ui_backend.enabled,
                }
            d["launch"] = launch_d
        if self.model_audit_policy is not None:
            d["model_audit_policy"] = {
                "store_full_prompts": self.model_audit_policy.store_full_prompts,
                "store_full_responses": self.model_audit_policy.store_full_responses,
                "hash_algorithm": self.model_audit_policy.hash_algorithm,
                "redact_argument_values": self.model_audit_policy.redact_argument_values,
            }
        if self.model_endpoint_profile_registry:
            d["model_endpoint_profile_registry"] = (
                self.model_endpoint_profile_registry
            )
        if self.model_endpoints:
            d["model_endpoints"] = {
                alias: {
                    **(
                        {"profile_ref": endpoint.profile_ref}
                        if endpoint.profile_ref
                        else {}
                    ),
                    "provider": endpoint.provider,
                    "model_id": endpoint.model_id,
                    "base_url": endpoint.base_url,
                    "image_fetch_base_url": endpoint.image_fetch_base_url,
                    "api_key_env": endpoint.api_key_env,
                    "default_timeout_s": endpoint.default_timeout_s,
                    "max_tokens": endpoint.max_tokens,
                    "capabilities": {
                        "supports_response_tools": (
                            endpoint.capabilities.supports_response_tools
                        ),
                        "supports_json_object": (
                            endpoint.capabilities.supports_json_object
                        ),
                        "supports_image_input": (
                            endpoint.capabilities.supports_image_input
                        ),
                        "supported_image_transports": list(
                            endpoint.capabilities.supported_image_transports
                        ),
                        "tool_call_family": endpoint.capabilities.tool_call_family,
                        **(
                            {
                                "latency_budget_ms": (
                                    endpoint.capabilities.latency_budget_ms
                                )
                            }
                            if endpoint.capabilities.latency_budget_ms is not None
                            else {}
                        ),
                    },
                }
                for alias, endpoint in self.model_endpoints.items()
            }
        if self.assistant is not None:
            d["assistant"] = {
                "provider": self.assistant.provider,
                "endpoint_alias": self.assistant.endpoint_alias,
                "enabled": self.assistant.enabled,
            }
        if self.assistant_modes:
            d["assistant_modes"] = {
                mode_id: self._assistant_mode_to_raw(mode)
                for mode_id, mode in self.assistant_modes.items()
            }
        if self.tool_catalog:
            d["tool_catalog"] = [
                self._assistant_tool_to_raw(tool)
                for tool in self.tool_catalog.values()
            ]
        if self.prompt_templates:
            d["prompt_templates"] = {
                template_id: {
                    "version": template.version,
                    "template_text": template.template_text,
                    **(
                        {"output_schema": template.output_schema}
                        if template.output_schema is not None
                        else {}
                    ),
                    **(
                        {"response_tool": template.response_tool}
                        if template.response_tool is not None
                        else {}
                    ),
                }
                for template_id, template in self.prompt_templates.items()
            }
        if self.pose_sources:
            d["pose_sources"] = []
            for source in self.pose_sources.values():
                source_d: dict[str, Any] = {
                    "id": source.source_id,
                    "kind": source.kind,
                    "enabled": source.enabled,
                    "max_update_hz": source.max_update_hz,
                    "stale_after_s": source.stale_after_s,
                    "objects": {},
                }
                if source.frame_id:
                    source_d["frame_id"] = source.frame_id
                if source.service_prefix != "auto":
                    source_d["service_prefix"] = source.service_prefix
                if source.topic:
                    source_d["topic"] = source.topic
                if source.description:
                    source_d["description"] = source.description
                if source.measured_tcp is not None:
                    source_d["measured_tcp"] = {
                        "entity": source.measured_tcp.entity,
                        "tcp_frame_id": source.measured_tcp.tcp_frame_id,
                    }
                    if source.measured_tcp.frame_id:
                        source_d["measured_tcp"]["frame_id"] = (
                            source.measured_tcp.frame_id
                        )
                if source.measured_robot is not None:
                    source_d["measured_robot"] = {
                        "joint_state_topic": source.measured_robot.joint_state_topic,
                    }
                for object_id, binding in source.objects.items():
                    binding_d: dict[str, Any] = {
                        "apply_policy": binding.apply_policy,
                    }
                    if binding.entity:
                        binding_d["entity"] = binding.entity
                    if binding.frame_id:
                        binding_d["frame_id"] = binding.frame_id
                    if binding.confidence_min is not None:
                        binding_d["confidence_min"] = binding.confidence_min
                    if binding.measured_state_ghost is not None:
                        binding_d["measured_state_ghost"] = {
                            "enabled": binding.measured_state_ghost.enabled,
                            "robot_tcp": binding.measured_state_ghost.robot_tcp,
                            "object": binding.measured_state_ghost.object,
                            "opacity": binding.measured_state_ghost.opacity,
                        }
                    if binding.measured_attachment_baseline is not None:
                        binding_d["measured_attachment_baseline"] = {
                            "capture": binding.measured_attachment_baseline.capture,
                            "require_same_time_robot_tcp": (
                                binding.measured_attachment_baseline.require_same_time_robot_tcp
                            ),
                        }
                    if binding.attached_drift_monitor is not None:
                        drift = binding.attached_drift_monitor
                        drift_d: dict[str, Any] = {
                            "enabled": drift.enabled,
                            "debounce_s": drift.debounce_s,
                            "stale_behavior": drift.stale_behavior,
                            "action": drift.action,
                            "signal_id": drift.signal_id,
                            "sample_time_tolerance_s": drift.sample_time_tolerance_s,
                        }
                        if drift.translation_threshold_m is not None:
                            drift_d["translation_threshold_m"] = (
                                drift.translation_threshold_m
                            )
                        if drift.rotation_threshold_rad is not None:
                            drift_d["rotation_threshold_rad"] = (
                                drift.rotation_threshold_rad
                            )
                        binding_d["attached_drift_monitor"] = drift_d
                    if binding.metadata:
                        binding_d["metadata"] = dict(binding.metadata)
                    source_d["objects"][object_id] = binding_d
                d["pose_sources"].append(source_d)
        if self.simulation_control is not None:
            simulation_control_d: dict[str, Any] = {
                "kind": self.simulation_control.kind,
                "service_prefix": self.simulation_control.service_prefix,
                "capabilities": {
                    "reset": self.simulation_control.capabilities.reset,
                    "pause": self.simulation_control.capabilities.pause,
                    "play": self.simulation_control.capabilities.play,
                    "step": self.simulation_control.capabilities.step,
                    "set_entity_state": (
                        self.simulation_control.capabilities.set_entity_state
                    ),
                },
            }
            if self.simulation_control.capabilities.reset:
                simulation_control_d["reset"] = {
                    "default_scope": self.simulation_control.reset.default_scope,
                    "close_command_gate": (
                        self.simulation_control.reset.close_command_gate
                    ),
                    "wait_ready_s": self.simulation_control.reset.wait_ready_s,
                    "resume_play_after_reset": (
                        self.simulation_control.reset.resume_play_after_reset
                    ),
                }
            d["simulation_control"] = simulation_control_d
        if self.contact_policies:
            d["contact_policies"] = [dict(policy.raw) for policy in self.contact_policies]
        if self.authority_matrix:
            d["authority_matrix"] = {
                domain: {
                    "owner": config.owner,
                    "on_disconnect": config.on_disconnect,
                    "description": config.description,
                }
                for domain, config in self.authority_matrix.items()
            }
        return d

    def save(self, path: str | Path) -> None:
        """Write deployment to YAML file."""
        with open(path, "w") as f:
            yaml.dump(self.to_dict(), f, sort_keys=False, default_flow_style=False)

    def update_from_dict(self, data: dict[str, Any]) -> None:
        """Update fields from a dict. Re-validates after update."""
        removed_fields = sorted(_REMOVED_COOPERATIVE_INTERRUPT_FIELDS.intersection(data))
        if removed_fields:
            raise DeploymentValidationError(
                "Cooperative interrupt fields were removed and must not be "
                f"present in deployment artifacts: {removed_fields}. Use the "
                "kernel-native interrupt model once it is available."
            )
        if "name" in data:
            self.name = data["name"]
        if "description" in data:
            self.description = data["description"]
        if "program_profile" in data:
            self.program_profile = data["program_profile"]
        if "robot_config" in data:
            self.robot_config = self._parse_robot_config(
                data["robot_config"],
                current=self.robot_config,
            )
        if "grippers" in data:
            merged_grippers = {
                gripper_id: {
                    "command_mode": config.command_mode,
                    "group_id": config.group_id,
                    "joint_name": config.joint_name,
                    "motion_time_s": config.motion_time_s,
                    "description": config.description,
                    "touch_links": list(config.touch_links),
                }
                for gripper_id, config in self.grippers.items()
            }
            for gripper_id, gripper_data in data["grippers"].items():
                if not isinstance(gripper_data, dict):
                    raise DeploymentValidationError(
                        f"grippers.{gripper_id} must be a mapping"
                    )
                current = merged_grippers.get(gripper_id, {})
                merged_grippers[gripper_id] = {
                    "command_mode": gripper_data.get(
                        "command_mode", current.get("command_mode", "")
                    ),
                    "group_id": gripper_data.get("group_id", current.get("group_id", "")),
                    "joint_name": gripper_data.get(
                        "joint_name", current.get("joint_name", "")
                    ),
                    "motion_time_s": gripper_data.get(
                        "motion_time_s", current.get("motion_time_s", 0.0)
                    ),
                    "description": gripper_data.get(
                        "description", current.get("description", "")
                    ),
                    "touch_links": gripper_data.get(
                        "touch_links", current.get("touch_links", [])
                    ),
                    "mount_frame": gripper_data.get(
                        "mount_frame", current.get("mount_frame", "")
                    ),
                    "attach_frame": gripper_data.get(
                        "attach_frame", current.get("attach_frame", "")
                    ),
                    "tcp_frame": gripper_data.get(
                        "tcp_frame", current.get("tcp_frame", "")
                    ),
                    "auxiliary_frames": gripper_data.get(
                        "auxiliary_frames", current.get("auxiliary_frames", [])
                    ),
                }
            self.grippers = self._parse_grippers(merged_grippers)
        if "adapters" in data:
            adapters_data = data["adapters"]
            for slot, adapter_data in adapters_data.items():
                if slot in self.adapters:
                    self.adapters[slot] = AdapterConfig(
                        adapter_id=adapter_data.get("adapter_id", self.adapters[slot].adapter_id),
                        adapter_version=adapter_data.get("adapter_version", self.adapters[slot].adapter_version),
                        config=adapter_data.get("config", self.adapters[slot].config),
                        supported_profiles=adapter_data.get("supported_profiles", self.adapters[slot].supported_profiles),
                        capability_keys=adapter_data.get("capability_keys", self.adapters[slot].capability_keys),
                    )
        if "orchestrator" in data:
            oc = data["orchestrator"]
            self.orchestrator = OrchestratorConfig(
                engine=oc.get("engine", self.orchestrator.engine),
                config=oc.get("config", self.orchestrator.config),
            )
        if "launch" in data:
            lc = data["launch"]
            runner_data = lc.get("runner", {})
            runner = RunnerConfig(
                module=runner_data.get("module", self.launch.runner.module),
                args=runner_data.get("args", self.launch.runner.args),
            )
            ui_backend: Optional[UiBackendConfig] = None
            ui_data = lc.get("ui_backend")
            if ui_data is not None:
                ui_backend = UiBackendConfig(
                    module=ui_data.get("module", self.launch.ui_backend.module if self.launch.ui_backend else ""),
                    args=ui_data.get("args", self.launch.ui_backend.args if self.launch.ui_backend else []),
                    enabled=ui_data.get("enabled", self.launch.ui_backend.enabled if self.launch.ui_backend else True),
                )
            self.launch = LaunchConfig(runner=runner, ui_backend=ui_backend)
        if (
            "model_endpoint_profile_registry" in data
            or "model_endpoints" in data
        ):
            if "model_endpoint_profile_registry" in data:
                profile_registry = self._load_model_endpoint_profile_registry(
                    data.get("model_endpoint_profile_registry")
                )
            else:
                profile_registry = self._load_model_endpoint_profile_registry(
                    self.model_endpoint_profile_registry
                )
            endpoint_data = data.get("model_endpoints", self.to_dict().get("model_endpoints", {}))
            self.model_endpoints = self._parse_model_endpoints(
                endpoint_data,
                profile_registry,
            )
            if self.assistant is not None and "assistant" not in data:
                self.assistant = self._parse_assistant_config(
                    {
                        "provider": self.assistant.provider,
                        "endpoint_alias": self.assistant.endpoint_alias,
                        "enabled": self.assistant.enabled,
                    },
                    self.model_endpoints,
                )
        if "assistant" in data:
            self.assistant = self._parse_assistant_config(
                data["assistant"],
                self.model_endpoints,
            )
        if "tool_catalog" in data or "assistant_modes" in data:
            current_raw = self.to_dict()
            assistant_modes_data = data.get(
                "assistant_modes",
                current_raw.get("assistant_modes", {}),
            )
            if assistant_modes_data is not None and not isinstance(assistant_modes_data, dict):
                raise DeploymentValidationError("assistant_modes must be a mapping")
            tool_catalog_data = data.get(
                "tool_catalog",
                current_raw.get("tool_catalog"),
            )
            self.tool_catalog = self._parse_tool_catalog(
                tool_catalog_data,
                assistant_modes_present=bool(assistant_modes_data),
            )
            self.assistant_modes = self._parse_assistant_modes(
                assistant_modes_data,
                self.tool_catalog,
            )
        if "pose_sources" in data:
            self.pose_sources = self._parse_pose_sources(data["pose_sources"])
        if "simulation_control" in data:
            self.simulation_control = self._parse_simulation_control(
                data["simulation_control"]
            )
        if "contact_policies" in data:
            self.contact_policies = self._parse_contact_policies(
                data["contact_policies"]
            )
        if "authority_matrix" in data:
            merged_matrix = {
                domain: {
                    "owner": config.owner,
                    "on_disconnect": config.on_disconnect,
                    "description": config.description,
                }
                for domain, config in self.authority_matrix.items()
            }
            for domain, domain_data in data["authority_matrix"].items():
                if not isinstance(domain_data, dict):
                    raise DeploymentValidationError(
                        f"authority_matrix.{domain} must be a mapping"
                    )
                current = merged_matrix.get(domain, {})
                merged_matrix[domain] = {
                    "owner": domain_data.get("owner", current.get("owner", "")),
                    "on_disconnect": domain_data.get(
                        "on_disconnect",
                        current.get("on_disconnect", ""),
                    ),
                    "description": domain_data.get(
                        "description",
                        current.get("description", ""),
                    ),
                }
            self.authority_matrix = self._parse_authority_matrix(merged_matrix)
        self._validate_adapter_capability_manifests()
