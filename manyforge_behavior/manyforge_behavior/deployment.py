"""Deployment artifact loader and validator for ManyForge."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

import yaml

EXPECTED_API_VERSION = "manyforge.deployment.v0"


@dataclass
class RobotConfig:
    robot_id: str
    group_id: str
    urdf_path: str
    srdf_path: str = ""
    global_max_tcp_linear_speed_mps: float = 0.0


@dataclass
class AdapterConfig:
    adapter_id: str
    adapter_version: str
    config: dict[str, Any] = field(default_factory=dict)
    supported_profiles: list[str] = field(default_factory=list)
    capability_keys: list[str] = field(default_factory=list)


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


@dataclass
class LaunchConfig:
    runner: RunnerConfig
    ui_backend: Optional[UiBackendConfig] = None


class DeploymentValidationError(Exception):
    """Raised when deployment artifact validation fails."""
    pass


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
        self.adapters: dict[str, AdapterConfig] = {}
        self.orchestrator: Optional[OrchestratorConfig] = None
        self.launch: Optional[LaunchConfig] = None
        self._load_and_validate()

    def _load_and_validate(self) -> None:
        with open(self._path) as f:
            data = yaml.safe_load(f)

        if not isinstance(data, dict):
            raise DeploymentValidationError("Deployment must be a YAML mapping")

        # Schema validation
        api_version = data.get("api_version")
        if api_version != EXPECTED_API_VERSION:
            raise DeploymentValidationError(
                f"Expected api_version '{EXPECTED_API_VERSION}', got '{api_version}'"
            )
        self.api_version = api_version

        self.schema_version = data.get("schema_version", "")
        if not self.schema_version:
            raise DeploymentValidationError("schema_version is required")

        self.program_profile = data.get("program_profile", "")
        if not self.program_profile:
            raise DeploymentValidationError("program_profile is required")

        self.name = data.get("name", "")
        self.description = data.get("description", "")

        # Robot config
        robot_data = data.get("robot_config")
        if not robot_data:
            raise DeploymentValidationError("robot_config section is required")
        for field_name in ("robot_id", "group_id", "urdf_path"):
            if field_name not in robot_data:
                raise DeploymentValidationError(
                    f"robot_config.{field_name} is required"
                )
        tcp_speed = robot_data.get("global_max_tcp_linear_speed_mps", 0.0)
        if tcp_speed <= 0:
            raise DeploymentValidationError(
                "robot_config.global_max_tcp_linear_speed_mps must be > 0"
            )
        self.robot_config = RobotConfig(
            robot_id=robot_data["robot_id"],
            group_id=robot_data["group_id"],
            urdf_path=robot_data["urdf_path"],
            srdf_path=robot_data.get("srdf_path", ""),
            global_max_tcp_linear_speed_mps=tcp_speed,
        )

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
                capability_keys=adapter_data.get("capability_keys", []),
            )

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

    def validate_against_program(self, program: Any) -> None:
        """Cross-artifact validation against a Program.

        Raises DeploymentValidationError if:
        - program_profile doesn't match
        - robot_config.robot_id doesn't match program.robot.robot_id
        - robot_config.group_id doesn't match program.robot.group_id
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

    def validate_capabilities(self) -> None:
        """Verify adapter capabilities cover the program profile requirements.

        Raises DeploymentValidationError if:
        - Any adapter declares supported_profiles but doesn't include self.program_profile
        - The union of all adapters' capability_keys doesn't cover all required keys
        - Required keys are missing from the capability union

        Silently passes if no adapter declares capability_keys (backwards compat).
        """
        from manyforge_behavior.capabilities import get_required_keys

        # Collect all capability keys from all adapters (union)
        all_capability_keys: set[str] = set()
        for slot, adapter in self.adapters.items():
            all_capability_keys.update(adapter.capability_keys)

        # If no adapter declared capability_keys, skip validation (backwards compat)
        if not all_capability_keys:
            return

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

        # Compute missing keys
        missing = required_keys - all_capability_keys

        if missing:
            raise DeploymentValidationError(
                f"Missing capability keys for profile '{self.program_profile}': "
                f"{sorted(missing)}"
            )

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
                "global_max_tcp_linear_speed_mps": self.robot_config.global_max_tcp_linear_speed_mps,
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
        return d

    def save(self, path: str | Path) -> None:
        """Write deployment to YAML file."""
        with open(path, "w") as f:
            yaml.dump(self.to_dict(), f, sort_keys=False, default_flow_style=False)

    def update_from_dict(self, data: dict[str, Any]) -> None:
        """Update fields from a dict. Re-validates after update."""
        if "name" in data:
            self.name = data["name"]
        if "description" in data:
            self.description = data["description"]
        if "program_profile" in data:
            self.program_profile = data["program_profile"]
        if "robot_config" in data:
            rc = data["robot_config"]
            self.robot_config = RobotConfig(
                robot_id=rc.get("robot_id", self.robot_config.robot_id),
                group_id=rc.get("group_id", self.robot_config.group_id),
                urdf_path=rc.get("urdf_path", self.robot_config.urdf_path),
                srdf_path=rc.get("srdf_path", self.robot_config.srdf_path),
                global_max_tcp_linear_speed_mps=rc.get(
                    "global_max_tcp_linear_speed_mps",
                    self.robot_config.global_max_tcp_linear_speed_mps
                ),
            )
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
